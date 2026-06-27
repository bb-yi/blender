/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER

#  if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER) && defined(MAT_NPR_LIGHTING)

#    define NPR_STABLE_SHADOW_RAY_COUNT 8
#    define NPR_STABLE_SHADOW_MIN_STEP_COUNT 6

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
    int ray_step_count = max(uniform_buf.shadow.step_count, NPR_STABLE_SHADOW_MIN_STEP_COUNT);
    shadow_mask = shadow_eval_stable(light,
                                     is_directional,
                                     false,
                                     false,
                                     0.0f,
                                     g_data.P,
                                     g_data.Ng,
                                     N,
                                     0.0f,
                                     0.0f,
                                     NPR_STABLE_SHADOW_RAY_COUNT,
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

#    define FOREACH_LIGHT_BEGIN( \
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

#    define FOREACH_LIGHT_END() LIGHT_FOREACH_ALL_END()

#  else

#    if !defined(FOREACH_LIGHT_BEGIN)
#      define FOREACH_LIGHT_BEGIN( \
          N, out_color, out_vector, out_distance, out_attenuation, out_shadow_mask) \
        if (false) {
#    endif

#    if !defined(FOREACH_LIGHT_END)
#      define FOREACH_LIGHT_END() }
#    endif

#  endif

#else

/**
 * Dummy functions for gpu_shader_dependency.
 * Functions need parameters to be reflected, but zones use custom IO handling.
 */
[[node]]
void FOREACH_LIGHT_BEGIN(float dummy)
{
}

[[node]]
void FOREACH_LIGHT_END(float dummy)
{
}

#endif

[[node]]
void npr_image_sample_view(TextureHandle image, float3 offset, float4 &color, float &alpha)
{
#if (defined(NPR_SHADER) || defined(MAT_FILTER)) && defined(GPU_FRAGMENT_SHADER)
  color = TextureHandle_eval(image, offset.xy, false);
  alpha = color.a;
  /* Scene color stores transmittance in alpha. Invert to get opacity,
   * matching the original Scene Color node's separate Alpha output. */
  if (image.type == TEX_HANDLE_SCENE && image.index == 0) {
    alpha = saturate(1.0f - alpha);
  }
  else if (image.type == TEX_HANDLE_SCENE && image.index == 1) {
    alpha = color.r;
  }
#else
  color = float4(0.0f);
  alpha = 0.0f;
#endif
}

[[node]]
void npr_image_sample_texel(TextureHandle image, float3 offset, float4 &color, float &alpha)
{
#if (defined(NPR_SHADER) || defined(MAT_FILTER)) && defined(GPU_FRAGMENT_SHADER)
  color = TextureHandle_eval(image, offset.xy, true);
  alpha = color.a;
  if (image.type == TEX_HANDLE_SCENE && image.index == 0) {
    alpha = saturate(1.0f - alpha);
  }
  else if (image.type == TEX_HANDLE_SCENE && image.index == 1) {
    alpha = color.r;
  }
#else
  color = float4(0.0f);
  alpha = 0.0f;
#endif
}

[[node]]
void npr_image_sample_uv(TextureHandle image, float3 uv, float4 &color, float &alpha)
{
#if (defined(NPR_SHADER) || defined(MAT_FILTER)) && defined(GPU_FRAGMENT_SHADER)
  color = TextureHandle_eval_uv(image, uv.xy);
  alpha = color.a;
  if (image.type == TEX_HANDLE_SCENE && image.index == 0) {
    alpha = saturate(1.0f - alpha);
  }
  else if (image.type == TEX_HANDLE_SCENE && image.index == 1) {
    alpha = color.r;
  }
#else
  color = float4(0.0f);
  alpha = 0.0f;
#endif
}

[[node]]
void npr_input(TextureHandle &combined_color,
               TextureHandle &diffuse_color,
               TextureHandle &diffuse_direct,
               TextureHandle &diffuse_indirect,
               TextureHandle &specular_color,
               TextureHandle &specular_direct,
               TextureHandle &specular_indirect,
               TextureHandle &position,
               TextureHandle &normal)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  npr_input_impl(combined_color,
                 diffuse_color,
                 diffuse_direct,
                 diffuse_indirect,
                 specular_color,
                 specular_direct,
                 specular_indirect,
                 position,
                 normal);
#else
  combined_color = TEXTURE_HANDLE_DEFAULT;
  diffuse_color = TEXTURE_HANDLE_DEFAULT;
  diffuse_direct = TEXTURE_HANDLE_DEFAULT;
  diffuse_indirect = TEXTURE_HANDLE_DEFAULT;
  specular_color = TEXTURE_HANDLE_DEFAULT;
  specular_direct = TEXTURE_HANDLE_DEFAULT;
  specular_indirect = TEXTURE_HANDLE_DEFAULT;
  position = TEXTURE_HANDLE_DEFAULT;
  normal = TEXTURE_HANDLE_DEFAULT;
#endif
}

[[node]]
void npr_output(float4 color, float4 &out_color)
{
  out_color = color;
}

[[node]]
void npr_output_float(float color, float4 &out_color)
{
  out_color = float4(float3(color), 1.0f);
}

[[node]]
void npr_output_vec3(float3 color, float4 &out_color)
{
  out_color = float4(color, 1.0f);
}

[[node]]
void npr_output_texture_handle(TextureHandle color, float4 &out_color)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  out_color = TextureHandle_eval(color, float2(0.0f), false);
#else
  out_color = float4(0.0f);
#endif
}

[[node]]
void npr_refraction(TextureHandle &combined_color, TextureHandle &position)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  npr_refraction_impl(combined_color, position);
#else
  combined_color = TEXTURE_HANDLE_DEFAULT;
  position = TEXTURE_HANDLE_DEFAULT;
#endif
}

[[node]]
void node_input_aov(float hash, TextureHandle &color, TextureHandle &value)
{
#if (defined(NPR_SHADER) || defined(MAT_FILTER)) && defined(GPU_FRAGMENT_SHADER)
  input_aov_impl(floatBitsToUint(hash), color, value);
#else
  color = TEXTURE_HANDLE_DEFAULT;
  value = TEXTURE_HANDLE_DEFAULT;
#endif
}
