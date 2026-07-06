/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Deferred lighting evaluation: Lighting is evaluated in a separate pass.
 *
 * Outputs shading parameter per pixel using a randomized set of BSDFs.
 * Some render-pass are written during this pass.
 */
#pragma once

#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)

#include "draw_curves_lib.glsl" /* IWYU pragma: export. For nodetree functions. */
#include "draw_view.bsl.hh"     /* IWYU pragma: export. For nodetree functions. */
#include "eevee_cryptomatte.bsl.hh"
#include "eevee_gbuffer_write.bsl.hh"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"
#include "eevee_thickness_lib.bsl.hh"

float4 closure_to_rgba(Closure /*cl*/)
{
  [[resource_table]] const eevee::Sampling &sampling = resource_table_get(eevee::Sampling);
  [[resource_table]] const UtilityTexture &util_tx = resource_table_get(UtilityTexture);
  float4 out_color;
  out_color.rgb = g_emission;
  out_color.a = saturate(1.0f - average(g_transmittance));

  /* Reset for the next closure tree. */
  float noise = util_tx.fetch(gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling.rng_1D_get(SAMPLING_CLOSURE));
  closure_weights_reset(closure_rand);

  return out_color;
}

namespace eevee {

struct SurfaceDeferred {
  [[legacy_info]] ShaderCreateInfo draw_view_culling;
  [[legacy_info]] ShaderCreateInfo eevee_geom_iface_info;
  /* NPR: custom screen-space material nodes (bevel, curvature, ...) sample scene depth directly.
   * Restores the `hiz_tx` binding that npr-port-5.1 had on the (now removed) legacy deferred base. */
  [[legacy_info]] ShaderCreateInfo eevee_hiz_data;

  /* Everything is stored inside a two layered target, one for each format. This is to fit the
   * limitation of the number of images we can bind on a single shader. */
  [[image(GBUF_CLOSURE_SLOT, write, UNORM_10_10_10_2)]] image2DArray gbuf_closure_img;
  [[image(GBUF_NORMAL_SLOT, write, UNORM_16_16)]] image2DArray gbuf_normal_img;
  /* Storage for additional infos that are shared across closures. */
  [[image(GBUF_HEADER_SLOT, write, UINT_32)]] uimage2DArray gbuf_header_img;

  void write_closure_data(int2 texel, int layer, float4 data)
  {
    /* NOTE: The image view start at layer GBUF_CLOSURE_FB_LAYER_COUNT so all destination layer is
     * `layer - GBUF_CLOSURE_FB_LAYER_COUNT`. */
    imageStoreFast(gbuf_closure_img, int3(texel, layer - GBUF_CLOSURE_FB_LAYER_COUNT), data);
  }

  void write_normal_data(int2 texel, int layer, float2 data)
  {
    /* NOTE: The image view start at layer GBUF_NORMAL_FB_LAYER_COUNT so all destination layer is
     * `layer - GBUF_NORMAL_FB_LAYER_COUNT`. */
    imageStoreFast(gbuf_normal_img, int3(texel, layer - GBUF_NORMAL_FB_LAYER_COUNT), data.xyyy);
  }

  void write_header_data(int2 texel, int layer, uint data)
  {
    /* NOTE: The image view start at layer GBUF_HEADER_FB_LAYER_COUNT so all destination layer is
     * `layer - GBUF_HEADER_FB_LAYER_COUNT`. */
    imageStoreFast(gbuf_header_img, int3(texel, layer - GBUF_HEADER_FB_LAYER_COUNT), uint4(data));
  }
};

struct DeferredFragOut {
  /* Direct output. (Emissive, Holdout) */
  [[frag_color(0)]] float4 radiance;

