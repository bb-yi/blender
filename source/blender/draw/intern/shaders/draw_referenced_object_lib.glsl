/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/* Runtime lookup for Object data packed into the existing ObjectAttribute SSBO. */

uint referenced_object_data_find(uint session_uid)
{
  const uint invalid_record = 0xFFFFFFFFu;
  if (session_uid == 0u) {
    return invalid_record;
  }

  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  ObjectAttribute header = attrs_buf[0];
  if (header.hash_code != DRW_REFERENCED_OBJECT_DATA_MAGIC) {
    return invalid_record;
  }

  const uint abi_version = floatBitsToUint(header.data_x);
  const uint table_offset = floatBitsToUint(header.data_y);
  const uint table_size = floatBitsToUint(header.data_z);
  const uint record_stride = floatBitsToUint(header.data_w);
  if (abi_version != DRW_REFERENCED_OBJECT_DATA_ABI_VERSION || table_size == 0u ||
      record_stride != DRW_REFERENCED_OBJECT_DATA_RECORD_STRIDE ||
      (table_size & (table_size - 1u)) != 0u)
  {
    return invalid_record;
  }

  uint slot = hash_uint2(session_uid, 0u) & (table_size - 1u);
  for (uint probe = 0u; probe < table_size; probe++) {
    const uint record_index = table_offset + slot * record_stride;
    ObjectAttribute metadata = attrs_buf[record_index];
    if (metadata.hash_code == session_uid) {
      return record_index;
    }
    if (metadata.hash_code == 0u) {
      return invalid_record;
    }
    slot = (slot + 1u) & (table_size - 1u);
  }
  return invalid_record;
}

ObjectAttribute referenced_object_data_lane_at(uint record_index, uint lane)
{
  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  if (record_index == 0xFFFFFFFFu || lane >= 10u) {
    ObjectAttribute empty_lane;
    empty_lane.data_x = 0.0f;
    empty_lane.data_y = 0.0f;
    empty_lane.data_z = 0.0f;
    empty_lane.data_w = 0.0f;
    empty_lane.hash_code = 0u;
    return empty_lane;
  }
  return attrs_buf[record_index + lane];
}

ObjectAttribute referenced_object_data_lane(uint session_uid, uint lane)
{
  return referenced_object_data_lane_at(referenced_object_data_find(session_uid), lane);
}

uint referenced_object_data_object_type(uint session_uid)
{
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index == 0xFFFFFFFFu) {
    return 0u;
  }
  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  return floatBitsToUint(attrs_buf[record_index].data_x);
}

float referenced_object_data_light_type(uint session_uid)
{
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index == 0xFFFFFFFFu) {
    return -1.0f;
  }
  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  return float(floatBitsToInt(attrs_buf[record_index].data_y));
}

float3 referenced_object_data_location(uint session_uid)
{
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index == 0xFFFFFFFFu) {
    return float3(0.0f);
  }
  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  ObjectAttribute lane = attrs_buf[record_index + 4u];
  return float3(lane.data_x, lane.data_y, lane.data_z);
}

float3 referenced_object_data_rotation(uint session_uid)
{
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index == 0xFFFFFFFFu) {
    return float3(0.0f);
  }
  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  ObjectAttribute lane = attrs_buf[record_index + 5u];
  return float3(lane.data_x, lane.data_y, lane.data_z);
}

float3 referenced_object_data_scale(uint session_uid)
{
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index == 0xFFFFFFFFu) {
    return float3(1.0f);
  }
  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  ObjectAttribute lane = attrs_buf[record_index + 6u];
  return float3(lane.data_x, lane.data_y, lane.data_z);
}

float4 referenced_object_data_color(uint session_uid)
{
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index == 0xFFFFFFFFu) {
    return float4(0.0f);
  }
  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  ObjectAttribute lane = attrs_buf[record_index + 7u];
  return float4(lane.data_x, lane.data_y, lane.data_z, lane.data_w);
}

float4 referenced_object_data_light_payload(uint session_uid)
{
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index == 0xFFFFFFFFu) {
    return float4(0.0f);
  }
  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  ObjectAttribute lane = attrs_buf[record_index + 8u];
  return float4(lane.data_x, lane.data_y, lane.data_z, lane.data_w);
}

float4 referenced_object_data_light_shape(uint session_uid)
{
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index == 0xFFFFFFFFu) {
    return float4(0.0f);
  }
  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  ObjectAttribute lane = attrs_buf[record_index + 9u];
  return float4(lane.data_x, lane.data_y, lane.data_z, lane.data_w);
}

float referenced_object_data_visible(uint session_uid)
{
  const uint record_index = referenced_object_data_find(session_uid);
  if (record_index == 0xFFFFFFFFu) {
    return 0.0f;
  }
  const auto &attrs_buf = buffer_get(draw_object_attributes, drw_attrs);
  return floatBitsToUint(attrs_buf[record_index].data_z) == 0u ? 1.0f : 0.0f;
}
