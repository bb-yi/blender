/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_filter_object_info(float object_uid,
                             out float3 location,
                             out float3 rotation,
                             out float3 scale,
                             out float4 color)
{
  const uint session_uid = floatBitsToUint(object_uid);
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index == 0xFFFFFFFFu) {
    location = float3(0.0f);
    rotation = float3(0.0f);
    scale = float3(1.0f);
    color = float4(0.0f);
    return;
  }

  const ObjectAttribute location_lane = referenced_object_data_lane_at(record_index, 4u);
  const ObjectAttribute rotation_lane = referenced_object_data_lane_at(record_index, 5u);
  const ObjectAttribute scale_lane = referenced_object_data_lane_at(record_index, 6u);
  const ObjectAttribute color_lane = referenced_object_data_lane_at(record_index, 7u);
  location = float3(location_lane.data_x, location_lane.data_y, location_lane.data_z);
  rotation = float3(rotation_lane.data_x, rotation_lane.data_y, rotation_lane.data_z);
  scale = float3(scale_lane.data_x, scale_lane.data_y, scale_lane.data_z);
  color = float4(color_lane.data_x, color_lane.data_y, color_lane.data_z, color_lane.data_w);
}
