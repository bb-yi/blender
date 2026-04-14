/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

bool glsl_light_is_zero(float3 value)
{
  return all(lessThanEqual(abs(value), float3(1e-8f)));
}

float3 glsl_light_resolve_normal(float3 normal_value, float3 fallback_normal)
{
  float normal_len_squared = dot(normal_value, normal_value);
  if (normal_len_squared > 1e-16f) {
    return normal_value * inversesqrt(normal_len_squared);
  }

  float fallback_len_squared = dot(fallback_normal, fallback_normal);
  if (fallback_len_squared > 1e-16f) {
    return fallback_normal * inversesqrt(fallback_len_squared);
  }

  return float3(0.0f, 0.0f, 1.0f);
}

#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_GLSL_LIGHT_ACCESS) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD))

bool glsl_light_receiver_accepts(LightData light)
{
  ObjectInfos object_infos = drw_infos[drw_resource_id()];
  uchar receiver_light_set = receiver_light_set_get(object_infos);
  return light_linking_affects_receiver(light.light_set_membership, receiver_light_set);
}

bool glsl_light_is_accessible(LightData light)
{
  return !glsl_light_is_zero(light.color) && glsl_light_receiver_accepts(light);
}

bool glsl_light_loop_accept(uint light_index, bool is_local)
{
  UNUSED_VARS(is_local);
  return glsl_light_is_accessible(light_buf[light_index]);
}

float3 glsl_light_color(uint light_index)
{
  LightData light = light_buf[light_index];
  return glsl_light_is_accessible(light) ? light.color : float3(0.0f);
}

float3 glsl_light_vector(uint light_index, bool is_local)
{
  LightData light = light_buf[light_index];
  if (!glsl_light_is_accessible(light)) {
    return float3(0.0f);
  }

  LightVector lv = light_vector_get(light, !is_local, g_data.P);
  return lv.L;
}

float glsl_light_distance(uint light_index, bool is_local)
{
  LightData light = light_buf[light_index];
  if (!glsl_light_is_accessible(light)) {
    return 0.0f;
  }

  LightVector lv = light_vector_get(light, !is_local, g_data.P);
  return lv.dist;
}

float glsl_light_diffuse_power(uint light_index)
{
  LightData light = light_buf[light_index];
  return glsl_light_is_accessible(light) ? light_power_get(light, LIGHT_DIFFUSE) : 0.0f;
}

float glsl_light_specular_power(uint light_index)
{
  LightData light = light_buf[light_index];
  return glsl_light_is_accessible(light) ? light_power_get(light, LIGHT_SPECULAR) : 0.0f;
}

float glsl_light_surface_attenuation(uint light_index, bool is_local)
{
  LightData light = light_buf[light_index];
  if (!glsl_light_is_accessible(light)) {
    return 0.0f;
  }

  LightVector lv = light_vector_get(light, !is_local, g_data.P);
  return light_attenuation_surface(light, !is_local, lv);
}

float glsl_light_shadow_visibility(uint light_index, bool is_local, float3 shading_normal)
{
  LightData light = light_buf[light_index];
  if (!glsl_light_is_accessible(light)) {
    return 0.0f;
  }
  if (light.tilemap_index == LIGHT_NO_SHADOW) {
    return 1.0f;
  }

  ObjectInfos object_infos = drw_infos[drw_resource_id()];
  float3 geometry_normal = glsl_light_resolve_normal(g_data.Ng, float3(0.0f, 0.0f, 1.0f));
  float3 resolved_shading_normal = glsl_light_resolve_normal(shading_normal, geometry_normal);

  return shadow_eval(light,
                     !is_local,
                     false,
                     false,
                     0.0f,
                     g_data.P,
                     geometry_normal,
                     resolved_shading_normal,
                     object_infos.shadow_terminator_normal_offset,
                     object_infos.shadow_terminator_geometry_offset,
                     uniform_buf.shadow.ray_count,
                     uniform_buf.shadow.step_count);
}

#  define GLSL_LIGHT_FOREACH_BEGIN(_light_index, _is_local) \
    LIGHT_FOREACH_ALL_BEGIN(light_cull_buf, \
                            light_zbin_buf, \
                            light_tile_buf, \
                            gl_FragCoord.xy, \
                            drw_point_world_to_view(g_data.P).z, \
                            _light_index, \
                            _is_local) \
    if (!glsl_light_loop_accept(_light_index, _is_local)) \
    { \
      continue; \
    }

#  define GLSL_LIGHT_FOREACH_END() LIGHT_FOREACH_ALL_END()

#else

float3 glsl_light_color(uint light_index)
{
  UNUSED_VARS(light_index);
  return float3(0.0f);
}

float3 glsl_light_vector(uint light_index, bool is_local)
{
  UNUSED_VARS(light_index, is_local);
  return float3(0.0f);
}

float glsl_light_distance(uint light_index, bool is_local)
{
  UNUSED_VARS(light_index, is_local);
  return 0.0f;
}

float glsl_light_diffuse_power(uint light_index)
{
  UNUSED_VARS(light_index);
  return 0.0f;
}

float glsl_light_specular_power(uint light_index)
{
  UNUSED_VARS(light_index);
  return 0.0f;
}

float glsl_light_surface_attenuation(uint light_index, bool is_local)
{
  UNUSED_VARS(light_index, is_local);
  return 0.0f;
}

float glsl_light_shadow_visibility(uint light_index, bool is_local, float3 shading_normal)
{
  UNUSED_VARS(light_index, is_local, shading_normal);
  return 0.0f;
}

#  define GLSL_LIGHT_FOREACH_BEGIN(_light_index, _is_local) \
    if (false) { \
      uint _light_index = 0u; \
      bool _is_local = false;

#  define GLSL_LIGHT_FOREACH_END() }

#endif
