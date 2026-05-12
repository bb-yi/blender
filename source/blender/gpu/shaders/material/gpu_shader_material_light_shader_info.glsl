/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float light_shader_info_abs_sum(float3 value)
{
  return abs(value.x) + abs(value.y) + abs(value.z);
}

float3 light_shader_info_to_euler(float3x3 mat)
{
  float cy = sqrt(mat[0][0] * mat[0][0] + mat[0][1] * mat[0][1]);
  float3 eul1;
  float3 eul2;

  if (cy > 16.0f * 1.1920928955078125e-7f) {
    eul1.x = atan(mat[1][2], mat[2][2]);
    eul1.y = atan(-mat[0][2], cy);
    eul1.z = atan(mat[0][1], mat[0][0]);

    eul2.x = atan(-mat[1][2], -mat[2][2]);
    eul2.y = atan(-mat[0][2], -cy);
    eul2.z = atan(-mat[0][1], -mat[0][0]);
  }
  else {
    eul1.x = atan(-mat[2][1], mat[1][1]);
    eul1.y = atan(-mat[0][2], cy);
    eul1.z = 0.0f;
    eul2 = eul1;
  }

  return (light_shader_info_abs_sum(eul1) > light_shader_info_abs_sum(eul2)) ? eul2 : eul1;
}

[[node]]
void node_eevee_light_shader_info(out float4 default_color,
                                  out float default_intensity,
                                  out float default_attenuation,
                                  out float distance,
                                  out float3 light_space,
                                  out float3 direction,
                                  out float3 world_position,
                                  out float3 rotation)
{
#if defined(MAT_LIGHT_SHADER)
  LightData light = light_buf[light_index];
  const bool is_directional = is_sun_light(light.type);
  LightVector light_vector = light_vector_get(light, is_directional, g_data.P);

  default_intensity = reduce_max(light.color);
  default_color = float4(default_intensity > 0.0f ? light.color / default_intensity :
                                                     float3(1.0f),
                         1.0f);
  if (is_directional) {
    default_attenuation = 1.0f;
  }
  else {
    float default_influence_radius_invsqr =
#  if defined(MAT_LIGHT_SHADER_VOLUME)
        light.local().local.influence_radius_invsqr_volume;
#  else
        light.local().local.influence_radius_invsqr_surface;
#  endif
    default_attenuation = light_point_light(light, is_directional, light_vector) *
                          light_influence_attenuation(light_vector.dist,
                                                      default_influence_radius_invsqr);
  }
  distance = light_vector.dist;
  light_space = light_world_to_local_point(light, g_data.P);
  direction = light_vector.L;
  world_position = light_position_get(light);
  rotation = light_shader_info_to_euler(
      float3x3(light_x_axis(light), light_y_axis(light), light_z_axis(light)));
#else
  default_color = float4(1.0f);
  default_intensity = 1.0f;
  default_attenuation = 1.0f;
  distance = 1.0f;
  light_space = float3(0.0f);
  direction = float3(0.0f, 0.0f, 1.0f);
  world_position = float3(0.0f);
  rotation = float3(0.0f);
#endif
}
