/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_freestyle)

#include "draw_view_lib.glsl"
#include "eevee_outline_lib.glsl"
#include "eevee_reverse_z_lib.glsl"

float4 outline_source_color_fetch(int2 texel)
{
  return texelFetch(outline_color_tx, texel, 0);
}

uint4 outline_source_info_fetch(int2 texel)
{
  return texelFetch(outline_info_tx, texel, 0);
}

float outline_depth_fetch(int2 texel)
{
  return reverse_z::read(texelFetch(depth_tx, texel, 0).r);
}

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = textureSize(depth_tx, 0);
  if (any(lessThan(texel, int2(0))) || any(greaterThanEqual(texel, extent))) {
    discard;
  }

  const float line_depth = reverse_z::read(gl_FragCoord.z);
  const float scene_depth = outline_depth_fetch(texel);
  if (line_depth > scene_depth + 1.0e-5f) {
    discard;
  }

  const float4 outline_color = outline_source_color_fetch(texel);
  const uint4 outline_info = outline_source_info_fetch(texel);
  const float line_width = outline_width_unpack(outline_info.r);
  if (line_width <= 0.0f || outline_color.a <= 0.0f ||
      !outline_freestyle_edge_unpack(outline_info.b))
  {
    discard;
  }

  out_outline_seed = float4(outline_color.rgb, line_width);
}
