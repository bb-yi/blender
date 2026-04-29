/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_defines.hh"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

/* Width is stored in a UNORM16 channel. Reserve the top code-point so unpacking never hits 1.0
 * exactly and can represent very large widths without a fixed shader-side cap. */
#define OUTLINE_WIDTH_PACK_MAX (65534.0f / 65535.0f)

float outline_width_pack(float width)
{
  width = max(width, 0.0f);
  if (width <= 0.0f) {
    return 0.0f;
  }
  return min(width / (width + 1.0f), OUTLINE_WIDTH_PACK_MAX);
}

float outline_width_unpack(float width_packed)
{
  width_packed = clamp(width_packed, 0.0f, OUTLINE_WIDTH_PACK_MAX);
  if (width_packed <= 0.0f) {
    return 0.0f;
  }
  return width_packed / (1.0f - width_packed);
}

float outline_id_pack(uint outline_id)
{
  return float(min(outline_id, 65535u)) / 65535.0f;
}

uint outline_id_unpack(float outline_id_packed)
{
  return uint(clamp(outline_id_packed, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_CLEAR) && defined(GPU_FRAGMENT_SHADER)
float4 g_outline_staged_color;
float4 g_outline_staged_info;
#endif

void outline_output_reset()
{
#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_CLEAR) && defined(GPU_FRAGMENT_SHADER)
  g_outline_staged_color = float4(0.0f);
  g_outline_staged_info = float4(0.0f);
#endif
}

void outline_output_flush()
{
#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_CLEAR) && defined(GPU_FRAGMENT_SHADER)
  if (g_outline_staged_color.a <= 0.0f || g_outline_staged_info.r <= 0.0f) {
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

void output_outline(
    float4 line_color, float line_width, float depth_threshold, float normal_threshold, float outline_id)
{
#if defined(MAT_OUTLINE_SUPPORT) && defined(GPU_FRAGMENT_SHADER)
  if (line_width <= 0.0f || line_color.a <= 0.0f) {
    return;
  }

  int2 texel = int2(gl_FragCoord.xy);
  uint custom_outline_id = uint(max(outline_id, 0.0f) + 0.5f);
  uint resolved_outline_id = (custom_outline_id > 0u) ? custom_outline_id :
                                                      min(drw_resource_id() + 1u, 65535u);

  float4 stored_color = line_color;
  stored_color.a = saturate(stored_color.a);
  float4 stored_info = float4(outline_width_pack(line_width),
                              saturate(depth_threshold),
                              saturate(normal_threshold),
                              outline_id_pack(resolved_outline_id));
#  if defined(MAT_OUTLINE_CLEAR)
  g_outline_staged_color = stored_color;
  g_outline_staged_info = stored_info;
#  else
  imageStoreFast(outline_color_img, texel, stored_color);
  imageStoreFast(outline_info_img, texel, stored_info);
#  endif
#endif
}
