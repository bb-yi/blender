/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * NPR evaluation pass for materials with an attached NPR tree.
 */

#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_nodetree_infos.hh"
#include "infos/eevee_surf_deferred_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)
FRAGMENT_SHADER_CREATE_INFO(eevee_geom_mesh)
FRAGMENT_SHADER_CREATE_INFO(eevee_surf_npr)

#include "draw_view_lib.glsl"
#include "eevee_deferred_combine_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_renderpass_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

#define TEX_HANDLE_NULL 0u
#define TEX_HANDLE_RP_COLOR 1u
#define TEX_HANDLE_RP_VALUE 2u
#define TEX_HANDLE_COMBINED_COLOR 10u
#define TEX_HANDLE_DIFFUSE_COLOR 11u
#define TEX_HANDLE_DIFFUSE_DIRECT 12u
#define TEX_HANDLE_DIFFUSE_INDIRECT 13u
#define TEX_HANDLE_SPECULAR_COLOR 14u
#define TEX_HANDLE_SPECULAR_DIRECT 15u
#define TEX_HANDLE_SPECULAR_INDIRECT 16u
#define TEX_HANDLE_POSITION 17u
#define TEX_HANDLE_NORMAL 18u
#define TEX_HANDLE_BACK_COMBINED_COLOR 19u
#define TEX_HANDLE_BACK_POSITION 20u

void npr_input_impl(out TextureHandle combined_color,
                    out TextureHandle diffuse_color,
                    out TextureHandle diffuse_direct,
                    out TextureHandle diffuse_indirect,
                    out TextureHandle specular_color,
                    out TextureHandle specular_direct,
                    out TextureHandle specular_indirect,
                    out TextureHandle position,
                    out TextureHandle normal)
{
  combined_color = TextureHandle(TEX_HANDLE_COMBINED_COLOR, 0);
  diffuse_color = TextureHandle(TEX_HANDLE_DIFFUSE_COLOR, 0);
  diffuse_direct = TextureHandle(TEX_HANDLE_DIFFUSE_DIRECT, 0);
  diffuse_indirect = TextureHandle(TEX_HANDLE_DIFFUSE_INDIRECT, 0);
  specular_color = TextureHandle(TEX_HANDLE_SPECULAR_COLOR, 0);
  specular_direct = TextureHandle(TEX_HANDLE_SPECULAR_DIRECT, 0);
  specular_indirect = TextureHandle(TEX_HANDLE_SPECULAR_INDIRECT, 0);
  position = TextureHandle(TEX_HANDLE_POSITION, 0);
  normal = TextureHandle(TEX_HANDLE_NORMAL, 0);
}

void npr_refraction_impl(out TextureHandle combined_color, out TextureHandle position)
{
#ifdef MAT_NPR_REFRACTION
  combined_color = TextureHandle(TEX_HANDLE_BACK_COMBINED_COLOR, 0);
  position = TextureHandle(TEX_HANDLE_BACK_POSITION, 0);
#else
  combined_color = TEXTURE_HANDLE_DEFAULT;
  position = TEXTURE_HANDLE_DEFAULT;
#endif
}

void input_aov_impl(uint hash, out TextureHandle color, out TextureHandle value)
{
  uint total_len = uniform_buf.render_pass.aovs.color_len + uniform_buf.render_pass.aovs.value_len;
  uint hash_index;
  for (hash_index = 0u; hash_index < AOV_MAX && hash_index < total_len; hash_index += 4u) {
    bool4 cmp_mask = equal(uniform_buf.render_pass.aovs.hash[hash_index >> 2u], uint4(hash));
    if (any(cmp_mask)) {
      hash_index += (cmp_mask[0] ? 0u : (cmp_mask[1] ? 1u : (cmp_mask[2] ? 2u : 3u)));
      break;
    }
  }

  if (hash_index < total_len) {
    bool is_value = hash_index >= uint(uniform_buf.render_pass.aovs.color_len);
    uint aov_index = hash_index - (is_value ? uint(uniform_buf.render_pass.aovs.color_len) : 0u);
    int render_pass_index = (is_value ? uniform_buf.render_pass.value_len :
                                        uniform_buf.render_pass.color_len) +
                            int(aov_index);
    color = is_value ? TEXTURE_HANDLE_DEFAULT :
                       TextureHandle(TEX_HANDLE_RP_COLOR, render_pass_index);
    value = is_value ? TextureHandle(TEX_HANDLE_RP_VALUE, render_pass_index) :
                       TEXTURE_HANDLE_DEFAULT;
    return;
  }

  color = TEXTURE_HANDLE_DEFAULT;
  value = TEXTURE_HANDLE_DEFAULT;
}

float4 swap_alpha(float4 v)
{
  v.a = 1.0f - saturate(v.a);
  return v;
}

#define TEXTURE_HANDLE_EVAL_DEFINED

