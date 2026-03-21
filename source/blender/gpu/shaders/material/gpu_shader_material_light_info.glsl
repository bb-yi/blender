/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_info(float3 light_color,
                     float light_power,
                     float light_type,
                     float3 light_position,
                     float3 light_direction,
                     float light_radius,
                     float light_spot_size,
                     float light_sun_angle,
                     out float4 color,
                     out float power,
                     out float type,
                     out float3 position,
                     out float3 direction,
                     out float radius,
                     out float spot_size,
                     out float sun_angle)
{
  color = float4(light_color, 1.0f);
  power = light_power;
  type = light_type;
  position = light_position;
  direction = light_direction;
  radius = light_radius;
  spot_size = light_spot_size;
  sun_angle = light_sun_angle;
}
