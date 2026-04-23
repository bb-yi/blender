/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_resolve)

#include "draw_view_lib.glsl"
#include "eevee_outline_lib.glsl"

float outline_depth_fetch(int2 texel)
{
  return 1.0f - texelFetch(depth_tx, texel, 0).r;
}

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = textureSize(outline_seed_tx, 0);
  const float center_depth = outline_depth_fetch(texel);
  const uint center_outline_id = outline_id_unpack(texelFetch(outline_info_tx, texel, 0).a);

  const float2 seed_coord = texelFetch(jfa_tx, texel, 0).rg;
  if (seed_coord.x < -1e9f) {
    out_outline_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  const int2 seed_texel = int2(seed_coord);
  const float2 offset = float2(texel) + 0.5f - seed_coord;
  const float offset_length = length(offset) - 0.5f;

  const float4 seed = texelFetch(outline_seed_tx, seed_texel, 0);
  const float seed_width = seed.a;
  if (seed_width <= 0.0f || offset_length > seed_width * 0.5f) {
    out_outline_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  const float4 outline_color = texelFetch(outline_color_tx, seed_texel, 0);
  float alpha = outline_color.a;
  if (offset_length <= 0.0f && seed_width <= 1.0f) {
    alpha *= seed_width;
  }
  else {
    alpha *= clamp(seed_width * 0.5f - offset_length, 0.0f, 1.0f);
  }
  if (alpha <= 0.0f) {
    out_outline_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  const float sample_depth = outline_depth_fetch(seed_texel);
  const uint sample_outline_id = outline_id_unpack(
      texelFetch(outline_info_tx, seed_texel, 0).a);

  if (sample_outline_id != center_outline_id && center_depth < sample_depth) {
    out_outline_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  out_outline_color = float4(outline_color.rgb * alpha, 1.0f - alpha);
}