float4 TextureHandle_eval_impl(TextureHandle tex, float2 offset, bool texel_offset)
{
  if (tex.type == TEX_HANDLE_NULL) {
    return float4(0.0f);
  }

  if (all(equal(offset, float2(0.0f)))) {
    switch (tex.type) {
      case TEX_HANDLE_COMBINED_COLOR: {
        /* Combined Color should reflect the pre-NPR combined buffer, including emission. */
        return swap_alpha(g_combined_color);
      }
      case TEX_HANDLE_DIFFUSE_COLOR:
        return swap_alpha(g_diffuse_color);
      case TEX_HANDLE_DIFFUSE_DIRECT:
        return swap_alpha(g_diffuse_direct);
      case TEX_HANDLE_DIFFUSE_INDIRECT:
        return swap_alpha(g_diffuse_indirect);
      case TEX_HANDLE_SPECULAR_COLOR:
        return swap_alpha(g_specular_color);
      case TEX_HANDLE_SPECULAR_DIRECT:
        return swap_alpha(g_specular_direct);
      case TEX_HANDLE_SPECULAR_INDIRECT:
        return swap_alpha(g_specular_indirect);
      case TEX_HANDLE_POSITION:
#ifdef MAT_DEPTH_OFFSET
      {
        int2 texel = int2(gl_FragCoord.xy);
        int2 extent = textureSize(radiance_tx, 0);
        float depth = texelFetch(hiz_tx, texel, 0).r;
        float2 screen_uv = (float2(texel) + 0.5f) / float2(extent);
        return float4(drw_point_screen_to_world(float3(screen_uv, depth)), 0.0f);
      }
#else
        return float4(g_data.P, 0.0f);
#endif
      case TEX_HANDLE_NORMAL:
        return float4(g_average_normal, 0.0f);
      default:
        break;
    }
  }

  int2 texel = int2(gl_FragCoord.xy);
  int2 extent = textureSize(radiance_tx, 0);
  if (texel_offset) {
    texel += int2(offset);
  }
  else {
    float3 vP = drw_point_world_to_view(g_data.P);
    float2 uv = drw_point_view_to_screen(vP + float3(offset, 0.0f)).xy;
    texel = int2(uv * float2(extent));
  }

  texel = clamp(texel, int2(0), extent - int2(1));

  float depth = texelFetch(hiz_tx, texel, 0).r;
  float2 screen_uv = (float2(texel) + 0.5f) / float2(extent);

  switch (tex.type) {
    case TEX_HANDLE_RP_COLOR:
      /* AOV color passes are data buffers; keep them opaque when exposed through NPR output. */
      return float4(imageLoad(rp_color_img, int3(texel, tex.index)).rgb, 0.0f);
    case TEX_HANDLE_RP_VALUE:
      return float4(imageLoad(rp_value_img, int3(texel, tex.index)).rrr, 0.0f);
#ifdef MAT_NPR_REFRACTION
    case TEX_HANDLE_BACK_COMBINED_COLOR:
      return texelFetch(radiance_back_tx, texel, 0);
#endif
    case TEX_HANDLE_POSITION: {
      float3 position = drw_point_screen_to_world(float3(screen_uv, depth));
      return float4(position, 0.0f);
    }
#ifdef MAT_NPR_REFRACTION
    case TEX_HANDLE_BACK_POSITION: {
      float back_depth = texelFetch(hiz_back_tx, texel, 0).r;
      float3 back_position = drw_point_screen_to_world(float3(screen_uv, back_depth));
      return float4(back_position, 0.0f);
    }
#endif
    default: {
      if (depth == 1.0f) {
        if (tex.type == TEX_HANDLE_COMBINED_COLOR) {
          return texelFetch(radiance_tx, texel, 0);
        }
        if (tex.type == TEX_HANDLE_NORMAL) {
          float3 position = drw_point_screen_to_world(float3(screen_uv, depth));
          float3 normal = drw_world_incident_vector(position);
          return float4(normal, 0.0f);
        }
        return float4(0.0f);
      }

      DeferredCombine dc = deferred_combine(texel);
      deferred_combine_clamp(dc);
      switch (tex.type) {
        case TEX_HANDLE_COMBINED_COLOR:
          return texelFetch(radiance_tx, texel, 0);
        case TEX_HANDLE_DIFFUSE_COLOR:
          return float4(dc.diffuse_color, 0.0f);
        case TEX_HANDLE_DIFFUSE_DIRECT:
          return float4(dc.diffuse_direct, 0.0f);
        case TEX_HANDLE_DIFFUSE_INDIRECT:
          return float4(dc.diffuse_indirect, 0.0f);
        case TEX_HANDLE_SPECULAR_COLOR:
          return float4(dc.specular_color, 0.0f);
        case TEX_HANDLE_SPECULAR_DIRECT:
          return float4(dc.specular_direct, 0.0f);
        case TEX_HANDLE_SPECULAR_INDIRECT:
          return float4(dc.specular_indirect, 0.0f);
        case TEX_HANDLE_NORMAL:
          return float4(dc.average_normal, 0.0f);
        default:
          return float4(0.0f);
      }
    }
  }
}

float4 TextureHandle_eval(TextureHandle tex, float2 offset, bool texel_offset)
{
  return swap_alpha(TextureHandle_eval_impl(tex, offset, texel_offset));
}

