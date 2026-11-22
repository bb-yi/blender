/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

float3 shader_info_decode_hardware_shadow(float4 result)
{
  float visibility = saturate(dot(result.rgb, float3(0.2126f, 0.7152f, 0.0722f)));
  float self_shadow = clamp(result.a, 0.0f, 1.0f - visibility);
  return float3(visibility, self_shadow, 1.0f - visibility - self_shadow);
}

float3 shader_info_hardware_shadow_sample(LightData light, float2 frag_co)
{
#if defined(MAT_SHADER_INFO_HARDWARE_SHADOW) && defined(GPU_FRAGMENT_SHADER)
  if (light.hwrt_shadow_layer >= 0 && light.hwrt_shadow_layer < shader_info_hardware_shadow_layers) {
    float4 result = texelFetch(shader_info_hardware_visibility_tx,
                               int3(int2(frag_co), light.hwrt_shadow_layer),
                               0);
    return shader_info_decode_hardware_shadow(result);
  }
#endif
  return float3(-1.0f);
}

float shader_info_hardware_visibility(LightData light, float2 frag_co)
{
  return shader_info_hardware_shadow_sample(light, frag_co).x;
}

bool shader_info_is_zero(float3 value)
{
  return all(lessThanEqual(abs(value), float3(1e-8f)));
}

#define SHADER_INFO_STABLE_SHADOW_MAX_RAY_COUNT 32
#define SHADER_INFO_STABLE_SHADOW_MIN_STEP_COUNT 6
#define SHADER_INFO_SHADOW_MODE_BUILTIN 1.0f
#define SHADER_INFO_SHADOW_MODE_SOFT_FILTERED 2.0f
#define SHADER_INFO_SHADOW_UNKNOWN_CASTER_ID 0xFFFFFFFFu

float shader_info_max_component(float3 value)
{
  return max(value.x, max(value.y, value.z));
}

float shader_info_light_power_get(LightData light, LightingType type)
{
  /* Mask anything above 3. See LIGHT_TRANSLUCENT_WITH_THICKNESS. */
  return eevee::light::power_get(light, type);
}

bool shader_info_light_linking_affects_receiver(uint2 light_set_membership,
                                                uchar receiver_light_set)
{
  return bitmask64_test(light_set_membership, receiver_light_set);
}

float shader_info_light_shape_radiance(LightData light)
{
  if (light.type == LIGHT_RECT || light.type == LIGHT_ELLIPSE) {
    float area = light_area_data_get(light).size.x * light_area_data_get(light).size.y * 4.0f;
    if (light.type == LIGHT_ELLIPSE) {
      area *= M_PI / 4.0f;
    }
    return float(M_1_PI) / area;
  }

  if (light.type == LIGHT_OMNI_SPHERE || light.type == LIGHT_OMNI_DISK ||
      light.type == LIGHT_SPOT_SPHERE || light.type == LIGHT_SPOT_DISK)
  {
    float area = float(4.0f * M_PI) * square(light_local_data_get(light).local.shape_radius);
    return 1.0f / (area * float(M_PI));
  }

  if (is_sun_light(light.type)) {
    float inv_sin_sq = 1.0f + 1.0f / square(light_sun_data_get(light).shape_radius);
    return float(M_1_PI) * inv_sin_sq;
  }

  return 1.0f;
}

float shader_info_light_point_radiance(LightData light)
{
  if (light.type == LIGHT_RECT || light.type == LIGHT_ELLIPSE) {
    float area = light_area_data_get(light).size.x * light_area_data_get(light).size.y * 4.0f;
    float tmp = M_PI_2 / (M_PI_2 + sqrt(area));
    float mrp_scaling = tmp + (1.0f - tmp) * M_1_PI;
    return float(M_1_PI) * mrp_scaling;
  }

  if (light.type == LIGHT_OMNI_SPHERE || light.type == LIGHT_OMNI_DISK ||
      light.type == LIGHT_SPOT_SPHERE || light.type == LIGHT_SPOT_DISK)
  {
    return float(1.0f / (4.0f * M_PI));
  }

  if (is_sun_light(light.type)) {
    return 1.0f;
  }

  return 1.0f;
}

float shader_info_light_friendly_power_get(LightData light, LightingType type)
{
  float shape_power = shader_info_light_shape_radiance(light);
  float point_power = shader_info_light_point_radiance(light);
  if (shape_power <= 1e-16f) {
    return 0.0f;
  }
  return shader_info_light_power_get(light, type) * (point_power / shape_power);
}

float3 shader_info_resolve_normal(KernelGlobals &kg, ShadingData &sd, float3 normal_value)
{
  float normal_len_squared = dot(normal_value, normal_value);
  if (normal_len_squared > 1e-16f) {
    return normal_value * inversesqrt(normal_len_squared);
  }

  float geom_len_squared = dot(sd.Ng, sd.Ng);
  if (geom_len_squared > 1e-16f) {
    return sd.Ng * inversesqrt(geom_len_squared);
  }

  return float3(0.0f, 0.0f, 1.0f);
}

