/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_defines.hh"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

float outline_width_pack(float width)
{
  return saturate(width / OUTLINE_MAX_WIDTH);
}

float outline_width_unpack(float width_packed)
{
  return width_packed * OUTLINE_MAX_WIDTH;
}

float outline_id_pack(uint outline_id)
{
  return float(min(outline_id, 65535u)) / 65535.0f;
}

uint outline_id_unpack(float outline_id_packed)
{
  return uint(clamp(outline_id_packed, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

float outline_pixel_world_size_at(float depth, int2 extent, int2 texel)
{
  float2 uv = (float2(texel) + 0.5f) / float2(extent);
  int2 next_texel = min(texel + int2(1, 0), extent - int2(1));
  float2 next_uv = (float2(next_texel) + 0.5f) / float2(extent);
  return distance(drw_point_screen_to_view(float3(uv, depth)),
                  drw_point_screen_to_view(float3(next_uv, depth)));
}

float3 outline_screen_to_view(int2 texel, int2 extent, float depth)
{
  const float2 uv = (float2(texel) + 0.5f) / float2(extent);
  return drw_point_screen_to_view(float3(uv, depth));
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
  imageStoreFast(outline_color_img, texel, stored_color);
  imageStoreFast(outline_info_img,
                 texel,
                 float4(outline_width_pack(line_width),
                        saturate(depth_threshold),
                        saturate(normal_threshold),
                        outline_id_pack(resolved_outline_id)));
#endif
}