float4 TextureHandle_eval(TextureHandle tex)
{
  return TextureHandle_eval(tex, float2(0.0f), true);
}

#ifndef FOREACH_LIGHT_BEGIN
#ifdef MAT_NPR_LIGHTING
bool npr_is_zero(float3 value)
{
  return all(lessThanEqual(abs(value), float3(1e-8f)));
}

bool foreach_light_setup(uint l_idx,
                         bool is_directional,
                         float3 N,
                         out float4 out_color,
                         out float3 out_vector,
                         out float out_distance,
                         out float out_attenuation,
                         out float out_shadow_mask)
{
  LightData light = light_buf[l_idx];
  if (npr_is_zero(light.color)) {
    return false;
  }

  ObjectInfos object_infos = drw_infos[drw_resource_id()];
  uchar receiver_light_set = receiver_light_set_get(object_infos);
  if (!light_linking_affects_receiver(light.light_set_membership, receiver_light_set)) {
    return false;
  }

  LightVector lv = light_vector_get(light, is_directional, g_data.P);
  float attenuation = light_attenuation_volume(light, is_directional, lv);
  if (attenuation < LIGHT_ATTENUATION_THRESHOLD) {
    return false;
  }

  float3 V = drw_world_incident_vector(g_data.P);
  float4 ltc_mat = float4(1.0f, 0.0f, 0.0f, 1.0f);
  float ltc = light_ltc(utility_tx, light, lv.L, V, lv, ltc_mat);
  attenuation *= ltc * light_power_get(light, LIGHT_DIFFUSE);
  if (attenuation < LIGHT_ATTENUATION_THRESHOLD) {
    return false;
  }

  float shadow_mask = 1.0f;
  if (light.tilemap_index != LIGHT_NO_SHADOW) {
    int ray_count = uniform_buf.shadow.ray_count;
    int ray_step_count = uniform_buf.shadow.step_count;
    shadow_mask = shadow_eval(light,
                              is_directional,
                              false,
                              false,
                              0.0f,
                              g_data.P,
                              g_data.Ng,
                              N,
                              0.0f,
                              0.0f,
                              ray_count,
                              ray_step_count);
    shadow_mask *= dot(N, lv.L) > 0.0f ? 1.0f : 0.0f;
  }

  out_color = float4(light.color, 1.0f);
  out_vector = lv.L;
  out_distance = lv.dist;
  out_attenuation = attenuation;
  out_shadow_mask = shadow_mask;
  return true;
}

#  define FOREACH_LIGHT_BEGIN( \
      N, out_color, out_vector, out_distance, out_attenuation, out_shadow_mask) \
    LIGHT_FOREACH_ALL_BEGIN(light_cull_buf, \
                            light_zbin_buf, \
                            light_tile_buf, \
                            gl_FragCoord.xy, \
                            drw_point_world_to_view(g_data.P).z, \
                            l_idx, \
                            is_local) \
    if (!foreach_light_setup(l_idx, \
                             !is_local, \
                             N, \
                             out_color, \
                             out_vector, \
                             out_distance, \
                             out_attenuation, \
                             out_shadow_mask)) \
    { \
      continue; \
    }

#  define FOREACH_LIGHT_END() LIGHT_FOREACH_ALL_END()
#else
#  define FOREACH_LIGHT_BEGIN( \
      N, out_color, out_vector, out_distance, out_attenuation, out_shadow_mask)
#  define FOREACH_LIGHT_END()
#endif
#endif

float4 closure_to_rgba(Closure cl)
{
  UNUSED_VARS(cl);

  float4 out_color;
  out_color.rgb = g_emission;
  out_color.a = saturate(1.0f - average(g_transmittance));

  /* Reset for the next closure tree, matching the regular deferred path. */
  float noise = utility_tx_fetch(utility_tx, gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling_rng_1D_get(SAMPLING_CLOSURE));
  closure_weights_reset(closure_rand);

  return out_color;
}

void main()
{
  material_surface_cull_discard();
  init_globals();

#ifdef MAT_DEPTH_OFFSET
  material_depth_offset_write();
#endif

  int2 texel = int2(gl_FragCoord.xy);
  DeferredCombine dc = deferred_combine(texel);
  deferred_combine_clamp(dc);

  /* Preserve the exact pre-NPR combined input so emissive surfaces do not disappear. */
  g_combined_color = float4(texelFetch(radiance_tx, texel, 0).rgb, 1.0f);
  g_diffuse_color = float4(dc.diffuse_color, 1.0f);
  g_diffuse_direct = float4(dc.diffuse_direct, 1.0f);
  g_diffuse_indirect = float4(dc.diffuse_indirect, 1.0f);
  g_average_normal = dc.average_normal;
  g_specular_color = float4(dc.specular_color, 1.0f);
  g_specular_direct = float4(dc.specular_direct, 1.0f);
  g_specular_indirect = float4(dc.specular_indirect, 1.0f);

  out_radiance = swap_alpha(nodetree_npr());
  nodetree_surface(0.0f);
}
