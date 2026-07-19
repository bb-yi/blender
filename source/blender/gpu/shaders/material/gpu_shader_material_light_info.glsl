/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_info(float light_uid,
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
