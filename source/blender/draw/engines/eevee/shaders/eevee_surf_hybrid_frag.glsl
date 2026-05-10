/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Deferred lighting evaluation: Lighting is evaluated in a separate pass.
 *
 * Outputs shading parameter per pixel using a randomized set of BSDFs.
 * Some render-pass are written during this pass.
 */

#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_nodetree_infos.hh"
#include "infos/eevee_surf_hybrid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)
FRAGMENT_SHADER_CREATE_INFO(eevee_geom_mesh)
FRAGMENT_SHADER_CREATE_INFO(eevee_surf_deferred_hybrid)
FRAGMENT_SHADER_CREATE_INFO(eevee_render_pass_out)
FRAGMENT_SHADER_CREATE_INFO(eevee_cryptomatte_out)

#include "draw_curves_lib.glsl"
#include "draw_view_lib.glsl"
#include "eevee_forward_lib.glsl"
#include "eevee_gbuffer_write_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

/* Global thickness because it is needed for closure_to_rgba. */
float g_thickness;
float3 g_forward_lighting_P;

float4 closure_to_rgba(Closure cl_unused)
{
  float3 radiance, transmittance;
  forward_lighting_eval(g_forward_lighting_P, g_thickness, radiance, transmittance);

  /* Reset for the next closure tree. */
  float noise = utility_tx_fetch(utility_tx, gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling_rng_1D_get(SAMPLING_CLOSURE));
  closure_weights_reset(closure_rand);

#if defined(MAT_TRANSPARENT) && defined(MAT_SHADER_TO_RGBA)
  float3 V = -drw_world_incident_vector(g_forward_lighting_P);
  LightProbeSample samp = lightprobe_load(g_forward_lighting_P, g_data.Ng, V);
  float3 radiance_behind = lightprobe_spherical_sample_normalized_with_parallax(
      samp, g_forward_lighting_P, V, 0.0);

#  ifndef MAT_FIRST_LAYER
  int2 texel = int2(gl_FragCoord.xy);
  if (texelFetchExtend(hiz_prev_tx, texel, 0).x != 1.0f) {
    radiance_behind = texelFetch(previous_layer_radiance_tx, texel, 0).xyz;
  }
#  endif

  radiance += radiance_behind * saturate(transmittance);
#endif

  return float4(radiance, saturate(1.0f - average(transmittance)));
}

void write_closure_data(int2 texel, int layer, float4 data)
{
  /* NOTE: The image view start at layer GBUF_CLOSURE_FB_LAYER_COUNT so all destination layer is
   * `layer - GBUF_CLOSURE_FB_LAYER_COUNT`. */
  imageStoreFast(out_gbuf_closure_img, int3(texel, layer - GBUF_CLOSURE_FB_LAYER_COUNT), data);
}

void write_normal_data(int2 texel, int layer, float2 data)
{
  /* NOTE: The image view start at layer GBUF_NORMAL_FB_LAYER_COUNT so all destination layer is
   * `layer - GBUF_NORMAL_FB_LAYER_COUNT`. */
  imageStoreFast(out_gbuf_normal_img, int3(texel, layer - GBUF_NORMAL_FB_LAYER_COUNT), data.xyyy);
}

void write_header_data(int2 texel, int layer, uint data)
{
  /* NOTE: The image view start at layer GBUF_HEADER_FB_LAYER_COUNT so all destination layer is
   * `layer - GBUF_HEADER_FB_LAYER_COUNT`. */
  imageStoreFast(
      out_gbuf_header_img, int3(texel, layer - GBUF_HEADER_FB_LAYER_COUNT), uint4(data));
}

#ifdef MAT_DEPTH_OFFSET
bool depth_offset_fragment_matches_prepass(float depth_offset)
{
  float fragment_depth = reverse_z::read(material_depth_offset_frag_depth(depth_offset));
  float prepass_depth = texelFetch(hiz_tx, int2(gl_FragCoord.xy), 0).r;
  return abs(fragment_depth - prepass_depth) <= 1.0e-6f;
}
#endif

