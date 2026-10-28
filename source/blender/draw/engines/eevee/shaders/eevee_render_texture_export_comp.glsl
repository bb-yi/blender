/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_render_texture_infos.hh"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, output_extent))) {
    return;
  }
  int2 pixel = texel + sample_offset;
  if (any(lessThan(pixel, film_offset)) ||
      any(greaterThanEqual(pixel, film_offset + film_extent)))
  {
    imageStore(output_img, texel, float4(0.0f));
    return;
  }
  float2 uv = (float2(pixel) + 0.5f) / float2(display_extent);
  float4 color = textureLod(capture_tx, uv, 0.0f);
  if (single_channel) {
    color = float4(float3(color.r), 1.0f);
  }
  else if (invert_alpha) {
    color.a = clamp(1.0f - color.a, 0.0f, 1.0f);
  }
  imageStore(output_img, texel, color);
}
