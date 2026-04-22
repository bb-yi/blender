/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_expand)

#include "draw_view_lib.glsl"
#include "eevee_outline_lib.glsl"

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = textureSize(outline_seed_tx, 0);
  const float center_depth = texelFetch(depth_tx, texel, 0).r;
  const uint center_outline_id = outline_id_unpack(texelFetch(outline_info_tx, texel, 0).a);

  float4 best_color = float4(0.0f);
  float best_linear_depth = 1e20f;
  const int max_half_width = int(ceil(OUTLINE_MAX_WIDTH * 0.5f));

  for (int x = -max_half_width; x <= max_half_width; x++) {
    for (int y = -max_half_width; y <= max_half_width; y++) {
      const int2 sample_texel = texel + int2(x, y);
      if (any(lessThan(sample_texel, int2(0))) || any(greaterThanEqual(sample_texel, extent))) {
        continue;
      }

      const float4 seed = texelFetch(outline_seed_tx, sample_texel, 0);
      const float seed_width_signed = seed.a;
      const float seed_width = abs(seed_width_signed);
      const bool seed_is_silhouette = seed_width_signed < 0.0f;
      if (seed_width <= 0.0f) {
        continue;
      }

      const float offset_length = length(float2(x, y)) - 0.5f;
      if (offset_length > seed_width * 0.5f) {
        continue;
      }

      const float4 outline_color = texelFetch(outline_color_tx, sample_texel, 0);
      float alpha = outline_color.a;
      if (offset_length <= 0.0f && seed_width <= 1.0f) {
        alpha *= seed_width;
      }
      else {
        alpha *= clamp(seed_width * 0.5f - offset_length, 0.0f, 1.0f);
      }
      if (alpha <= 0.0f) {
        continue;
      }

      const float sample_depth = texelFetch(depth_tx, sample_texel, 0).r;
      const uint sample_outline_id = outline_id_unpack(texelFetch(outline_info_tx, sample_texel, 0).a);
      const float2 sample_uv = (float2(sample_texel) + 0.5f) / float2(extent);
      const float linear_depth = -drw_point_screen_to_view(float3(sample_uv, sample_depth)).z;

      if (seed_is_silhouette && center_outline_id != 0u && center_outline_id == sample_outline_id) {
        continue;
      }

      bool use_sample = alpha > best_color.a;
      use_sample = use_sample || ((abs(alpha - best_color.a) < 1e-6f) &&
                                  (linear_depth < best_linear_depth));

      if (sample_outline_id != center_outline_id && center_depth < sample_depth) {
        use_sample = false;
      }

      if (use_sample) {
        best_color = float4(outline_color.rgb * alpha, alpha);
        best_linear_depth = linear_depth;
      }
    }
  }

  out_outline_color = best_color;
}