void main()
{
  material_surface_cull_discard();
  init_globals();

  float noise = utility_tx_fetch(utility_tx, gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling_rng_1D_get(SAMPLING_CLOSURE));

  g_forward_lighting_P = g_data.P;
#ifdef MAT_DEPTH_OFFSET
  float depth_offset = nodetree_depth_offset();
  if (!depth_offset_fragment_matches_prepass(depth_offset)) {
    gpu_discard_fragment();
    return;
  }
  if (!material_depth_offset_is_zero(depth_offset)) {
    float3 depth_offset_P = material_depth_offset_apply_nodetree_position(depth_offset);
#  ifndef MAT_DEPTH_OFFSET_NO_LIGHTING
    g_forward_lighting_P = depth_offset_P;
#  endif
  }
#endif

#ifndef MAT_REFRACTION
  /* First visible hybrid surfaces replace previous AOV data. Refraction layers must preserve the
   * AOVs from the already-rendered surface behind unless they explicitly write an AOV themselves. */
  clear_aovs();
#endif
  clear_outline();

#ifdef MAT_DEPTH_OFFSET_NO_LIGHTING
  bool use_surface_depth = !material_depth_offset_is_zero(depth_offset);
  float surface_depth = use_surface_depth ? reverse_z::read(gl_FragCoord.z) : 0.0f;
#else
  constexpr bool use_surface_depth = false;
  float surface_depth = 0.0f;
#endif

#ifdef MAT_DEPTH_OFFSET
  material_depth_offset_write(depth_offset);
#endif

  g_thickness = nodetree_thickness() * thickness_mode;

  fragment_displacement();

  nodetree_surface(closure_rand);

  g_holdout = saturate(g_holdout);

  /** Transparency weight is already applied through dithering, remove it from other closures. */
  float alpha = 1.0f - average(g_transmittance);
  float alpha_rcp = safe_rcp(alpha);

  /* Object holdout. */
  eObjectInfoFlag ob_flag = drw_object_infos().flag;
  if (flag_test(ob_flag, OBJECT_HOLDOUT)) {
    /* alpha is set from rejected pixels / dithering. */
    g_holdout = 1.0f;

    /* Set alpha to 0.0 so that lighting is not computed. */
    alpha_rcp = 0.0f;
  }

  g_emission *= alpha_rcp;

  int2 out_texel = int2(gl_FragCoord.xy);

#ifdef MAT_SUBSURFACE
  constexpr bool use_sss = true;
#else
  constexpr bool use_sss = false;
#endif

  ObjectInfos object_infos = drw_infos[drw_resource_id()];
  bool use_light_linking = receiver_light_set_get(object_infos) != 0 ||
                           world_environment_disabled_get(object_infos);
  bool use_terminator_offset = object_infos.shadow_terminator_normal_offset > 0.0;

  /* ----- Render Passes output ----- */

#ifdef MAT_RENDER_PASS_SUPPORT /* Needed because node_tree isn't present in test shaders. */
  /* Some render pass can be written during the gbuffer pass. Light passes are written later. */
  if (imageSize(rp_cryptomatte_img).x > 1) {
    float4 cryptomatte_output = float4(
        cryptomatte_object_buf[drw_resource_id()], node_tree.crypto_hash, 0.0f);
    imageStoreFast(rp_cryptomatte_img, out_texel, cryptomatte_output);
  }
  output_renderpass_color(uniform_buf.render_pass.emission_id, float4(g_emission, 1.0f));
#endif

  /* ----- GBuffer output ----- */

  gbuffer::InputClosures gbuf_data;
  gbuf_data.closure[0] = g_closure_get_resolved(0, alpha_rcp);
#if CLOSURE_BIN_COUNT > 1
  gbuf_data.closure[1] = g_closure_get_resolved(1, alpha_rcp);
#endif
#if CLOSURE_BIN_COUNT > 2
  gbuf_data.closure[2] = g_closure_get_resolved(2, alpha_rcp);
#endif
  const bool use_object_id = use_sss || use_light_linking || use_terminator_offset;

  gbuffer::Packed gbuf = gbuffer::pack(gbuf_data,
                                       g_data.Ng,
                                       g_data.N,
                                       g_thickness,
                                       use_object_id,
                                       use_surface_depth,
                                       surface_depth);

  /* Output header and first closure using frame-buffer attachment. */
  out_gbuf_header = gbuf.header;
  out_gbuf_closure1 = gbuf.closure[0];
  out_gbuf_closure2 = gbuf.closure[1];
  out_gbuf_normal = gbuf.normal[0];

  /* Output remaining closures using image store. */
#if GBUFFER_LAYER_MAX >= 2 && !defined(GBUFFER_SIMPLE_CLOSURE_LAYOUT)
  if (flag_test(gbuf.used_layers, CLOSURE_DATA_2)) {
    write_closure_data(out_texel, 2, gbuf.closure[2]);
  }
  if (flag_test(gbuf.used_layers, CLOSURE_DATA_3)) {
    write_closure_data(out_texel, 3, gbuf.closure[3]);
  }
#endif
#if GBUFFER_LAYER_MAX >= 3
  if (flag_test(gbuf.used_layers, CLOSURE_DATA_4)) {
    write_closure_data(out_texel, 4, gbuf.closure[4]);
  }
  if (flag_test(gbuf.used_layers, CLOSURE_DATA_5)) {
    write_closure_data(out_texel, 5, gbuf.closure[5]);
  }
#endif

#if GBUFFER_LAYER_MAX >= 2
  if (flag_test(gbuf.used_layers, NORMAL_DATA_1)) {
    write_normal_data(out_texel, 1, gbuf.normal[1]);
  }
#endif
#if GBUFFER_LAYER_MAX >= 3
  if (flag_test(gbuf.used_layers, NORMAL_DATA_2)) {
    write_normal_data(out_texel, 2, gbuf.normal[2]);
  }
#endif

#if defined(GBUFFER_HAS_REFRACTION) || defined(GBUFFER_HAS_SUBSURFACE) || \
    defined(GBUFFER_HAS_TRANSLUCENT) || defined(MAT_DEPTH_OFFSET_NO_LIGHTING)
  if (flag_test(gbuf.used_layers, ADDITIONAL_DATA)) {
    write_normal_data(
        out_texel, uniform_buf.pipeline.gbuffer_additional_data_layer_id, gbuf.additional_info);
  }
#endif

  if (flag_test(gbuf.used_layers, OBJECT_ID)) {
    write_header_data(out_texel, 1, drw_resource_id());
  }

  /* ----- Radiance output ----- */

  /* Only output emission during the gbuffer pass. */
  out_radiance = float4(g_emission, 0.0f);
  out_radiance.rgb *= 1.0f - g_holdout;
  out_radiance.a = g_holdout;
}
