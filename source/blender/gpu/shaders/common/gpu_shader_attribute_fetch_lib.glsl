/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_attribute_load_lib.glsl"

/**
 * Library to read vertex attributes of any format from a `gpu::Batch` bound as storage buffers
 * (see #GPU_batch_bind_as_resources). The storage buffer must be declared as an array of `uint`.
 * Each attribute requires the `gpu_attr_N` (stride and offset) and `gpu_attr_N_meta` (packed
 * format) push constants. Missing attributes have an empty format and are read as zero.
 */

/* Matches #GPUVertCompType. */
#define GPU_ATTR_COMP_I8 0
#define GPU_ATTR_COMP_U8 1
#define GPU_ATTR_COMP_I16 2
#define GPU_ATTR_COMP_U16 3
#define GPU_ATTR_COMP_I32 4
#define GPU_ATTR_COMP_U32 5
#define GPU_ATTR_COMP_F32 6
#define GPU_ATTR_COMP_I10 7
/* Matches #GPUVertFetchMode. */
#define GPU_ATTR_FETCH_INT_TO_FLOAT_UNIT 2

int gpu_attr_meta_comp_len(int meta)
{
  return meta & 0xFF;
}

int gpu_attr_meta_comp_type(int meta)
{
  return (meta >> 8) & 0xFF;
}

bool gpu_attr_meta_is_normalized(int meta)
{
  return ((meta >> 16) & 0xFF) == GPU_ATTR_FETCH_INT_TO_FLOAT_UNIT;
}

/** Number of 32bit words a single attribute value spans. */
int gpu_attr_meta_word_len(int meta)
{
  const int comp_len = gpu_attr_meta_comp_len(meta);
  switch (gpu_attr_meta_comp_type(meta)) {
    case GPU_ATTR_COMP_I8:
    case GPU_ATTR_COMP_U8:
      return (comp_len + 3) >> 2;
    case GPU_ATTR_COMP_I16:
    case GPU_ATTR_COMP_U16:
      return (comp_len + 1) >> 1;
    case GPU_ATTR_COMP_I10:
      return min(comp_len, 1);
    default:
      return comp_len;
  }
}

float gpu_attr_decode_signed(uint value, int bit_len, bool normalized)
{
  const int shift = 32 - bit_len;
  const int signed_value = (int(value) << shift) >> shift;
  if (normalized) {
    return max(float(signed_value) / float((1 << (bit_len - 1)) - 1), -1.0f);
  }
  return float(signed_value);
}

float gpu_attr_decode_unsigned(uint value, int bit_len, bool normalized)
{
  if (normalized) {
    return float(value) / float((1u << uint(bit_len)) - 1u);
  }
  return float(value);
}

float gpu_attr_decode_component(uint4 words, int meta, int c)
{
  const bool normalized = gpu_attr_meta_is_normalized(meta);
  switch (gpu_attr_meta_comp_type(meta)) {
    case GPU_ATTR_COMP_F32:
      return uintBitsToFloat(words[c]);
    case GPU_ATTR_COMP_U32:
      return float(words[c]);
    case GPU_ATTR_COMP_I32:
      return float(int(words[c]));
    case GPU_ATTR_COMP_U16:
      return gpu_attr_decode_unsigned((words[c >> 1] >> uint((c & 1) * 16)) & 0xFFFFu,
                                      16,
                                      normalized);
    case GPU_ATTR_COMP_I16:
      return gpu_attr_decode_signed(
          (words[c >> 1] >> uint((c & 1) * 16)) & 0xFFFFu, 16, normalized);
    case GPU_ATTR_COMP_U8:
      return gpu_attr_decode_unsigned((words.x >> uint(c * 8)) & 0xFFu, 8, normalized);
    case GPU_ATTR_COMP_I8:
      return gpu_attr_decode_signed((words.x >> uint(c * 8)) & 0xFFu, 8, normalized);
    case GPU_ATTR_COMP_I10:
      if (c == 3) {
        return gpu_attr_decode_signed(words.x >> 30u, 2, normalized);
      }
      return gpu_attr_decode_signed((words.x >> uint(c * 10)) & 0x3FFu, 10, normalized);
  }
  return 0.0f;
}

/** Decode up to 4 components. Missing components are set to zero. */
float4 gpu_attr_decode_float4(uint4 words, int meta)
{
  float4 value = float4(0.0f);
  const int comp_len = gpu_attr_meta_comp_len(meta);
  for (int c = 0; c < min(comp_len, 4); c++) {
    value[c] = gpu_attr_decode_component(words, meta, c);
  }
  return value;
}

/* Implemented as macros to avoid compiler differences with buffer qualifiers.
 * Only reads the words spanned by the attribute to avoid out of bound access. */
#define gpu_attr_fetch_word(_data, _desc, _meta, _i, _word) \
  ((gpu_attr_meta_word_len(_meta) > (_word)) ? \
       _data[gpu_attr_load_index(_i, _desc) + (_word)] : \
       0u)

#define gpu_attr_fetch_float4(_data, _desc, _meta, _i) \
  gpu_attr_decode_float4(uint4(gpu_attr_fetch_word(_data, _desc, _meta, _i, 0), \
                               gpu_attr_fetch_word(_data, _desc, _meta, _i, 1), \
                               gpu_attr_fetch_word(_data, _desc, _meta, _i, 2), \
                               gpu_attr_fetch_word(_data, _desc, _meta, _i, 3)), \
                         _meta)