bool shader_info_shadow_is_builtin(float shadow_mode)
{
  return abs(shadow_mode - SHADER_INFO_SHADOW_MODE_BUILTIN) < 0.25f;
}

bool shader_info_shadow_is_soft_filtered(float shadow_mode)
{
  return abs(shadow_mode - SHADER_INFO_SHADOW_MODE_SOFT_FILTERED) < 0.25f;
}

int shader_info_shadow_stable_ray_count(float stable_shadow_samples)
{
  return clamp(
      int(stable_shadow_samples + 0.5f), 1, SHADER_INFO_STABLE_SHADOW_MAX_RAY_COUNT);
}

void shader_info_shadow_eval_random_numbers_get(out float3 random_shadow_3d,
                                                out float2 random_pcf_2d)
{
  random_shadow_3d = float3(0.5f);
  random_pcf_2d = float2(0.0f);
#if defined(GPU_FRAGMENT_SHADER) && !defined(GLSL_CPP_STUBS)
  [[resource_table]] eevee::ShadowRenderData &srd = resource_table_get(eevee::ShadowRenderData);
  if (srd.shadow_random) [[static_branch]] {
    [[resource_table]] const eevee::Sampling sampling = srd.sampling;
    [[resource_table]] const UtilityTexture util_tx = srd.util_tx;
    float3 blue_noise_3d = util_tx.fetch(gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).rgb;
    random_shadow_3d = fract(blue_noise_3d + sampling.rng_3D_get(SAMPLING_SHADOW_U));
    random_pcf_2d = fract(blue_noise_3d.xy + sampling.rng_2D_get(SAMPLING_SHADOW_X));
  }
#endif
}

#if defined(MAT_RAYCAST)
float shader_info_shadow_screen_depth_fetch(KernelGlobals &kg, ShadingData &sd, float2 uv)
{
  return eevee_hiz_sample(uv);
}
#endif

float shader_info_shadow_visibility_native(KernelGlobals &kg, ShadingData &sd, LightData light,
                                            bool is_directional,
                                            float3 position,
                                            float3 geometry_normal,
                                            float3 shading_normal,
                                            float normal_offset,
                                            float geometry_offset,
                                            int ray_count)
{
  return eevee_shadow_eval(sd.frag_co.xy, light,
                           is_directional,
                           false,
                           false,
                           0.0f,
                           position,
                           geometry_normal,
                           shading_normal,
                           normal_offset,
                           geometry_offset,
                            ray_count,
                           eevee_uniform_data_get().shadow.step_count);
}

float shader_info_shadow_visibility_builtin(KernelGlobals &kg, ShadingData &sd, LightData light,
                                            bool is_directional,
                                            float3 position,
                                            float3 geometry_normal,
                                            float3 shading_normal,
                                            float normal_offset,
                                            float geometry_offset)
{
  return shader_info_shadow_visibility_native(kg, sd, light, is_directional, position,
                                               geometry_normal, shading_normal, normal_offset,
                                               geometry_offset,
                                               eevee_uniform_data_get().shadow.ray_count);
}

float shader_info_shadow_visibility_stable(KernelGlobals &kg, ShadingData &sd, LightData light,
                                           bool is_directional,
                                           float3 position,
                                           float3 geometry_normal,
                                           float3 shading_normal,
                                           float normal_offset,
                                           float geometry_offset,
                                           float stable_shadow_samples)
{
  int ray_step_count = max(eevee_uniform_data_get().shadow.step_count, SHADER_INFO_STABLE_SHADOW_MIN_STEP_COUNT);
  int stable_ray_count = shader_info_shadow_stable_ray_count(stable_shadow_samples);
  return eevee_shadow_eval_stable(light,
                                  is_directional,
                                  false,
                                  false,
                                  0.0f,
                                  position,
                                  geometry_normal,
                                  shading_normal,
                                  normal_offset,
                                  geometry_offset,
                                  stable_ray_count,
                                  ray_step_count);
}

float shader_info_shadow_visibility_single(KernelGlobals &kg, ShadingData &sd, LightData light,
                                           bool is_directional,
                                           float3 position,
                                           float3 geometry_normal,
                                           float3 shading_normal,
                                           float normal_offset,
                                           float geometry_offset,
                                           float shadow_mode,
                                           float stable_shadow_samples)
{
  if (light.tilemap_index == LIGHT_NO_SHADOW) {
    return 1.0f;
  }

  const bool builtin = shader_info_shadow_is_builtin(shadow_mode);
  if (builtin || shader_info_shadow_is_soft_filtered(shadow_mode)) {
    int ray_count = builtin ? eevee_uniform_data_get().shadow.ray_count :
                              shader_info_shadow_stable_ray_count(stable_shadow_samples);
    return shader_info_shadow_visibility_native(kg, sd, light,
                                                 is_directional,
                                                 position,
                                                 geometry_normal,
                                                 shading_normal,
                                                 normal_offset,
                                                  geometry_offset,
                                                  ray_count);
  }

  return shader_info_shadow_visibility_stable(kg, sd, light,
                                              is_directional,
                                              position,
                                              geometry_normal,
                                              shading_normal,
                                              normal_offset,
                                              geometry_offset,
                                              stable_shadow_samples);
}