  [[frag_color(1), raster_order_group(DEFERRED_GBUFFER_ROG_ID)]] uint gbuf_header;
  [[frag_color(2)]] float2 gbuf_normal;
  [[frag_color(3)]] float4 gbuf_closure1;
  [[frag_color(4)]] float4 gbuf_closure2;
};

DeferredFragOut surf_deferred_impl([[resource_table]] PipelineConstants &pipe,
                                   [[resource_table]] SurfaceDeferred &srt,
                                   [[resource_table]] gbuffer::PackParameters &gbuf_params,
                                   [[resource_table]] RenderPassOutput &render_passes,
                                   [[resource_table]] CryptomatteOutput &cryptomatte,
                                   [[resource_table]] const draw::Infos &infos,
                                   [[resource_table]] const draw::View &views,
                                   [[resource_table]] const Uniform &uni,
                                   [[resource_table]] const Sampling &sampling,
                                   [[resource_table]] const UtilityTexture &util_tx,
                                   const float4 frag_co,
                                   const bool front_face)
{
  DeferredFragOut frag_out = {};
  auto &interp_flat = interface_get(eevee_geom_iface_info, interp_flat);
  draw::ID id{interp_flat.resource_id_raw};
  const uint resource_id = id.resource_id<1>();

  const ViewMatrices view = views.get(0);

  init_globals(uni, view, front_face);

  float noise = util_tx.fetch(frag_co.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling.rng_1D_get(SAMPLING_CLOSURE));

#ifdef MAT_DEPTH_OFFSET
  float depth_offset = nodetree_depth_offset();
  if (!material_depth_offset_fragment_matches_prepass(depth_offset)) {
    gpu_discard_fragment();
    return frag_out;
  }
#endif

#ifdef MAT_DEPTH_OFFSET_NO_LIGHTING
  bool use_surface_depth = !material_depth_offset_is_zero(depth_offset);
  float surface_depth = use_surface_depth ? reverse_z::read(frag_co.z) : 0.0f;
#else
  constexpr bool use_surface_depth = false;
  float surface_depth = 0.0f;
#endif

#ifdef MAT_DEPTH_OFFSET
  material_depth_offset_write(depth_offset);
  material_depth_offset_apply_nodetree_position(depth_offset);
#endif

  fragment_displacement();

  nodetree_surface(closure_rand);

  g_holdout = saturate(g_holdout);

  Thickness thickness = Thickness::from(nodetree_thickness(), thickness_mode);

  /** Transparency weight is already applied through dithering, remove it from other closures. */
  float alpha = 1.0f - average(g_transmittance);
  float alpha_rcp = safe_rcp(alpha);

  /* Object holdout. */
  eObjectInfoFlag ob_flag = object_infos_get().flag;
  if (flag_test(ob_flag, OBJECT_HOLDOUT)) {
    /* alpha is set from rejected pixels / dithering. */
    g_holdout = 1.0f;

    /* Set alpha to 0.0 so that lighting is not computed. */
    alpha_rcp = 0.0f;
  }

  g_emission *= alpha_rcp;

  int2 out_texel = int2(frag_co.xy);

  ObjectInfos object_infos = infos.get(resource_id);
  /* NPR: `world_environment_disabled` must force object-id storage so the deferred light eval can
   * read the flag back and suppress environment lighting for the excluded object. */
  bool use_light_linking = receiver_light_set_get(object_infos) != 0 ||
                           world_environment_disabled_get(object_infos);
  bool use_terminator_offset = object_infos.shadow_terminator_normal_offset > 0.0;

  /* ----- Render Passes output ----- */

  /* Some render pass can be written during the gbuffer pass. Light passes are written later. */
  {
    const auto &nt = buffer_get(eevee_nodetree, node_tree);
    cryptomatte.store(out_texel, nt.crypto_hash, resource_id);
    render_passes.store_color(
        out_texel, uni.uniform_buf.render_pass.emission_id, float4(g_emission, 1.0f));
  }

  /* ----- GBuffer output ----- */

  gbuffer::InputClosures gbuf_data;
  /* Make sure we we do not read uninitialized data (see #159161). */
  if (pipe.closure_bin_count == 0) [[static_branch]] {
    gbuf_data.closure[0] = ClosureUndetermined{};
  }
  for (int i = 0; i < 3; i++) [[unroll]] {
    if (pipe.closure_bin_count > i) [[static_branch]] {
      gbuf_data.closure[i] = g_closure_get_resolved(i, alpha_rcp);
    }
  }
  const bool use_object_id = pipe.use_sss || use_light_linking || use_terminator_offset;

  float3 gbuffer_dither = sampling.rng_3D_get(SAMPLING_GBUFFER_U);
  gbuffer::Packed gbuf = gbuffer::pack(gbuf_params,
                                       gbuf_data,
                                       g_data.Ng,
                                       g_data.N,
                                       thickness,
                                       use_object_id,
                                       use_surface_depth,
                                       surface_depth);

  /* Output header and first closure using frame-buffer attachment. */
  frag_out.gbuf_header = gbuf.header;
  frag_out.gbuf_closure1 = gbuffer::closure_data_layer_dither_round_to_nearest(
      gbuf.closure[0], frag_co.xy, 0u, gbuffer_dither);
  frag_out.gbuf_closure2 = gbuffer::closure_data_layer_dither_round_to_nearest(
      gbuf.closure[1], frag_co.xy, 1u, gbuffer_dither);
  frag_out.gbuf_normal = gbuf.normal[0];

  /* Output remaining closures using image store. */
  if (gbuf_params.gbuffer_layer_max >= 2) [[static_branch]] {
    if (!gbuf_params.gbuffer_simple_layout) [[static_branch]] {
      if (flag_test(gbuf.used_layers, CLOSURE_DATA_2)) {
        srt.write_closure_data(out_texel,
                               2,
                               gbuffer::closure_data_layer_dither_flush_to_zero(
                                   gbuf.closure[2], frag_co.xy, 2u, gbuffer_dither));
      }
      if (flag_test(gbuf.used_layers, CLOSURE_DATA_3)) {
        srt.write_closure_data(out_texel,
                               3,
                               gbuffer::closure_data_layer_dither_flush_to_zero(
                                   gbuf.closure[3], frag_co.xy, 3u, gbuffer_dither));
      }
    }
    if (flag_test(gbuf.used_layers, NORMAL_DATA_1)) {
      srt.write_normal_data(out_texel, 1, gbuf.normal[1]);
    }
  }
  if (gbuf_params.gbuffer_layer_max >= 3) [[static_branch]] {
    if (flag_test(gbuf.used_layers, CLOSURE_DATA_4)) {
      srt.write_closure_data(out_texel,
                             4,
                             gbuffer::closure_data_layer_dither_flush_to_zero(
                                 gbuf.closure[4], frag_co.xy, 4u, gbuffer_dither));
    }
    if (flag_test(gbuf.used_layers, CLOSURE_DATA_5)) {
      srt.write_closure_data(out_texel,
                             5,
                             gbuffer::closure_data_layer_dither_flush_to_zero(
                                 gbuf.closure[5], frag_co.xy, 5u, gbuffer_dither));
    }
    if (flag_test(gbuf.used_layers, NORMAL_DATA_2)) {
      srt.write_normal_data(out_texel, 2, gbuf.normal[2]);
    }
  }

#if defined(GBUFFER_HAS_REFRACTION) || defined(GBUFFER_HAS_SUBSURFACE) || \
    defined(GBUFFER_HAS_TRANSLUCENT) || defined(MAT_DEPTH_OFFSET_NO_LIGHTING)
  if (flag_test(gbuf.used_layers, ADDITIONAL_DATA)) {
    srt.write_normal_data(
        out_texel, uni.pipeline_buf.gbuffer_additional_data_layer_id, gbuf.additional_info);
  }
#endif

  if (flag_test(gbuf.used_layers, OBJECT_ID)) {
    srt.write_header_data(out_texel, 1, resource_id);
  }

  /* ----- Radiance output ----- */

  /* Only output emission during the gbuffer pass. */
  frag_out.radiance = float4(g_emission, 0.0f);
  frag_out.radiance.rgb *= 1.0f - g_holdout;
  frag_out.radiance.a = g_holdout;

  return frag_out;
}

/* NOTE: This removes the possibility of using gl_FragDepth. Depth Offset materials use the
 * sibling entry-points below so they can keep late depth tests and write gl_FragDepth. */
[[fragment]] [[early_fragment_tests]]
void surf_deferred([[resource_table]] PipelineConstants &pipe,
                   [[resource_table]] SurfaceDeferred &srt,
                   [[resource_table]] gbuffer::PackParameters &gbuf_params,
                   [[resource_table]] RenderPassOutput &render_passes,
                   [[resource_table]] CryptomatteOutput &cryptomatte,
                   [[resource_table]] const draw::Infos &infos,
                   [[resource_table]] const draw::View &views,
                   [[resource_table]] const Uniform &uni,
                   [[resource_table]] const Sampling &sampling,
                   [[resource_table]] const UtilityTexture &util_tx,
                   [[frag_coord]] const float4 frag_co,
                   [[out]] DeferredFragOut &frag_out,
                   [[front_facing]] const bool front_face)
{
  DeferredFragOut result = surf_deferred_impl(pipe,
                                              srt,
                                              gbuf_params,
                                              render_passes,
                                              cryptomatte,
                                              infos,
                                              views,
                                              uni,
                                              sampling,
                                              util_tx,
                                              frag_co,
                                              front_face);
  frag_out.radiance = result.radiance;
  frag_out.gbuf_header = result.gbuf_header;
  frag_out.gbuf_normal = result.gbuf_normal;
  frag_out.gbuf_closure1 = result.gbuf_closure1;
  frag_out.gbuf_closure2 = result.gbuf_closure2;
}

[[fragment]]
void surf_deferred_depth_offset([[resource_table]] PipelineConstants &pipe,
                                [[resource_table]] SurfaceDeferred &srt,
                                [[resource_table]] gbuffer::PackParameters &gbuf_params,
                                [[resource_table]] RenderPassOutput &render_passes,
                                [[resource_table]] CryptomatteOutput &cryptomatte,
                                [[resource_table]] const draw::Infos &infos,
                                [[resource_table]] const draw::View &views,
                                [[resource_table]] const Uniform &uni,
                                [[resource_table]] const Sampling &sampling,
                                [[resource_table]] const UtilityTexture &util_tx,
                                [[frag_coord]] const float4 frag_co,
                                [[out]] DeferredFragOut &frag_out,
                                [[front_facing]] const bool front_face)
{
  DeferredFragOut result = surf_deferred_impl(pipe,
                                              srt,
                                              gbuf_params,
                                              render_passes,
                                              cryptomatte,
                                              infos,
                                              views,
                                              uni,
                                              sampling,
                                              util_tx,
                                              frag_co,
                                              front_face);
  frag_out.radiance = result.radiance;
  frag_out.gbuf_header = result.gbuf_header;
  frag_out.gbuf_normal = result.gbuf_normal;
  frag_out.gbuf_closure1 = result.gbuf_closure1;
  frag_out.gbuf_closure2 = result.gbuf_closure2;
}

/* Deferred materials only expose LightprobeRenderData when the material graph actually needs it. */
[[fragment]] [[early_fragment_tests]]
void surf_deferred_lightprobe([[resource_table]] PipelineConstants &pipe,
                              [[resource_table]] SurfaceDeferred &srt,
                              [[resource_table]] gbuffer::PackParameters &gbuf_params,
                              [[resource_table]] eevee::LightprobeRenderData & /*lightprobes*/,
                              [[resource_table]] RenderPassOutput &render_passes,
                              [[resource_table]] CryptomatteOutput &cryptomatte,
                              [[resource_table]] const draw::Infos &infos,
                              [[resource_table]] const draw::View &views,
                              [[resource_table]] const Uniform &uni,
                              [[resource_table]] const Sampling &sampling,
                              [[resource_table]] const UtilityTexture &util_tx,
                              [[frag_coord]] const float4 frag_co,
                              [[out]] DeferredFragOut &frag_out,
                              [[front_facing]] const bool front_face)
{
  DeferredFragOut result = surf_deferred_impl(pipe,
                                              srt,
                                              gbuf_params,
                                              render_passes,
                                              cryptomatte,
                                              infos,
                                              views,
                                              uni,
                                              sampling,
                                              util_tx,
                                              frag_co,
                                              front_face);
  frag_out.radiance = result.radiance;
  frag_out.gbuf_header = result.gbuf_header;
  frag_out.gbuf_normal = result.gbuf_normal;
  frag_out.gbuf_closure1 = result.gbuf_closure1;
  frag_out.gbuf_closure2 = result.gbuf_closure2;
}

[[fragment]]
void surf_deferred_lightprobe_depth_offset(
    [[resource_table]] PipelineConstants &pipe,
    [[resource_table]] SurfaceDeferred &srt,
    [[resource_table]] gbuffer::PackParameters &gbuf_params,
    [[resource_table]] eevee::LightprobeRenderData & /*lightprobes*/,
    [[resource_table]] RenderPassOutput &render_passes,
    [[resource_table]] CryptomatteOutput &cryptomatte,
    [[resource_table]] const draw::Infos &infos,
    [[resource_table]] const draw::View &views,
    [[resource_table]] const Uniform &uni,
    [[resource_table]] const Sampling &sampling,
    [[resource_table]] const UtilityTexture &util_tx,
    [[frag_coord]] const float4 frag_co,
    [[out]] DeferredFragOut &frag_out,
    [[front_facing]] const bool front_face)
{
  DeferredFragOut result = surf_deferred_impl(pipe,
                                              srt,
                                              gbuf_params,
                                              render_passes,
                                              cryptomatte,
                                              infos,
                                              views,
                                              uni,
                                              sampling,
                                              util_tx,
                                              frag_co,
                                              front_face);
  frag_out.radiance = result.radiance;
  frag_out.gbuf_header = result.gbuf_header;
  frag_out.gbuf_normal = result.gbuf_normal;
  frag_out.gbuf_closure1 = result.gbuf_closure1;
  frag_out.gbuf_closure2 = result.gbuf_closure2;
}

}  // namespace eevee
