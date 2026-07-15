/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Forward lighting evaluation: Lighting is evaluated during the geometry rasterization.
 *
 * This is used by alpha blended materials and materials using Shader to RGB nodes.
 */
#pragma once

#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)
FRAGMENT_SHADER_CREATE_INFO(eevee_geom_iface_info)

#include "draw_curves_lib.glsl" /* IWYU pragma: export. For nodetree functions. */
#include "draw_view.bsl.hh"
#include "eevee_forward_lib.bsl.hh"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"
#include "eevee_volume_lib.bsl.hh"

/* Global thickness because it is needed for closure_to_rgba. */
Thickness g_thickness_forward;
float3 g_forward_surface_lighting_P;

float4 closure_to_rgba_forward(Closure /*cl_unused*/)
{
  [[resource_table]] const draw::View &views = resource_table_get(draw::View);
  [[resource_table]] const eevee::Sampling &sampling = resource_table_get(eevee::Sampling);
  [[resource_table]] const UtilityTexture &util_tx = resource_table_get(UtilityTexture);
  auto &interp_flat = interface_get(eevee_geom_iface_info, interp_flat);
  draw::ID id{interp_flat.resource_id_raw};
  const uint resource_id = id.resource_id<1>();

  const float2 frag_co = gl_FragCoord.xy;

  float3 radiance, transmittance;
  float3 nodetree_P = g_data.P;
  g_data.P = g_forward_surface_lighting_P;
  eevee::forward_lighting_eval(
      views.get(0), resource_id, g_thickness_forward, frag_co, radiance, transmittance);
  g_data.P = nodetree_P;

  /* Reset for the next closure tree. */
  float noise = util_tx.fetch(frag_co, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling.rng_1D_get(SAMPLING_CLOSURE));
  closure_weights_reset(closure_rand);

#if defined(MAT_TRANSPARENT) && defined(MAT_SHADER_TO_RGBA)
  { /* Limit resource guard to this scope. */
    /* Multi-line macro breaks error line counting. */
    /* clang-format off */
    [[resource_table]] eevee::LightprobeRenderData &lightprobes = resource_table_get(eevee::LightprobeRenderData);
    /* clang-format on */
    [[resource_table]] eevee::LightprobeSphereRenderData &lp_spheres = lightprobes.spheres;

    float3 V = -views.get(0).world_incident_vector(g_forward_surface_lighting_P);
    eevee::LightProbeSample samp = lightprobes.load(
        frag_co, g_forward_surface_lighting_P, g_data.N, V);
    float3 radiance_behind = lp_spheres.spherical_sample_normalized_with_parallax(
        samp, g_forward_surface_lighting_P, V, 0.0);

#  ifndef MAT_FIRST_LAYER
    { /* Limit resource guard to this scope. */
      /* Multi-line macro breaks error line counting. */
      /* clang-format off */
      [[resource_table]] const eevee::PreviousLayerHiZ &prev_hiz = resource_table_get(eevee::PreviousLayerHiZ);
      [[resource_table]] const eevee::PreviousLayerRadiance &prev_radiance = resource_table_get(eevee::PreviousLayerRadiance);
      /* clang-format on */

      int2 texel = int2(frag_co);
      if (texelFetchExtend(prev_hiz.hiz_prev_tx, texel, 0).x != 1.0f) {
        radiance_behind = texelFetch(prev_radiance.previous_layer_radiance_tx, texel, 0).xyz;
      }
    }
#  endif

    radiance += radiance_behind * saturate(transmittance);
  }
#endif

  return float4(radiance, saturate(1.0f - average(transmittance)));
}

namespace eevee {

struct SurfaceForward {
  [[legacy_info]] ShaderCreateInfo eevee_geom_iface_info;

  [[legacy_info]] ShaderCreateInfo draw_view_culling;

