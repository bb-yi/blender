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
#define SHADER_INFO_SOFT_SHADOW_SPATIAL_MAX_TAPS 8
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

float shader_info_shadow_gaussian_factor(float linear_distance, float standard_deviation)
{
  const float log_2_inv = 1.442695041f;
  return log_2_inv * standard_deviation / max(linear_distance * linear_distance, 1e-8f);
}

float shader_info_shadow_gaussian_weight(float factor, float square_distance)
{
  return exp2(-factor * square_distance);
}

float shader_info_shadow_planar_weight(float3 plane_normal,
                                       float3 plane_position,
                                       float3 sample_position,
                                       float scale)
{
  float plane_distance = dot(plane_normal, sample_position - plane_position);
  return shader_info_shadow_gaussian_weight(scale, plane_distance * plane_distance);
}

float shader_info_shadow_angle_weight(float3 center_normal, float3 sample_normal)
{
  float facing = saturate(dot(center_normal, sample_normal));
  facing *= facing;
  facing *= facing;
  return facing;
}

#if defined(MAT_RAYCAST)
int2 shader_info_shadow_uv_to_texel(float2 uv, int2 tex_size)
{
  return clamp(int2(uv * float2(tex_size)), int2(0), tex_size - int2(1));
}

float2 shader_info_shadow_texel_to_uv(int2 texel, int2 tex_size)
{
  return (float2(texel) + 0.5f) / float2(tex_size);
}

uint shader_info_shadow_object_id_fetch(int2 texel)
{
  return texelFetch(object_id_tx, texel, 0).x;
}

float shader_info_shadow_screen_depth_fetch(float2 uv)
{
  return textureLod(hiz_tx, uv, 0.0f).r;
}

float3 shader_info_shadow_prepass_normal_fetch(int2 texel, float3 fallback_normal)
{
  float3 sample_normal = texelFetch(prepass_normal_tx, texel, 0).xyz * 2.0f - 1.0f;
  return shader_info_resolve_normal(sample_normal + fallback_normal * 1e-8f);
}

bool shader_info_shadow_fetch_surface_sample(float2 uv,
                                             bool use_object_id,
                                             uint self_id,
                                             float3 fallback_normal,
                                             out float3 sample_position,
                                             out float3 sample_normal,
                                             out float2 sample_uv)
{
  int2 tex_size = textureSize(object_id_tx, 0);
  if (tex_size.x <= 0 || tex_size.y <= 0) {
    return false;
  }

  int2 sample_texel = shader_info_shadow_uv_to_texel(clamp(uv, float2(0.0f), float2(1.0f)),
                                                     tex_size);
  uint sample_id = shader_info_shadow_object_id_fetch(sample_texel);
  if (use_object_id && sample_id != self_id) {
    return false;
  }

  sample_uv = shader_info_shadow_texel_to_uv(sample_texel, tex_size);
  float sample_screen_depth = shader_info_shadow_screen_depth_fetch(sample_uv);
  if (sample_screen_depth >= 1.0f) {
    return false;
  }

  sample_position = drw_point_screen_to_world(float3(sample_uv, sample_screen_depth));
  sample_normal = shader_info_shadow_prepass_normal_fetch(sample_texel, fallback_normal);
  return true;
}
#endif

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

