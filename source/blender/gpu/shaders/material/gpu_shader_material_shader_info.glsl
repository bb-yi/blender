/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

bool shader_info_is_zero(float3 value)
{
  return all(lessThanEqual(abs(value), float3(1e-8f)));
}

#define SHADER_INFO_STABLE_SHADOW_MAX_RAY_COUNT 32
#define SHADER_INFO_STABLE_SHADOW_MIN_STEP_COUNT 6
#define SHADER_INFO_SOFT_SHADOW_MAX_EVAL_COUNT 8
#define SHADER_INFO_SOFT_SHADOW_MAX_RAY_COUNT 4
#define SHADER_INFO_SHADOW_MODE_STABLE 0.0f
#define SHADER_INFO_SHADOW_MODE_BUILTIN 1.0f
#define SHADER_INFO_SHADOW_MODE_SOFT_FILTERED 2.0f

float shader_info_max_component(float3 value)
{
  return max(value.x, max(value.y, value.z));
}

float3 shader_info_resolve_normal(float3 normal_value)
{
  float normal_len_squared = dot(normal_value, normal_value);
  if (normal_len_squared > 1e-16f) {
    return normal_value * inversesqrt(normal_len_squared);
  }

  float geom_len_squared = dot(g_data.Ng, g_data.Ng);
  if (geom_len_squared > 1e-16f) {
    return g_data.Ng * inversesqrt(geom_len_squared);
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

int shader_info_shadow_soft_filtered_eval_count(float stable_shadow_samples)
{
  int requested_sample_count = shader_info_shadow_stable_ray_count(stable_shadow_samples);
  return clamp(int(ceil(sqrt(float(requested_sample_count)) * 1.5f)),
               2,
               SHADER_INFO_SOFT_SHADOW_MAX_EVAL_COUNT);
}

int shader_info_shadow_soft_filtered_ray_count(float stable_shadow_samples, int eval_count)
{
  int requested_sample_count = shader_info_shadow_stable_ray_count(stable_shadow_samples);
  return clamp(int(ceil(float(requested_sample_count) / float(max(eval_count, 1)))),
               1,
               SHADER_INFO_SOFT_SHADOW_MAX_RAY_COUNT);
}

float3 shader_info_shadow_soft_frame_rotation_3d()
{
  float3 rotation = float3(0.0f);
#if defined(EEVEE_SAMPLING_DATA) && !defined(GLSL_CPP_STUBS)
  rotation = sampling_rng_3D_get(SAMPLING_SHADOW_U);
#endif
  return rotation;
}

float2 shader_info_shadow_soft_frame_rotation_2d()
{
  float2 rotation = float2(0.0f);
#if defined(EEVEE_SAMPLING_DATA) && !defined(GLSL_CPP_STUBS)
  rotation = sampling_rng_2D_get(SAMPLING_SHADOW_X);
#endif
  return rotation;
}

float shader_info_shadow_soft_deband_noise(float3 position, float3 light_vector)
{
#if defined(GPU_FRAGMENT_SHADER)
  float seed = dot(position, float3(0.06711056f, 0.00583715f, 0.049981f)) +
               dot(light_vector, float3(0.03125f, 0.125f, 0.2109375f));
  return interleaved_gradient_noise(gl_FragCoord.xy, seed, 0.0f);
#else
  UNUSED_VARS(position, light_vector);
  return 0.5f;
#endif
}

float shader_info_shadow_visibility_single(LightData light,
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

  if (shader_info_shadow_is_builtin(shadow_mode)) {
    return shadow_eval(light,
                       is_directional,
                       false,
                       false,
                       0.0f,
                       position,
                       geometry_normal,
                       shading_normal,
                       normal_offset,
                       geometry_offset,
                       uniform_buf.shadow.ray_count,
                       uniform_buf.shadow.step_count);
  }

  int ray_step_count = max(uniform_buf.shadow.step_count, SHADER_INFO_STABLE_SHADOW_MIN_STEP_COUNT);
  int stable_ray_count = shader_info_shadow_stable_ray_count(stable_shadow_samples);
  return shadow_eval_stable(light,
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

float shader_info_shadow_soft_visibility_core(LightData light,
                                              bool is_directional,
                                              float3 position,
                                              float3 geometry_normal,
                                              float3 shading_normal,
                                              float normal_offset,
                                              float geometry_offset,
                                              float stable_shadow_samples,
                                              out float effective_sample_count)
{
  int eval_count = shader_info_shadow_soft_filtered_eval_count(stable_shadow_samples);
  int ray_count = shader_info_shadow_soft_filtered_ray_count(stable_shadow_samples, eval_count);
  if (eval_count <= 1 && ray_count <= 1) {
    effective_sample_count = 1.0f;
    return shader_info_shadow_visibility_single(light,
                                                is_directional,
                                                position,
                                                geometry_normal,
                                                shading_normal,
                                                normal_offset,
                                                geometry_offset,
                                                SHADER_INFO_SHADOW_MODE_BUILTIN,
                                                stable_shadow_samples);
  }

  int ray_step_count = max(uniform_buf.shadow.step_count, SHADER_INFO_STABLE_SHADOW_MIN_STEP_COUNT);
  float3 frame_rotation_3d = shader_info_shadow_soft_frame_rotation_3d();
  float2 frame_rotation_2d = shader_info_shadow_soft_frame_rotation_2d();
  float visibility_sum = 0.0f;
  float weight_sum = float(eval_count);

  for (int tap_index = 0; tap_index < SHADER_INFO_STABLE_SHADOW_MAX_RAY_COUNT; tap_index++) {
    if (tap_index >= eval_count) {
      break;
    }

    float2 ray_tap = shadow_stable_hammersley_2d(tap_index, eval_count, frame_rotation_3d.xy);
    float2 pcf_tap = shadow_stable_hammersley_2d(
        tap_index, eval_count, frame_rotation_2d + float2(0.37f, 0.13f));
    float z_tap = fract(van_der_corput_radical_inverse(uint(tap_index * 1103515245u + 12345u)) +
                        frame_rotation_3d.z);
    float3 random_shadow_3d = float3(ray_tap, z_tap);
    float2 random_pcf_2d = pcf_tap;

    float tap_visibility = shadow_eval_seeded(light,
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
                                              ray_step_count,
                                              random_shadow_3d,
                                              random_pcf_2d);
    visibility_sum += tap_visibility;
  }

  effective_sample_count = float(eval_count * ray_count);
  return saturate(visibility_sum / max(weight_sum, 1e-6f));
}

float shader_info_shadow_visibility(LightData light,
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
  if (!shader_info_shadow_is_soft_filtered(shadow_mode)) {
    return shader_info_shadow_visibility_single(light,
                                                is_directional,
                                                position,
                                                geometry_normal,
                                                shading_normal,
                                                normal_offset,
                                                geometry_offset,
                                                shadow_mode,
                                                stable_shadow_samples);
  }

  float3 frame_rotation_3d = shader_info_shadow_soft_frame_rotation_3d();
  float2 frame_rotation_2d = shader_info_shadow_soft_frame_rotation_2d();
  float effective_sample_count = 1.0f;
  float visibility = shader_info_shadow_soft_visibility_core(light,
                                                             is_directional,
                                                             position,
                                                             geometry_normal,
                                                             shading_normal,
                                                             normal_offset,
                                                             geometry_offset,
                                                             stable_shadow_samples,
                                                             effective_sample_count);
  float deband_strength = saturate(visibility * (1.0f - visibility) * 4.0f);
  float deband_noise = shader_info_shadow_soft_deband_noise(position, light_vector) - 0.5f;
  visibility += deband_noise * deband_strength / max(effective_sample_count, 1.0f);
  return saturate(visibility);
}

bool shader_info_is_world_sun_light(uint light_index, LightData light, bool is_local)
{
  if (is_local || !is_sun_light(light.type)) {
    return false;
  }

  uint directional_index = light_index - light_cull_buf.local_lights_len;
  if (directional_index >= WORLD_SUN_MAX) {
    return false;
  }

  LightData world_sun = sunlight_buf[directional_index];
  if (shader_info_is_zero(world_sun.color)) {
    return false;
  }

  float3 world_sun_direction = transform_z_axis(world_sun.object_to_world);
  float color_delta = length(light.color - world_sun.color);
  float direction_alignment = dot(light.sun().direction, world_sun_direction);
  return (color_delta < 1e-4f) && (direction_alignment > 0.9999f);
}

[[node]]
void node_shader_info(float3 position,
                      float3 normal_in,
                      float shadow_mode,
                      float stable_shadow_samples,
                      out float4 diffuse_shading,
                      out float shadow,
                      out float4 ambient_lighting,
                      out float half_lambert_factor)
{
#if defined(GPU_FRAGMENT_SHADER) && (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
  float3 shading_normal = shader_info_resolve_normal(normal_in);
  float3 geometry_normal = shader_info_resolve_normal(g_data.Ng);
  float3 view_vector = drw_world_incident_vector(position);

  ObjectInfos object_infos = drw_infos[drw_resource_id()];
  uchar receiver_light_set = receiver_light_set_get(object_infos);
  float normal_offset = object_infos.shadow_terminator_normal_offset;
  float geometry_offset = object_infos.shadow_terminator_geometry_offset;

  float3 diffuse_shading_sum = float3(0.0f);
  float visibility_sum = 0.0f;
  float shadow_weight_sum = 0.0f;
  float half_lambert_sum = 0.0f;
  float half_lambert_weight_sum = 0.0f;

  LIGHT_FOREACH_ALL_BEGIN(light_cull_buf,
                          light_zbin_buf,
                          light_tile_buf,
                          gl_FragCoord.xy,
                          drw_point_world_to_view(position).z,
                          l_idx,
                          is_local)
  {
    LightData light = light_buf[l_idx];
    bool is_directional = !is_local;

    if (shader_info_is_zero(light.color)) {
      continue;
    }
    if (!light_linking_affects_receiver(light.light_set_membership, receiver_light_set)) {
      continue;
    }

    LightVector lv = light_vector_get(light, is_directional, position);
    bool is_world_sun = shader_info_is_world_sun_light(l_idx, light, is_local);
    float surface_attenuation = light_attenuation_surface(light, is_directional, lv);
    float diffuse_power = light_power_get(light, LIGHT_DIFFUSE);
    if (diffuse_power < LIGHT_ATTENUATION_THRESHOLD) {
      continue;
    }

    if (is_world_sun) {
      continue;
    }

    float light_weight = diffuse_power * shader_info_max_component(light.color);
    float ndotl = dot(shading_normal, lv.L);
    float lambert = saturate(ndotl);
    float half_lambert = saturate(ndotl * 0.5f + 0.5f);

    float4 ltc_mat = float4(1.0f, 0.0f, 0.0f, 1.0f);
    float diffuse_radiance = light_ltc(utility_tx, light, shading_normal, view_vector, lv, ltc_mat);

    /* Match Goo's Shader Info structure: direct diffuse uses the light's diffuse radiance
     * without extra shadow masking or display remapping. */
    diffuse_shading_sum += light.color * diffuse_power * diffuse_radiance;
    half_lambert_sum += half_lambert * light_weight;
    half_lambert_weight_sum += light_weight;

    if (surface_attenuation > LIGHT_ATTENUATION_THRESHOLD) {
      float visibility = shader_info_shadow_visibility(light,
                                                       is_directional,
                                                       lv.L,
                                                       position,
                                                       geometry_normal,
                                                       shading_normal,
                                                       normal_offset,
                                                       geometry_offset,
                                                       shadow_mode,
                                                       stable_shadow_samples);
      float shadow_visibility = visibility * surface_attenuation;
      visibility_sum += shadow_visibility * light_weight;
      shadow_weight_sum += light_weight;
    }
  }
  LIGHT_FOREACH_ALL_END();

  diffuse_shading = float4(diffuse_shading_sum, 1.0f);
  shadow = (shadow_weight_sum > 1e-8f) ? saturate(visibility_sum / shadow_weight_sum) : 0.0f;

#  ifdef SPHERE_PROBE
  LightProbeSample probe_sample = lightprobe_load(position, geometry_normal, view_vector);
  probe_sample.volume_irradiance = spherical_harmonics_clamp(probe_sample.volume_irradiance,
                                                             uniform_buf.clamp.surface_indirect);
  float3 ambient = spherical_harmonics_evaluate_lambert(shading_normal,
                                                        probe_sample.volume_irradiance);
  ambient_lighting = float4(max(ambient, float3(0.0f)), 1.0f);
#  else
  ambient_lighting = float4(0.0f);
#  endif

  half_lambert_factor = (half_lambert_weight_sum > 1e-8f) ?
                            saturate(half_lambert_sum / half_lambert_weight_sum) :
                            0.0f;
#else
  diffuse_shading = float4(0.0f);
  shadow = 0.0f;
  ambient_lighting = float4(0.0f);
  half_lambert_factor = 0.0f;
#endif
}
