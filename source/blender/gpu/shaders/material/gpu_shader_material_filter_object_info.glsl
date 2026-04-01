/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_filter_object_info(float object_index,
                             out float3 location,
                             out float3 rotation,
                             out float3 scale,
                             out float4 color)
{
  location = float3(0.0f);
  rotation = float3(0.0f);
  scale = float3(1.0f);
  color = float4(0.0f);

  int index = int(object_index);
  if (index >= 0 && index < FILTER_OBJECT_INFO_MAX) {
    FilterObjectInfoData info = filter_object_buf[index];
    location = info.location.xyz;
    rotation = info.rotation.xyz;
    scale = info.scale.xyz;
    color = info.color;
  }
}
