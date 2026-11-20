/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_parameter_float(float fallback, float uid, float lo, float hi, float type,
                                out float value, out float valid)
{
  float4 data;
  bool found = light_shader_parameter_read(floatBitsToUint(uid), floatBitsToUint(lo),
                                           floatBitsToUint(hi), uint(type), data);
  valid = found ? 1.0f : 0.0f;
  value = found ? data.x : fallback;
}

[[node]]
void node_light_parameter_int(int fallback, float uid, float lo, float hi, float type,
                              out int value, out float valid)
{
  float4 data;
  bool found = light_shader_parameter_read(floatBitsToUint(uid), floatBitsToUint(lo),
                                           floatBitsToUint(hi), uint(type), data);
  valid = found ? 1.0f : 0.0f;
  value = found ? int(data.x) : fallback;
}

[[node]]
void node_light_parameter_bool(bool fallback, float uid, float lo, float hi, float type,
                               out bool value, out float valid)
{
  float4 data;
  bool found = light_shader_parameter_read(floatBitsToUint(uid), floatBitsToUint(lo),
                                           floatBitsToUint(hi), uint(type), data);
  valid = found ? 1.0f : 0.0f;
  value = found ? (data.x != 0.0f) : fallback;
}

[[node]]
void node_light_parameter_vec2(float2 fallback, float uid, float lo, float hi, float type,
                               out float2 value, out float valid)
{
  float4 data;
  bool found = light_shader_parameter_read(floatBitsToUint(uid), floatBitsToUint(lo),
                                           floatBitsToUint(hi), uint(type), data);
  valid = found ? 1.0f : 0.0f;
  value = found ? data.xy : fallback;
}

[[node]]
void node_light_parameter_vec3(float3 fallback, float uid, float lo, float hi, float type,
                               out float3 value, out float valid)
{
  float4 data;
  bool found = light_shader_parameter_read(floatBitsToUint(uid), floatBitsToUint(lo),
                                           floatBitsToUint(hi), uint(type), data);
  valid = found ? 1.0f : 0.0f;
  value = found ? data.xyz : fallback;
}

[[node]]
void node_light_parameter_vec4(float4 fallback, float uid, float lo, float hi, float type,
                               out float4 value, out float valid)
{
  float4 data;
  bool found = light_shader_parameter_read(floatBitsToUint(uid), floatBitsToUint(lo),
                                           floatBitsToUint(hi), uint(type), data);
  valid = found ? 1.0f : 0.0f;
  value = found ? data : fallback;
}

[[node]]
void node_light_info_transform(float light_uid,
                               out float3 origin,
                               out float3 x_axis,
                               out float3 y_axis,
                               out float3 z_axis)
{
  origin = float3(0.0f);
  x_axis = float3(0.0f);
  y_axis = float3(0.0f);
  z_axis = float3(0.0f);

  const uint record_index = referenced_object_data_find(floatBitsToUint(light_uid));
  if (record_index == 0xFFFFFFFFu) {
    return;
  }
  const ObjectAttribute metadata = referenced_object_data_lane_at(record_index, 0u);
  if (floatBitsToInt(metadata.data_y) < 0) {
    return;
  }

  /* Keep the evaluated matrix columns intact, including reflection and parent shear. */
  const ObjectAttribute x = referenced_object_data_lane_at(record_index, 1u);
  const ObjectAttribute y = referenced_object_data_lane_at(record_index, 2u);
  const ObjectAttribute z = referenced_object_data_lane_at(record_index, 3u);
  const ObjectAttribute t = referenced_object_data_lane_at(record_index, 4u);
  origin = float3(t.data_x, t.data_y, t.data_z);
  x_axis = float3(x.data_x, x.data_y, x.data_z);
  y_axis = float3(y.data_x, y.data_y, y.data_z);
  z_axis = float3(z.data_x, z.data_y, z.data_z);
}

[[node]]
void node_npr_light_info(float light_uid,
                     out float4 color,
                     out float power,
                     out float type,
                     out float3 position,
                     out float3 direction,
                     out float radius,
                     out float spot_size,
                     out float sun_angle,
                     out float visible)
{
  color = float4(0.0f);
  power = 0.0f;
  type = -1.0f;
  position = float3(0.0f);
  direction = float3(0.0f);
  radius = 0.0f;
  spot_size = 0.0f;
  sun_angle = 0.0f;
  visible = 0.0f;

  const uint session_uid = floatBitsToUint(light_uid);
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index != 0xFFFFFFFFu) {
    const ObjectAttribute metadata = referenced_object_data_lane_at(record_index, 0u);
    const float light_type = float(floatBitsToInt(metadata.data_y));
    if (light_type < 0.0f) {
      return;
    }

    const ObjectAttribute light_payload_lane = referenced_object_data_lane_at(record_index, 8u);
    const ObjectAttribute light_shape_lane = referenced_object_data_lane_at(record_index, 9u);
    const float4 light_payload = float4(light_payload_lane.data_x,
                                        light_payload_lane.data_y,
                                        light_payload_lane.data_z,
                                        light_payload_lane.data_w);
    const float4 light_shape = float4(light_shape_lane.data_x,
                                      light_shape_lane.data_y,
                                      light_shape_lane.data_z,
                                      light_shape_lane.data_w);
    color = float4(light_payload.xyz, 1.0f);
    power = light_payload.w;
    type = float(light_type);
    visible = floatBitsToUint(metadata.data_z) == 0u ? 1.0f : 0.0f;

    const ObjectAttribute z_axis_lane = referenced_object_data_lane_at(record_index, 3u);
    const float3 z_axis = float3(z_axis_lane.data_x, z_axis_lane.data_y, z_axis_lane.data_z);
    const bool has_direction = light_type != 0.0f && dot(z_axis, z_axis) > 1e-12f;
    if (has_direction) {
      direction = -normalize(z_axis);
    }

    if (light_type == 0.0f) {
      const ObjectAttribute position_lane = referenced_object_data_lane_at(record_index, 4u);
      position = float3(position_lane.data_x, position_lane.data_y, position_lane.data_z);
      radius = light_shape.x;
    }
    else if (light_type == 1.0f) {
      sun_angle = light_shape.z;
    }
    else if (light_type == 2.0f) {
      const ObjectAttribute position_lane = referenced_object_data_lane_at(record_index, 4u);
      position = float3(position_lane.data_x, position_lane.data_y, position_lane.data_z);
      radius = light_shape.x;
      spot_size = light_shape.y;
    }
    else if (light_type == 3.0f) {
      const ObjectAttribute position_lane = referenced_object_data_lane_at(record_index, 4u);
      position = float3(position_lane.data_x, position_lane.data_y, position_lane.data_z);
      radius = light_shape.x;
    }
  }
}