#if defined(SHADOW_CASTER_CLASSIFY)
struct ShaderInfoShadowClassification {
  float visibility;
  float self_shadow;
  float cast_shadow;
};

ShaderInfoShadowClassification shader_info_shadow_classification_none()
{
  ShaderInfoShadowClassification result;
  result.visibility = 1.0f;
  result.self_shadow = 0.0f;
  result.cast_shadow = 0.0f;
  return result;
}

ShaderInfoShadowClassification shader_info_shadow_classification_seeded(KernelGlobals &kg, ShadingData &sd,
    LightData light,
    const bool is_directional,
    float3 P,
    float3 Ng,
    float3 N,
    float terminator_normal_offset,
    float terminator_geometry_offset,
    int ray_count,
    int ray_step_count,
    float3 random_shadow_3d,
    float2 random_pcf_2d)
{
  float3 classification = eevee_shadow_caster_classification_seeded(
      light,
      is_directional,
      kg.resource_id_get(sd),
      P,
      Ng,
      N,
      terminator_normal_offset,
      terminator_geometry_offset,
      ray_count,
      ray_step_count,
      random_shadow_3d,
      random_pcf_2d);
  ShaderInfoShadowClassification result;
  result.visibility = classification.x;
  result.self_shadow = classification.y;
  result.cast_shadow = classification.z;
  return result;
}

ShaderInfoShadowClassification shader_info_shadow_classification_single(KernelGlobals &kg, ShadingData &sd,
    LightData light,
    bool is_directional,
    float3 position,
    float3 geometry_normal,
    float3 shading_normal,
    float normal_offset,
    float geometry_offset,
    float shadow_mode,
    float stable_shadow_samples)
{
  if (light.tilemap_index == LIGHT_NO_SHADOW) {
    return shader_info_shadow_classification_none();
  }
  const bool builtin = shader_info_shadow_is_builtin(shadow_mode);
  const bool use_native = builtin || shader_info_shadow_is_soft_filtered(shadow_mode);
  if (!eevee_uniform_data_get().shadow.use_caster_atlas) {
    ShaderInfoShadowClassification result = shader_info_shadow_classification_none();
    result.visibility = shader_info_shadow_visibility_single(kg, sd, light, is_directional,
                                                              position, geometry_normal, shading_normal,
                                                              normal_offset, geometry_offset, shadow_mode,
                                                              stable_shadow_samples);
    return result;
  }

  int ray_count = builtin ?
                      eevee_uniform_data_get().shadow.ray_count :
                      shader_info_shadow_stable_ray_count(stable_shadow_samples);
  int ray_step_count = use_native ?
                           eevee_uniform_data_get().shadow.step_count :
                           max(eevee_uniform_data_get().shadow.step_count,
                               SHADER_INFO_STABLE_SHADOW_MIN_STEP_COUNT);
  float3 random_shadow_3d;
  float2 random_pcf_2d;
  if (use_native) {
    shader_info_shadow_eval_random_numbers_get(random_shadow_3d, random_pcf_2d);
  }
  else {
    random_shadow_3d = float3(0.5f);
    random_pcf_2d = float2(0.0f);
  }

  return shader_info_shadow_classification_seeded(kg, sd, light,
                                                  is_directional,
                                                  position,
                                                  geometry_normal,
                                                  shading_normal,
                                                  normal_offset,
                                                  geometry_offset,
                                                  ray_count,
                                                  ray_step_count,
                                                  random_shadow_3d,
                                                  random_pcf_2d);
}

#endif

float shader_info_shadow_visibility(KernelGlobals &kg, ShadingData &sd, LightData light,
                                    bool is_directional,
                                    float3 light_vector,
                                    float3 position,
                                    float3 geometry_normal,
                                    float3 shading_normal,
                                    float normal_offset,
                                    float geometry_offset,
                                    float shadow_mode,
                                    float stable_shadow_samples)
{
  return shader_info_shadow_visibility_single(kg,
                                              sd,
                                              light,
                                              is_directional,
                                              position,
                                              geometry_normal,
                                              shading_normal,
                                              normal_offset,
                                              geometry_offset,
                                              shadow_mode,
                                              stable_shadow_samples);
}

#if defined(SHADOW_CASTER_CLASSIFY)
ShaderInfoShadowClassification shader_info_shadow_classified_visibility(KernelGlobals &kg, ShadingData &sd,
    LightData light,
    bool is_directional,
    float3 light_vector,
    float3 position,
    float3 geometry_normal,
    float3 shading_normal,
    float normal_offset,
    float geometry_offset,
    float shadow_mode,
    float stable_shadow_samples)
{
  return shader_info_shadow_classification_single(kg,
                                                  sd,
                                                  light,
                                                  is_directional,
                                                  position,
                                                  geometry_normal,
                                                  shading_normal,
                                                  normal_offset,
                                                  geometry_offset,
                                                  shadow_mode,
                                                  stable_shadow_samples);
}
#endif
