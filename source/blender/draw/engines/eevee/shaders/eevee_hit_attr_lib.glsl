/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_attribute_load_lib.glsl"
#include "eevee_attributes_mesh_lib.glsl"

/* Only sparse replay needs triangle indices; ordinary raster domains stay unchanged. */
struct HitMeshVertex {
  MeshVertex mesh;
  int3 vertex_indices;
  float3 barycentric_weights;
};

#ifndef GPU_COMP_I8
#  define GPU_COMP_I8 0
#  define GPU_COMP_U8 1
#  define GPU_COMP_I16 2
#  define GPU_COMP_U16 3
#  define GPU_COMP_I32 4
#  define GPU_COMP_U32 5
#  define GPU_COMP_F32 6
#  define GPU_COMP_I10 7
#endif

#ifndef GPU_FETCH_FLOAT
#  define GPU_FETCH_FLOAT 0
#  define GPU_FETCH_INT 1
#  define GPU_FETCH_INT_TO_FLOAT_UNIT 2
#endif

int hit_attr_meta_comp_len(int meta)
{
  return meta & 0xFF;
}

int hit_attr_meta_comp_type(int meta)
{
  return (meta >> 8) & 0xFF;
}

int hit_attr_meta_fetch_mode(int meta)
{
  return (meta >> 16) & 0xFF;
}

int hit_attr_packed_word_count(int comp_len, int comp_type)
{
  switch (comp_type) {
    case GPU_COMP_I8:
    case GPU_COMP_U8:
      return (comp_len + 3) >> 2;
    case GPU_COMP_I16:
    case GPU_COMP_U16:
      return (comp_len + 1) >> 1;
    case GPU_COMP_I10:
      return 1;
    case GPU_COMP_I32:
    case GPU_COMP_U32:
    case GPU_COMP_F32:
    default:
      return comp_len;
  }
}

int hit_attr_sign_extend(uint value, int bit_count)
{
  int shift = 32 - bit_count;
  return (int(value) << shift) >> shift;
}

float hit_attr_snorm(int value, int max_positive)
{
  return clamp(float(value) / float(max_positive), -1.0f, 1.0f);
}

float hit_attr_unorm(uint value, uint max_value)
{
  return float(value) / float(max_value);
}

float hit_attr_fetch_component_from_words(int comp_type,
                                          int fetch_mode,
                                          int component,
                                          uint data0,
                                          uint data1,
                                          uint data2,
                                          uint data3)
{
  switch (comp_type) {
    case GPU_COMP_F32: {
      switch (component) {
        case 0:
          return uintBitsToFloat(data0);
        case 1:
          return uintBitsToFloat(data1);
        case 2:
          return uintBitsToFloat(data2);
        default:
          return uintBitsToFloat(data3);
      }
    }
    case GPU_COMP_U32: {
      switch (component) {
        case 0:
          return float(data0);
        case 1:
          return float(data1);
        case 2:
          return float(data2);
        default:
          return float(data3);
      }
    }
    case GPU_COMP_I32: {
      switch (component) {
        case 0:
          return float(int(data0));
        case 1:
          return float(int(data1));
        case 2:
          return float(int(data2));
        default:
          return float(int(data3));
      }
    }
    case GPU_COMP_U16: {
      uint packed_data = (component < 2) ? data0 : (component < 4) ? data1 : 0u;
      uint value = ((component & 1) == 0) ? (packed_data & 0xFFFFu) : (packed_data >> 16u);
      return (fetch_mode == GPU_FETCH_INT_TO_FLOAT_UNIT) ? hit_attr_unorm(value, 0xFFFFu) :
                                                           float(value);
    }
    case GPU_COMP_I16: {
      uint packed_data = (component < 2) ? data0 : (component < 4) ? data1 : 0u;
      uint value = ((component & 1) == 0) ? (packed_data & 0xFFFFu) : (packed_data >> 16u);
      int signed_value = hit_attr_sign_extend(value, 16);
      return (fetch_mode == GPU_FETCH_INT_TO_FLOAT_UNIT) ? hit_attr_snorm(signed_value, 0x7FFF) :
                                                           float(signed_value);
    }
    case GPU_COMP_U8: {
      uint packed_data = (component < 4) ? data0 : 0u;
      uint value = (packed_data >> uint(component * 8)) & 0xFFu;
      return (fetch_mode == GPU_FETCH_INT_TO_FLOAT_UNIT) ? hit_attr_unorm(value, 0xFFu) :
                                                           float(value);
    }
    case GPU_COMP_I8: {
      uint packed_data = (component < 4) ? data0 : 0u;
      uint value = (packed_data >> uint(component * 8)) & 0xFFu;
      int signed_value = hit_attr_sign_extend(value, 8);
      return (fetch_mode == GPU_FETCH_INT_TO_FLOAT_UNIT) ? hit_attr_snorm(signed_value, 0x7F) :
                                                           float(signed_value);
    }
    case GPU_COMP_I10: {
      if (fetch_mode == GPU_FETCH_INT_TO_FLOAT_UNIT) {
        return gpu_attr_decode_1010102_snorm(data0)[component];
      }
      const int bit_count = (component == 3) ? 2 : 10;
      const uint shift = uint((component == 3) ? 30 : (component * 10));
      const uint mask = (component == 3) ? 0x3u : 0x3FFu;
      return float(hit_attr_sign_extend((data0 >> shift) & mask, bit_count));
    }
  }

  return 0.0f;
}

