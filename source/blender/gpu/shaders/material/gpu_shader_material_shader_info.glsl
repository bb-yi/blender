/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_material_shader_info_shared.glsl"

bool shader_info_is_world_sun_light(uint light_index, LightData light, bool is_local)
{
  if (is_local || !is_sun_light(light.type)) {
    return false;
  }

  uint directional_index = light_index - light_cull_buf.local_lights_len;
  if (directional_index >= WORLD_SUN_MAX) {
    return false;
  }

  [[resource_table]] const eevee::LightRenderData &light_data = resource_table_get(
      eevee::LightRenderData);
  LightData world_sun = light_data.sunlight_buf[directional_index];
  if (shader_info_is_zero(world_sun.color)) {
    return false;
  }

  float3 world_sun_direction = world_sun.object_to_world.z_axis();
  float color_delta = length(light.color - world_sun.color);
  float direction_alignment = dot(light.sun().direction, world_sun_direction);
  return (color_delta < 1e-4f) && (direction_alignment > 0.9999f);
}

bool shader_info_apply_light_shader(uint light_index,
                                    LightData &light,
                                    LightVector lv,
                                    bool is_directional,
                                    float &attenuation)
{
#if defined(LIGHT_SHADER_TEXTURE_EVAL)
  int light_shader_index = light_shader_index_buf[light_index];
  int light_shader_uniform_index = (light_shader_index < -1) ? -light_shader_index - 2 : -1;
  float4 light_shader;
  if (light_shader_uniform_index >= 0) {
    light_shader = light_shader_uniform_buf[light_shader_uniform_index];
  }
  else if (light_shader_index >= 0) {
    light_shader = texelFetch(light_shader_tx, int3(int2(gl_FragCoord.xy), light_shader_index), 0);
  }
  else {
    return false;
  }

  light.color = light_shader.rgb;
  attenuation = light_attenuation_common(light, is_directional, lv.L) * light_shader.a;
  if (!is_directional) {
    attenuation *= light_influence_cutoff(lv.dist,
                                          light.local().local.influence_radius_invsqr_surface);
  }
  return true;
#else
  return false;
#endif
}

float shader_info_light_ltc(LightData light,
                            LightVector lv,
                            float3 normal,
                            float3 view_vector,
                            float4 ltc_mat,
                            bool light_shader_no_distance_falloff,
                            bool is_directional)
{
  LightVertices light_shape_vertices = light_shape_corners(light, lv);
  float ltc_result = light_ltc(utility_tx, light, normal, view_vector, lv, ltc_mat, light_shape_vertices);
  if (light_shader_no_distance_falloff) {
    ltc_result /= max(light_point_light(light, is_directional, lv), 1e-8f);
  }
  return ltc_result;
}