  /* Optionally added depending on the material. */
  // [[legacy_info]] ShaderCreateInfo eevee_hiz_prev_data;
  // [[legacy_info]] ShaderCreateInfo eevee_previous_layer_radiance;
};

struct SurfaceForwardFragOut {
  /* Splitting RGB components into different target to overcome the lack of dual source blending
   * with multiple render targets. */
  [[frag_color(0)]] float4 combined_r;
  [[frag_color(1)]] float4 combined_g;
  [[frag_color(2)]] float4 combined_b;
  [[frag_color(3)]] float4 combined_a;
};

SurfaceForwardFragOut surf_forward_impl(
    [[resource_table]] PipelineConstants & /*pipe*/,
    [[resource_table]] SurfaceForward & /*srt*/,
    [[resource_table]] LightEvalIterator & /*lights*/,
    [[resource_table]] eevee::LightprobeRenderData & /*lightprobes*/,
    [[resource_table]] eevee::LightprobePlaneRenderData & /*lightprobe_planes*/,
    [[resource_table]] const draw::View &views,
    [[resource_table]] const draw::Model & /*models*/,
    [[resource_table]] const draw::Infos & /*infos*/,
    [[resource_table]] const UnifiedVolumeData &volumes,
    [[resource_table]] const Uniform &uni,
    [[resource_table]] const Sampling &sampling,
    [[resource_table]] const UtilityTexture &util_tx,
    const float4 frag_co,
    const bool front_face)
{
  SurfaceForwardFragOut frag_out = {};
  auto &interp_flat = interface_get(eevee_geom_iface_info, interp_flat);
  draw::ID id{interp_flat.resource_id_raw};
  const uint resource_id = id.resource_id<1>();

  const ViewMatrices view = views.get(0);

  init_globals(uni, view, front_face);
  g_forward_surface_lighting_P = g_data.P;

  float noise = util_tx.fetch(gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling.rng_1D_get(SAMPLING_CLOSURE));

#ifdef MAT_DEPTH_OFFSET
  float depth_offset = nodetree_depth_offset();
  float volume_depth = reverse_z::read(frag_co.z);
  if (!material_depth_offset_is_zero(depth_offset)) {
    volume_depth = material_depth_offset_screen_depth(depth_offset);
    float3 depth_offset_P = material_depth_offset_apply_nodetree_position(depth_offset);
#  ifndef MAT_DEPTH_OFFSET_NO_LIGHTING
    g_forward_surface_lighting_P = depth_offset_P;
#  endif
  }
  material_depth_offset_write(depth_offset);
#else
  float volume_depth = reverse_z::read(frag_co.z);
#endif

  fragment_displacement();

  g_thickness_forward = Thickness::from(nodetree_thickness(), thickness_mode);

  nodetree_surface(closure_rand);

  /* NPR: Forward/blended materials must still feed the Normal render pass. The deferred path writes
   * it from the GBuffer during the combine stage, but forward surfaces are never gbuffered, so the
   * pass would stay black without this explicit write (matches npr-port-5.1 forward behavior).
   * Forward materials always get the raw `eevee_render_pass_out` create-info (`rp_color_img` +
   * MAT_RENDER_PASS_SUPPORT), so write to that bound image directly. The BSL RenderPassOutput
   * resource-table struct is only present for AOV materials, so it cannot be used here. */
#if defined(MAT_RENDER_PASS_SUPPORT)
  if (uni.uniform_buf.render_pass.normal_id >= 0) {
    imageStoreFast(rp_color_img,
                   int3(int2(gl_FragCoord.xy), uni.uniform_buf.render_pass.normal_id),
                   float4(g_data.N, 1.0f));
  }
#endif

  float3 radiance, transmittance;
  float3 nodetree_P = g_data.P;
  g_data.P = g_forward_surface_lighting_P;
  eevee::forward_lighting_eval(
      view, resource_id, g_thickness_forward, gl_FragCoord.xy, radiance, transmittance);
  g_data.P = nodetree_P;

  /* Volumetric resolve and compositing. */
  float2 uvs = gl_FragCoord.xy * uni.uniform_buf.volumes.main_view_extent_inv;
  VolumeResolveSample vol = volumes.resolve(float3(uvs, volume_depth));
  /* Removes the part of the volume scattering that has
   * already been added to the destination pixels by the opaque resolve.
   * Since we do that using the blending pipeline we need to account for material transmittance. */
  vol.scattering -= vol.scattering * g_transmittance;
  radiance = radiance * vol.transmittance + vol.scattering;

  eObjectInfoFlag ob_flag = object_infos_get().flag;
  if (flag_test(ob_flag, OBJECT_HOLDOUT)) {
    g_holdout = 1.0f - average(g_transmittance);
    radiance *= 0.0f;
  }

  g_holdout = saturate(g_holdout);

  radiance *= 1.0f - g_holdout;

  /* There can be 2 frame-buffer layout for forward transparency:
   * - Combined RGB radiance with Monochromatic transmittance.
   * - Channel split RGB radiance & RGB transmittance + Dedicated average alpha with holdout. */
  if (uni.pipeline_buf.use_monochromatic_transmittance) {
    frag_out.combined_r = float4(radiance.rgb, transmittance.r);
  }
  else {
    frag_out.combined_r = float4(radiance.r, 0.0f, 0.0f, transmittance.r);
    frag_out.combined_g = float4(radiance.g, 0.0f, 0.0f, transmittance.g);
    frag_out.combined_b = float4(radiance.b, 0.0f, 0.0f, transmittance.b);
    frag_out.combined_a = float4(g_holdout, 0.0f, 0.0f, average(transmittance));
  }
  return frag_out;
}

/* Early fragment test is needed for render pass support for forward surfaces.
 * NOTE: This removes the possibility of using gl_FragDepth. Depth Offset materials use the
 * sibling entry-point below so they can keep late depth tests and write gl_FragDepth. */
[[fragment]] [[early_fragment_tests]]
void surf_forward([[resource_table]] PipelineConstants &pipe,
                  [[resource_table]] SurfaceForward &srt,
                  [[resource_table]] LightEvalIterator &lights,
                  [[resource_table]] eevee::LightprobeRenderData &lightprobes,
                  [[resource_table]] eevee::LightprobePlaneRenderData &lightprobe_planes,
                  [[resource_table]] const draw::View &views,
                  [[resource_table]] const draw::Model &models,
                  [[resource_table]] const draw::Infos &infos,
                  [[resource_table]] const UnifiedVolumeData &volumes,
                  [[resource_table]] const Uniform &uni,
                  [[resource_table]] const Sampling &sampling,
                  [[resource_table]] const UtilityTexture &util_tx,
                  [[frag_coord]] const float4 frag_co,
                  [[out]] SurfaceForwardFragOut &frag_out,
                  [[front_facing]] const bool front_face)
{
  SurfaceForwardFragOut result = surf_forward_impl(pipe,
                                                   srt,
                                                   lights,
                                                   lightprobes,
                                                   lightprobe_planes,
                                                   views,
                                                   models,
                                                   infos,
                                                   volumes,
                                                   uni,
                                                   sampling,
                                                   util_tx,
                                                   frag_co,
                                                   front_face);
  frag_out.combined_r = result.combined_r;
  frag_out.combined_g = result.combined_g;
  frag_out.combined_b = result.combined_b;
  frag_out.combined_a = result.combined_a;
}

[[fragment]]
void surf_forward_depth_offset([[resource_table]] PipelineConstants &pipe,
                               [[resource_table]] SurfaceForward &srt,
                               [[resource_table]] LightEvalIterator &lights,
                               [[resource_table]] eevee::LightprobeRenderData &lightprobes,
                               [[resource_table]] eevee::LightprobePlaneRenderData &lightprobe_planes,
                               [[resource_table]] const draw::View &views,
                               [[resource_table]] const draw::Model &models,
                               [[resource_table]] const draw::Infos &infos,
                               [[resource_table]] const UnifiedVolumeData &volumes,
                               [[resource_table]] const Uniform &uni,
                               [[resource_table]] const Sampling &sampling,
                               [[resource_table]] const UtilityTexture &util_tx,
                               [[frag_coord]] const float4 frag_co,
                               [[out]] SurfaceForwardFragOut &frag_out,
                               [[front_facing]] const bool front_face)
{
  SurfaceForwardFragOut result = surf_forward_impl(pipe,
                                                   srt,
                                                   lights,
                                                   lightprobes,
                                                   lightprobe_planes,
                                                   views,
                                                   models,
                                                   infos,
                                                   volumes,
                                                   uni,
                                                   sampling,
                                                   util_tx,
                                                   frag_co,
                                                   front_face);
  frag_out.combined_r = result.combined_r;
  frag_out.combined_g = result.combined_g;
  frag_out.combined_b = result.combined_b;
  frag_out.combined_a = result.combined_a;
}

}  // namespace eevee
