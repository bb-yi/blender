/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_filter_object_mask(float range_start, float range_count, out float mask, out float4 color)
{
  mask = 0.0f;
  color = float4(0.0f, 0.0f, 0.0f, 1.0f);

  int index_start = int(range_start);
  int count = int(range_count);
  int2 extent = textureSize(cryptomatte_tx, 0);
  if (count <= 0 || index_start < 0 || index_start >= FILTER_MASK_HASH_MAX ||
      any(lessThanEqual(extent, int2(1))))
  {
    return;
  }

  int2 texel = clamp(int2(gl_FragCoord.xy), int2(0), extent - int2(1));
  float4 current_layer = texelFetch(cryptomatte_tx, texel, 0);

  for (int offset = 0; offset < count && (index_start + offset) < FILTER_MASK_HASH_MAX; offset++) {
    float target_hash = filter_mask_hash_buf[index_start + offset].metadata.x;
    if (target_hash == 0.0f) {
      continue;
    }

    if (floatBitsToUint(current_layer.x) == floatBitsToUint(target_hash)) {
      mask = 1.0f;
      break;
    }
  }

  color = float4(mask, mask, mask, 1.0f);
}
