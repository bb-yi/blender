/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_shadow_filter_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_shadow_mask_filter)

#include "draw_view_lib.glsl"
#include "eevee_reverse_z_lib.glsl"

int2 shadow_filter_extent()
{
#ifdef SHADOW_FILTER_LAYERED_INPUT
  return textureSize(shadow_tx, 0).xy;
#else
  return textureSize(shadow_tx, 0);
#endif
}

float shadow_filter_fetch(int2 texel)
{
#ifdef SHADOW_FILTER_LAYERED_INPUT
  return texelFetch(shadow_tx, int3(texel, shadow_layer), 0).r;
#else
  return texelFetch(shadow_tx, texel, 0).r;
#endif
}

float shadow_filter_linear_depth(int2 texel)
{
  float depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
  return -drw_depth_screen_to_view(depth);
}

float shadow_filter_depth_weight(float center_depth, float sample_depth)
{
  float depth_delta = abs(sample_depth - center_depth);
  float depth_sigma = max(0.02f, center_depth * 0.05f);
  return exp2(-depth_delta / depth_sigma);
}

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = shadow_filter_extent();
  const int2 min_texel = int2(0);
  const int2 max_texel = max(extent - int2(1), int2(0));

  const float center_shadow = shadow_filter_fetch(texel);
  const float center_depth = shadow_filter_linear_depth(texel);

  if (center_depth <= 0.0f) {
    out_visibility = center_shadow;
    return;
  }

  const float weights[7] = {
      0.19648255f, 0.17603266f, 0.12098100f, 0.06475994f, 0.02699548f, 0.00876415f, 0.00221600f};

  float accum = center_shadow * weights[0];
  float weight_sum = weights[0];

  for (int offset = 1; offset <= 6; offset++) {
    int2 delta = filter_direction * offset;
    int2 texel_a = clamp(texel + delta, min_texel, max_texel);
    int2 texel_b = clamp(texel - delta, min_texel, max_texel);

    float depth_a = shadow_filter_linear_depth(texel_a);
    float depth_b = shadow_filter_linear_depth(texel_b);
    float shadow_a = shadow_filter_fetch(texel_a);
    float shadow_b = shadow_filter_fetch(texel_b);
    float weight_a = weights[offset] * shadow_filter_depth_weight(center_depth, depth_a);
    float weight_b = weights[offset] * shadow_filter_depth_weight(center_depth, depth_b);

    accum += shadow_a * weight_a;
    accum += shadow_b * weight_b;
    weight_sum += weight_a + weight_b;
  }

  float filtered_shadow = (weight_sum > 1e-6f) ? clamp(accum / weight_sum, 0.0f, 1.0f) :
                                                center_shadow;
  out_visibility = filtered_shadow;
}