float4 hit_attr_fetch_float4_from_words(int comp_len,
                                        int comp_type,
                                        int fetch_mode,
                                        uint data0,
                                        uint data1,
                                        uint data2,
                                        uint data3)
{
  float4 value = float4(0.0f);
  if (comp_len > 0) {
    value.x = hit_attr_fetch_component_from_words(comp_type, fetch_mode, 0, data0, data1, data2, data3);
  }
  if (comp_len > 1) {
    value.y = hit_attr_fetch_component_from_words(comp_type, fetch_mode, 1, data0, data1, data2, data3);
  }
  if (comp_len > 2) {
    value.z = hit_attr_fetch_component_from_words(comp_type, fetch_mode, 2, data0, data1, data2, data3);
  }
  if (comp_len > 3) {
    value.w = hit_attr_fetch_component_from_words(comp_type, fetch_mode, 3, data0, data1, data2, data3);
  }
  return value;
}

#define hit_attr_base_index(_desc, _index) gpu_attr_load_index(uint(_index), (_desc))
#define hit_attr_word_count(_meta) \
  hit_attr_packed_word_count(hit_attr_meta_comp_len(_meta), hit_attr_meta_comp_type(_meta))
#define hit_attr_word_load(_data, _base, _word_index, _word_count) \
  (((_word_index) < (_word_count)) ? (_data)[(_base) + (_word_index)] : 0u)

#define hit_attr_fetch_float4(_data, _desc, _meta, _index) \
  hit_attr_fetch_float4_from_words(hit_attr_meta_comp_len(_meta), \
                                   hit_attr_meta_comp_type(_meta), \
                                   hit_attr_meta_fetch_mode(_meta), \
                                   hit_attr_word_load(_data, \
                                                      hit_attr_base_index(_desc, _index), \
                                                      0, \
                                                      hit_attr_word_count(_meta)), \
                                   hit_attr_word_load(_data, \
                                                      hit_attr_base_index(_desc, _index), \
                                                      1, \
                                                      hit_attr_word_count(_meta)), \
                                   hit_attr_word_load(_data, \
                                                      hit_attr_base_index(_desc, _index), \
                                                      2, \
                                                      hit_attr_word_count(_meta)), \
                                   hit_attr_word_load(_data, \
                                                      hit_attr_base_index(_desc, _index), \
                                                      3, \
                                                      hit_attr_word_count(_meta)))
#define hit_attr_fetch_float3(_data, _desc, _meta, _index) \
  hit_attr_fetch_float4(_data, _desc, _meta, _index).xyz
#define hit_attr_fetch_float2(_data, _desc, _meta, _index) \
  hit_attr_fetch_float4(_data, _desc, _meta, _index).xy
#define hit_attr_fetch_float(_data, _desc, _meta, _index) \
  hit_attr_fetch_float4(_data, _desc, _meta, _index).x

#define hit_attr_fetch_float4_interp(_data, _desc, _meta, _domain) \
  (hit_attr_fetch_float4(_data, _desc, _meta, (_domain).vertex_indices.x) * \
       (_domain).barycentric_weights.x + \
   hit_attr_fetch_float4(_data, _desc, _meta, (_domain).vertex_indices.y) * \
       (_domain).barycentric_weights.y + \
   hit_attr_fetch_float4(_data, _desc, _meta, (_domain).vertex_indices.z) * \
       (_domain).barycentric_weights.z)
#define hit_attr_fetch_float3_interp(_data, _desc, _meta, _domain) \
  hit_attr_fetch_float4_interp(_data, _desc, _meta, _domain).xyz
#define hit_attr_fetch_float2_interp(_data, _desc, _meta, _domain) \
  hit_attr_fetch_float4_interp(_data, _desc, _meta, _domain).xy
#define hit_attr_fetch_float_interp(_data, _desc, _meta, _domain) \
  hit_attr_fetch_float4_interp(_data, _desc, _meta, _domain).x

#define hit_attr_load_orco(_domain, _data, _desc, _meta, _index) \
  attr_load_orco((_domain).mesh, hit_attr_fetch_float4_interp(_data, _desc, _meta, _domain), _index)
#define hit_attr_load_orco_hit_eval(_domain, _data, _desc, _meta, _index) \
  ((((_desc).x == 0) || (hit_attr_meta_comp_len(_meta) == 0) || \
    ((hit_attr_fetch_float4_interp(_data, _desc, _meta, _domain)).w == 1.0f)) ? \
       (_domain).mesh.orco_default : \
       ((hit_attr_fetch_float4_interp(_data, _desc, _meta, _domain)).xyz * 0.5f + 0.5f))
#define hit_attr_load_tangent(_domain, _data, _desc, _meta, _index) \
  attr_load_tangent((_domain).mesh, hit_attr_fetch_float4_interp(_data, _desc, _meta, _domain), _index)
#define hit_attr_load_float4(_domain, _data, _desc, _meta, _index) \
  attr_load_float4((_domain).mesh, hit_attr_fetch_float4_interp(_data, _desc, _meta, _domain), _index)
#define hit_attr_load_float3(_domain, _data, _desc, _meta, _index) \
  attr_load_float3((_domain).mesh, hit_attr_fetch_float3_interp(_data, _desc, _meta, _domain), _index)
#define hit_attr_load_float2(_domain, _data, _desc, _meta, _index) \
  attr_load_float2((_domain).mesh, hit_attr_fetch_float2_interp(_data, _desc, _meta, _domain), _index)
#define hit_attr_load_float(_domain, _data, _desc, _meta, _index) \
  attr_load_float((_domain).mesh, hit_attr_fetch_float_interp(_data, _desc, _meta, _domain), _index)