void shader_info_eval_light(uint l_idx,
                            bool is_local,
                            float3 position,
                            float3 shading_normal,
                            float3 geometry_normal,
                            float3 view_vector,
                            float safe_exponent,
                            int lightgroup_id,
                            uchar receiver_light_set,
                            float normal_offset,
                            float geometry_offset,
                            float shadow_mode,
                            float stable_shadow_samples,
                            float3 &diffuse_shading_sum,
                            float &visibility_sum,
                            float &self_shadow_sum,
                            float &cast_shadow_sum,
                            float &shadow_weight_sum,
                            float &half_lambert_sum,
                            float &blinn_phong_sum)
{
  LightData light = light_buf[l_idx];
  bool is_directional = !is_local;

  if (!shader_info_light_linking_affects_receiver(light.light_set_membership,
                                                  receiver_light_set))
  {
    return;
  }
  if (light.lightgroup_id != lightgroup_id) {
    return;
  }

  LightVector lv = light_vector_get(light, is_directional, position);
  bool is_world_sun = shader_info_is_world_sun_light(l_idx, light, is_local);
  if (is_world_sun) {
    return;
  }

  float surface_attenuation = light_attenuation_surface(light, is_directional, lv);
  bool light_shader_no_distance_falloff = shader_info_apply_light_shader(
      l_idx, light, lv, is_directional, surface_attenuation);

  if (shader_info_is_zero(light.color) || surface_attenuation < LIGHT_ATTENUATION_THRESHOLD) {
    return;
  }

  float diffuse_power = light_shader_no_distance_falloff ?
                            shader_info_light_friendly_power_get(light, LIGHT_DIFFUSE) :
                            shader_info_light_power_get(light, LIGHT_DIFFUSE);
  float specular_power = light_shader_no_distance_falloff ?
                             shader_info_light_friendly_power_get(light, LIGHT_SPECULAR) :
                             shader_info_light_power_get(light, LIGHT_SPECULAR);
  if (max(diffuse_power, specular_power) < LIGHT_ATTENUATION_THRESHOLD) {
    return;
  }

  float ndotl = dot(shading_normal, lv.L);
  float half_lambert = saturate(ndotl * 0.5f + 0.5f);

  float diffuse_weight = diffuse_power * shader_info_max_component(light.color);
  if (diffuse_power >= LIGHT_ATTENUATION_THRESHOLD) {
    float4 ltc_mat = float4(1.0f, 0.0f, 0.0f, 1.0f);
    /* Shader Info diffuse should stay on the same light-range envelope as Eevee's light culling
     * and shadow visibility paths. Omitting surface attenuation keeps the LTC response alive past
     * the intended local-light falloff and can show up as visibly blocky light-range boundaries. */
    float diffuse_radiance = shader_info_light_ltc(light,
                                                   lv,
                                                   shading_normal,
                                                   view_vector,
                                                   ltc_mat,
                                                   light_shader_no_distance_falloff,
                                                   is_directional) *
                             surface_attenuation;

    /* Keep Shader Info diffuse unshadowed and undisplay-remapped. */
    diffuse_shading_sum += light.color * diffuse_power * diffuse_radiance;
    half_lambert_sum += half_lambert * surface_attenuation;
  }

  if (specular_power >= LIGHT_ATTENUATION_THRESHOLD) {
    float3 half_vector = safe_normalize(lv.L + view_vector);
    float nh = saturate(dot(shading_normal, half_vector));
    float blinn_phong = pow(nh, safe_exponent);
    blinn_phong_sum += blinn_phong * surface_attenuation;
  }

  if (diffuse_power >= LIGHT_ATTENUATION_THRESHOLD &&
      surface_attenuation > LIGHT_ATTENUATION_THRESHOLD)
  {
#if defined(SHADOW_CASTER_CLASSIFY)
    ShaderInfoShadowClassification classification = shader_info_shadow_classified_visibility(
        light,
        is_directional,
        lv.L,
        position,
        geometry_normal,
        shading_normal,
        normal_offset,
        geometry_offset,
        shadow_mode,
        stable_shadow_samples);
    float visibility = classification.visibility;
    self_shadow_sum += classification.self_shadow * surface_attenuation * diffuse_weight;
    cast_shadow_sum += classification.cast_shadow * surface_attenuation * diffuse_weight;
#else
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
#endif
    float shadow_visibility = visibility * surface_attenuation;
    visibility_sum += shadow_visibility * diffuse_weight;
    shadow_weight_sum += diffuse_weight;
  }
}

#if defined(CREATE_INFO_eevee_LightprobeRenderData)
float4 shader_info_ambient_lighting(float3 position,
                                    float3 probe_bias_normal,
                                    float3 view_vector,
                                    float3 shading_normal)
{
  /* Use the interpolated surface normal for probe lookup bias so smooth-shaded meshes do not
   * inherit face-normal stepping from the volume probe receiver path. */
  [[resource_table]] const eevee::LightprobeRenderData &lightprobes = resource_table_get(
      eevee::LightprobeRenderData);
  eevee::LightProbeSample probe_sample = lightprobes.load(
      gl_FragCoord.xy, position, probe_bias_normal, view_vector);
  probe_sample.volume_irradiance = spherical_harmonics::clamp_energy(
      probe_sample.volume_irradiance, uniform_buf.clamp.surface_indirect);
  float3 ambient = probe_sample.volume_irradiance.evaluate_lambert(shading_normal).rgb;
  return float4(max(ambient, float3(0.0f)), 1.0f);
}
#endif