float shader_info_shadow_soft_spatial_visibility(LightData light,
                                                 bool is_directional,
                                                 float3 position,
                                                 float3 geometry_normal,
                                                 float3 shading_normal,
                                                 float normal_offset,
                                                 float geometry_offset,
                                                 float stable_shadow_samples,
                                                 float center_visibility,
                                                 float3 frame_rotation_3d,
                                                 float2 frame_rotation_2d)
{
#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_RAYCAST)
  float penumbra = saturate(1.0f - abs(center_visibility * 2.0f - 1.0f));
  if (penumbra <= 1e-3f) {
    return center_visibility;
  }

  int2 tex_size = textureSize(object_id_tx, 0);
  if (tex_size.x <= 0 || tex_size.y <= 0) {
    return center_visibility;
  }

  int2 center_texel = clamp(int2(gl_FragCoord.xy), int2(0), tex_size - int2(1));
  uint center_id = shader_info_shadow_object_id_fetch(center_texel);
  bool use_object_id = (center_id != 0u);
  uint self_id = (center_id != 0u) ? center_id : (drw_resource_id() & 0xFFFFu);

  float2 center_uv = shader_info_shadow_texel_to_uv(center_texel, tex_size);
  float center_screen_depth = shader_info_shadow_screen_depth_fetch(center_uv);
  if (center_screen_depth >= 1.0f) {
    return center_visibility;
  }

  float3 center_surface_position = drw_point_screen_to_world(float3(center_uv, center_screen_depth));
  float3 center_surface_normal = shader_info_shadow_prepass_normal_fetch(center_texel,
                                                                         geometry_normal);
  float2 texel_size = 1.0f / float2(tex_size);
  float radius_factor = saturate((stable_shadow_samples - 4.0f) / 28.0f);
  float filter_radius = mix(1.5f, 3.0f, radius_factor) * mix(0.75f, 1.0f, penumbra);
  float gaussian = shader_info_shadow_gaussian_factor(max(filter_radius, 1.0f), 1.5f);

  int tap_count = clamp(4 + int(radius_factor * 4.0f + 0.5f), 4, SHADER_INFO_SOFT_SHADOW_SPATIAL_MAX_TAPS);
  int ray_count = (stable_shadow_samples >= 24.0f) ? 2 : 1;
  int ray_step_count = max(uniform_buf.shadow.step_count, SHADER_INFO_STABLE_SHADOW_MIN_STEP_COUNT);

  float visibility_sum = center_visibility;
  float weight_sum = 1.0f;

  for (int tap_index = 0; tap_index < SHADER_INFO_SOFT_SHADOW_SPATIAL_MAX_TAPS; tap_index++) {
    if (tap_index >= tap_count) {
      break;
    }

    float tap_angle = 6.28318530718f * (float(tap_index) / float(tap_count) + frame_rotation_2d.x);
    float tap_radius = filter_radius * mix(0.65f, 1.0f, fract(frame_rotation_2d.y + float(tap_index) * 0.381966f));
    float2 offset_px = float2(cos(tap_angle), sin(tap_angle)) * tap_radius;
    float2 sample_uv = center_uv + offset_px * texel_size;

    float3 sample_position;
    float3 sample_normal;
    float2 resolved_uv;
    if (!shader_info_shadow_fetch_surface_sample(sample_uv,
                                                 use_object_id,
                                                 self_id,
                                                 center_surface_normal,
                                                 sample_position,
                                                 sample_normal,
                                                 resolved_uv))
    {
      continue;
    }

    float spatial_weight = shader_info_shadow_gaussian_weight(gaussian, dot(offset_px, offset_px));
    float planar_weight = shader_info_shadow_planar_weight(center_surface_normal,
                                                           center_surface_position,
                                                           sample_position,
                                                           1200.0f);
    float normal_weight = shader_info_shadow_angle_weight(center_surface_normal, sample_normal);
    float weight = spatial_weight * planar_weight * normal_weight;
    if (weight <= 1e-5f) {
      continue;
    }

    float2 ray_tap = shadow_stable_hammersley_2d(tap_index, tap_count, frame_rotation_3d.xy + resolved_uv);
    float2 pcf_tap = shadow_stable_hammersley_2d(
        tap_index, tap_count, frame_rotation_2d + resolved_uv.yx + float2(0.19f, 0.61f));
    float z_tap = fract(van_der_corput_radical_inverse(uint(tap_index * 747796405u + 2891336453u)) +
                        frame_rotation_3d.z + dot(resolved_uv, float2(0.25f, 0.5f)));
    float3 random_shadow_3d = float3(ray_tap, z_tap);
    float2 random_pcf_2d = pcf_tap;

    float tap_visibility = shadow_eval_seeded(light,
                                              is_directional,
                                              false,
                                              false,
                                              0.0f,
                                              sample_position,
                                              sample_normal,
                                              sample_normal,
                                              normal_offset,
                                              geometry_offset,
                                              ray_count,
                                              ray_step_count,
                                              random_shadow_3d,
                                              random_pcf_2d);

    visibility_sum += tap_visibility * weight;
    weight_sum += weight;
  }

  float filtered_visibility = saturate(visibility_sum / max(weight_sum, 1e-6f));
  float filter_blend = saturate(penumbra * 1.75f);
  return mix(center_visibility, filtered_visibility, filter_blend);
#else
  return center_visibility;
#endif
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
  if (shader_info_shadow_is_builtin(shadow_mode)) {
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
  visibility = shader_info_shadow_soft_spatial_visibility(light,
                                                          is_directional,
                                                          position,
                                                          geometry_normal,
                                                          shading_normal,
                                                          normal_offset,
                                                          geometry_offset,
                                                          stable_shadow_samples,
                                                          visibility,
                                                          frame_rotation_3d,
                                                          frame_rotation_2d);

  float deband_strength = saturate(visibility * (1.0f - visibility) * 4.0f);
  float deband_noise = shader_info_shadow_soft_deband_noise(position, light_vector) - 0.5f;
  visibility += deband_noise * deband_strength / max(effective_sample_count * 2.0f, 1.0f);
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
                      float lightgroup_hash_value,
                      out float4 diffuse_shading,
                      out float shadow,
                      out float4 ambient_lighting,
                      out float half_lambert_factor)
{
#if defined(GPU_FRAGMENT_SHADER) && (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
  float3 shading_normal = shader_info_resolve_normal(normal_in);
  float3 geometry_normal = shader_info_resolve_normal(g_data.Ng);
  float3 probe_bias_normal = shader_info_resolve_normal(g_data.Ni);
  float3 view_vector = drw_world_incident_vector(position);
  uint lightgroup_hash = floatBitsToUint(lightgroup_hash_value);

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
    if (light.lightgroup_hash != lightgroup_hash) {
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
  /* Use the interpolated surface normal for probe lookup bias so smooth-shaded meshes do not
   * inherit face-normal stepping from the volume probe receiver path. */
  LightProbeSample probe_sample = lightprobe_load(position, probe_bias_normal, view_vector);
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
