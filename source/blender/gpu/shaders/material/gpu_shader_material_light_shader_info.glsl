/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_eevee_light_shader_info(out float4 default_color,
                                  out float default_intensity,
                                  out float default_attenuation,
                                  out float3 world_position,
                                  out float3 direction,
                                  out float distance,
                                  out float3 light_space)
{
#if defined(MAT_LIGHT_SHADER)
  LightData light = light_buf[light_index];
  const bool is_directional = is_sun_light(light.type);
  LightVector light_vector = light_vector_get(light, is_directional, g_data.P);

  default_intensity = reduce_max(light.color);
  default_color = float4(default_intensity > 0.0f ? light.color / default_intensity :
                                                     float3(1.0f),
                         1.0f);
  default_attenuation = is_directional ?
                            1.0f :
                            light_influence_attenuation(
                                light_vector.dist,
                                light.local().local.influence_radius_invsqr_surface);
  world_position = is_directional ? float3(0.0f) : light_position_get(light);
  direction = is_directional ? light.sun().direction : light_z_axis(light);
  distance = light_vector.dist;
  light_space = light_world_to_local_point(light, g_data.P);
#else
  default_color = float4(1.0f);
  default_intensity = 1.0f;
  default_attenuation = 1.0f;
  world_position = float3(0.0f);
  direction = float3(0.0f, 0.0f, 1.0f);
  distance = 1.0f;
  light_space = float3(0.0f);
#endif
}
