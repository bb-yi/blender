/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_filter_object_mask(float object_index, out float mask)
{
  mask = 0.0f;

  int index = int(object_index);
  int2 extent = textureSize(cryptomatte_tx, 0);
  if (index < 0 || index >= FILTER_OBJECT_INFO_MAX || any(lessThanEqual(extent, int2(1)))) {
    return;
  }

  float target_hash = filter_object_buf[index].metadata.x;
  int2 texel = clamp(int2(gl_FragCoord.xy), int2(0), extent - int2(1));
  float current_hash = texelFetch(cryptomatte_tx, texel, 0).x;

  mask = (floatBitsToUint(current_hash) == floatBitsToUint(target_hash)) ? 1.0f : 0.0f;
}

[[node]]
void node_filter_object_mask_multi(float range_start, float range_count, out float mask)
{
  mask = 0.0f;

  int index_start = int(range_start);
  int count = int(range_count);
  int2 extent = textureSize(cryptomatte_tx, 0);
  if (count <= 0 || index_start < 0 || index_start >= FILTER_OBJECT_INFO_MAX ||
      any(lessThanEqual(extent, int2(1))))
  {
    return;
  }

  int2 texel = clamp(int2(gl_FragCoord.xy), int2(0), extent - int2(1));
  float current_hash = texelFetch(cryptomatte_tx, texel, 0).x;

  for (int offset = 0; offset < count && (index_start + offset) < FILTER_OBJECT_INFO_MAX; offset++) {
    float target_hash = filter_object_buf[index_start + offset].metadata.x;
    if (target_hash == 0.0f) {
      continue;
    }

    if (floatBitsToUint(current_hash) == floatBitsToUint(target_hash)) {
      mask = 1.0f;
      break;
    }
  }
}
