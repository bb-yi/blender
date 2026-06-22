/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_defines.hh"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

/* Outline info is stored in integer channels to avoid Vulkan storage-image issues with UNORM
 * formats. Reserve the top width code-point so unpacking never hits 1.0 exactly and can
 * represent very large widths. */
#define OUTLINE_INFO_CODE_MAX 65535u
#define OUTLINE_WIDTH_PACK_MAX 65534u

uint outline_unorm16_pack(float value)
{
  return uint(saturate(value) * float(OUTLINE_INFO_CODE_MAX) + 0.5f);
}

uint outline_unorm_pack(float value, uint max_value)
{
  return uint(saturate(value) * float(max_value) + 0.5f);
}

uint outline_width_pack(float width)
{
  width = max(width, 0.0f);
  if (width <= 0.0f) {
    return 0u;
  }
  return min(outline_unorm16_pack(width / (width + 1.0f)), OUTLINE_WIDTH_PACK_MAX);
}

float outline_width_unpack(uint width_packed)
{
  width_packed = min(width_packed, OUTLINE_WIDTH_PACK_MAX);
  if (width_packed == 0u) {
    return 0.0f;
  }
  const float width_normalized = float(width_packed) / float(OUTLINE_INFO_CODE_MAX);
  return width_normalized / (1.0f - width_normalized);
}

#define OUTLINE_THRESHOLD_VALUE_MASK OUTLINE_INFO_CODE_MAX
#define OUTLINE_WIDTH_VARIATION_MASK 255u
#define OUTLINE_WIDTH_VARIATION_SHIFT 16u

uint outline_depth_threshold_pack(float threshold, float width_variation)
{
  return outline_unorm16_pack(threshold) |
         (outline_unorm_pack(width_variation, OUTLINE_WIDTH_VARIATION_MASK)
          << OUTLINE_WIDTH_VARIATION_SHIFT);
}

float outline_depth_threshold_unpack(uint threshold_packed)
{
  return float(threshold_packed & OUTLINE_THRESHOLD_VALUE_MASK) /
         float(OUTLINE_THRESHOLD_VALUE_MASK);
}

float outline_width_variation_unpack(uint threshold_packed)
{
  return float((threshold_packed >> OUTLINE_WIDTH_VARIATION_SHIFT) &
               OUTLINE_WIDTH_VARIATION_MASK) /
         float(OUTLINE_WIDTH_VARIATION_MASK);
}

#define OUTLINE_ID_VALUE_MASK 32767u
#define OUTLINE_ID_EDGE_BIT 32768u
#define OUTLINE_NORMAL_THRESHOLD_VALUE_MASK OUTLINE_INFO_CODE_MAX
#define OUTLINE_FREESTYLE_EDGE_BIT 2147483648u

uint outline_id_pack(uint outline_id, bool id_edge)
{
  uint packed_id = min(outline_id, OUTLINE_ID_VALUE_MASK);
  if (id_edge) {
    packed_id |= OUTLINE_ID_EDGE_BIT;
  }
  return packed_id;
}

uint outline_id_unpack(uint outline_id_packed)
{
  return outline_id_packed & OUTLINE_ID_VALUE_MASK;
}

bool outline_id_edge_unpack(uint outline_id_packed)
{
  return (outline_id_packed & OUTLINE_ID_EDGE_BIT) != 0u;
}

uint outline_normal_threshold_pack(float threshold, bool freestyle_edge)
{
  uint packed_threshold = uint(saturate(threshold) *
                                   float(OUTLINE_NORMAL_THRESHOLD_VALUE_MASK) +
                               0.5f);
  if (freestyle_edge) {
    packed_threshold |= OUTLINE_FREESTYLE_EDGE_BIT;
  }
  return packed_threshold;
}

float outline_normal_threshold_unpack(uint threshold_packed)
{
  return float(threshold_packed & OUTLINE_NORMAL_THRESHOLD_VALUE_MASK) /
         float(OUTLINE_NORMAL_THRESHOLD_VALUE_MASK);
}

bool outline_freestyle_edge_unpack(uint threshold_packed)
{
  return (threshold_packed & OUTLINE_FREESTYLE_EDGE_BIT) != 0u;
}

#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_CLEAR) && defined(GPU_FRAGMENT_SHADER)
float4 g_outline_staged_color;
uint4 g_outline_staged_info;
#endif

void outline_output_reset()
{
#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_CLEAR) && defined(GPU_FRAGMENT_SHADER)
  g_outline_staged_color = float4(0.0f);
  g_outline_staged_info = uint4(0u);
#endif
}

void outline_output_flush()
{
#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_CLEAR) && defined(GPU_FRAGMENT_SHADER)
  if (g_outline_staged_color.a <= 0.0f || g_outline_staged_info.r == 0u) {
    return;
  }

  int2 texel = int2(gl_FragCoord.xy);
  imageStoreFast(outline_color_img, texel, g_outline_staged_color);
  imageStoreFast(outline_info_img, texel, g_outline_staged_info);
#endif
}

float outline_pixel_world_size_at(float depth, int2 extent, int2 texel)
{
  float2 uv = (float2(texel) + 0.5f) / float2(extent);
  int2 next_texel = min(texel + int2(1, 0), extent - int2(1));
  float2 next_uv = (float2(next_texel) + 0.5f) / float2(extent);
  return distance(drw_point_screen_to_view(float3(uv, depth)),
                  drw_point_screen_to_view(float3(next_uv, depth)));
}

void output_outline(float4 line_color,
                    float line_width,
                    float depth_threshold,
                    float normal_threshold,
                    float outline_id,
                    bool id_edge,
                    bool freestyle_edge,
                    float width_variation)
{
#if defined(MAT_OUTLINE_SUPPORT) && defined(GPU_FRAGMENT_SHADER)
  if (flag_test(drw_object_infos().flag, OBJECT_HOLDOUT)) {
    return;
  }

  if (line_width <= 0.0f || line_color.a <= 0.0f) {
    return;
  }

  int2 texel = int2(gl_FragCoord.xy);
  uint custom_outline_id = uint(max(outline_id, 0.0f) + 0.5f);
  uint resolved_outline_id = (custom_outline_id > 0u) ? custom_outline_id :
                                                      min(drw_resource_id() + 1u,
                                                          OUTLINE_ID_VALUE_MASK);

  float4 stored_color = line_color;
  stored_color.a = saturate(stored_color.a);
  uint4 stored_info = uint4(outline_width_pack(line_width),
                            outline_depth_threshold_pack(depth_threshold, width_variation),
                            outline_normal_threshold_pack(normal_threshold, freestyle_edge),
                            outline_id_pack(resolved_outline_id, id_edge));
#  if defined(MAT_OUTLINE_CLEAR)
  g_outline_staged_color = stored_color;
  g_outline_staged_info = stored_info;
#  else
  imageStoreFast(outline_color_img, texel, stored_color);
  imageStoreFast(outline_info_img, texel, stored_info);
#  endif
#endif
}