[[node]]
void node_shader_info(float3 position,
                      float3 normal_in,
                      float exponent,
                      float shadow_mode,
                      float stable_shadow_samples,
                      float lightgroup_id_value,
                      out float4 diffuse_shading,
                      out float shadow,
                      out float4 ambient_lighting,
                      out float half_lambert_factor,
                      out float blinn_phong_factor,
                      out float self_shadow,
                      out float cast_shadow)
{
#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER) || \
     defined(MAT_BAKE_COLOR))
  float3 shading_normal = shader_info_resolve_normal(normal_in);
  float3 geometry_normal = shader_info_resolve_normal(g_data.Ng);
  float3 probe_bias_normal = shader_info_resolve_normal(g_data.Ni);
  const ViewMatrices view = view_matrices_get();
  float3 view_vector = view.world_incident_vector(position);
  float safe_exponent = max(exponent, 1.0f);
  int lightgroup_id = int(round(lightgroup_id_value));

  ObjectInfos object_infos = object_infos_get();
  uchar receiver_light_set = receiver_light_set_get(object_infos);
  float normal_offset = object_infos.shadow_terminator_normal_offset;
  float geometry_offset = object_infos.shadow_terminator_geometry_offset;

  float3 diffuse_shading_sum = float3(0.0f);
  float visibility_sum = 0.0f;
  float self_shadow_sum = 0.0f;
  float cast_shadow_sum = 0.0f;
  float shadow_weight_sum = 0.0f;
  float half_lambert_sum = 0.0f;
  float blinn_phong_sum = 0.0f;

  for (uint l_idx = light_cull_buf.local_lights_len; l_idx < light_cull_buf.items_count; l_idx++) {
    shader_info_eval_light(l_idx,
                           false,
                           position,
                           shading_normal,
                           geometry_normal,
                           view_vector,
                           safe_exponent,
                           lightgroup_id,
                           receiver_light_set,
                           normal_offset,
                           geometry_offset,
                           shadow_mode,
                           stable_shadow_samples,
                           diffuse_shading_sum,
                           visibility_sum,
                           self_shadow_sum,
                           cast_shadow_sum,
                           shadow_weight_sum,
                           half_lambert_sum,
                           blinn_phong_sum);
  }
  for (uint l_idx = 0u; l_idx < light_cull_buf.visible_count; l_idx++) {
    shader_info_eval_light(l_idx,
                           true,
                           position,
                           shading_normal,
                           geometry_normal,
                           view_vector,
                           safe_exponent,
                           lightgroup_id,
                           receiver_light_set,
                           normal_offset,
                           geometry_offset,
                           shadow_mode,
                           stable_shadow_samples,
                           diffuse_shading_sum,
                           visibility_sum,
                           self_shadow_sum,
                           cast_shadow_sum,
                           shadow_weight_sum,
                           half_lambert_sum,
                           blinn_phong_sum);
  }

  diffuse_shading = float4(diffuse_shading_sum, 1.0f);
  shadow = (shadow_weight_sum > 1e-8f) ? saturate(visibility_sum / shadow_weight_sum) : 0.0f;
  self_shadow = (shadow_weight_sum > 1e-8f) ?
                    1.0f - saturate(self_shadow_sum / shadow_weight_sum) :
                    1.0f;
  cast_shadow = (shadow_weight_sum > 1e-8f) ?
                    1.0f - saturate(cast_shadow_sum / shadow_weight_sum) :
                    1.0f;

#  if defined(CREATE_INFO_eevee_LightprobeRenderData)
  ambient_lighting = shader_info_ambient_lighting(
      position, probe_bias_normal, view_vector, shading_normal);
#  else
  ambient_lighting = float4(0.0f);
#  endif

  half_lambert_factor = saturate(half_lambert_sum);
  blinn_phong_factor = saturate(blinn_phong_sum);
#else
  diffuse_shading = float4(0.0f);
  shadow = 1.0f;
  ambient_lighting = float4(0.0f);
  half_lambert_factor = 0.0f;
  blinn_phong_factor = 0.0f;
  self_shadow = 1.0f;
  cast_shadow = 1.0f;
#endif
}

[[node]]
void node_shader_info(float3 position,
                      float3 normal_in,
                      float exponent,
                      float shadow_mode,
                      float stable_shadow_samples,
                      float lightgroup_id_value,
                      out float4 diffuse_shading,
                      out float shadow,
                      out float4 ambient_lighting,
                      out float half_lambert_factor,
                      out float blinn_phong_factor)
{
  float self_shadow;
  float cast_shadow;
  node_shader_info(position,
                   normal_in,
                   exponent,
                   shadow_mode,
                   stable_shadow_samples,
                   lightgroup_id_value,
                   diffuse_shading,
                   shadow,
                   ambient_lighting,
                   half_lambert_factor,
                   blinn_phong_factor,
                   self_shadow,
                   cast_shadow);
}
