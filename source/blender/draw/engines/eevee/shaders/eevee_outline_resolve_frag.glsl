/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_resolve)

#include "draw_view_lib.glsl"
#include "eevee_outline_lib.glsl"

float outline_screen_depth_fetch(int2 texel)
{
  return 1.0f - texelFetch(depth_tx, texel, 0).r;
}

float outline_linear_depth(float screen_depth)
{
  return -drw_depth_screen_to_view(screen_depth);
}

float outline_hash1(float4 v)
{
  uint4 u = floatBitsToUint(v);
  u = u * 1664525u + 1013904223u;
  u.x += u.y * u.w;
  u.y += u.z * u.x;
  u.z += u.x * u.y;
  u.w += u.y * u.z;
  u ^= u >> 16u;
  u.x += u.y * u.w;
  u.y += u.z * u.x;
  u.z += u.x * u.y;
  u.w += u.y * u.z;
  return float(u.x) / float(0xffffffffU);
}

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = textureSize(outline_color_tx, 0);
  const float center_depth = outline_screen_depth_fetch(texel);
  const uint center_outline_id = outline_id_unpack(texelFetch(outline_info_tx, texel, 0).a);
  const float2 uv = (float2(texel) + 0.5f) / float2(extent);

  const int max_width = int(ceil(OUTLINE_MAX_WIDTH));
  const int max_half_width = int(ceil(OUTLINE_MAX_WIDTH * 0.5f));

  float4 line_color = float4(0.0f);
  float line_depth = 1.0f;
  float line_linear_depth = outline_linear_depth(line_depth);

  for (int x = -max_half_width; x <= max_half_width; x++) {
    for (int y = -max_half_width; y <= max_half_width; y++) {
      const int2 offset = int2(x, y);
      const int2 sample_texel = texel + offset;
      if (any(lessThan(sample_texel, int2(0))) || any(greaterThanEqual(sample_texel, extent))) {
        continue;
      }

      const float offset_length = length(float2(offset)) - 0.5f;
      float offset_width = texelFetch(outline_seed_tx, sample_texel, 0).a;
      offset_width = min(offset_width, float(max_width));

      if (offset_width > 0.0f && offset_length <= offset_width * 0.5f) {
        float4 offset_line_color = texelFetch(outline_color_tx, sample_texel, 0);
        const float offset_line_depth = outline_screen_depth_fetch(sample_texel);
        float offset_line_linear_depth = outline_linear_depth(offset_line_depth);
        const uint offset_line_id = outline_id_unpack(texelFetch(outline_info_tx, sample_texel, 0).a);

        if (offset_length <= 0.0f && offset_width <= 1.0f) {
          offset_line_color.a *= offset_width;
        }
        else {
          offset_line_color.a *= clamp(offset_width * 0.5f - offset_length, 0.0f, 1.0f);
        }

        const float alpha = offset_line_color.a;
        const float random_linear_depth_offset = outline_hash1(
            float4(float2(offset), 1.0f, outline_hash1(float4(uv, 0.0f, 0.0f))));
        offset_line_linear_depth +=
            random_linear_depth_offset * offset_width * 0.5f *
            outline_pixel_world_size_at(offset_line_depth, extent, sample_texel);

        bool override = false;
        if (alpha == line_color.a && offset_line_linear_depth < line_linear_depth) {
          override = true;
        }
        if (alpha > line_color.a) {
          override = true;
        }
        if (offset_line_id != center_outline_id && center_depth < offset_line_depth) {
          override = false;
        }

        if (override) {
          line_color = offset_line_color;
          line_linear_depth = offset_line_linear_depth;
          line_depth = offset_line_depth;
        }
      }
    }
  }

  out_outline_color = float4(line_color.rgb * line_color.a, 1.0f - line_color.a);
}
