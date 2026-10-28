/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_outline_jfa_step)

float outline_seed_distance_squared(int2 texel, float2 seed_coord)
{
  if (seed_coord.x < -1e9f) {
    return 1e20f;
  }
  const float4 seed = texelFetch(outline_seed_tx, int2(seed_coord), 0);
  const float width = seed.a * clamp(seed.r, 0.0f, 1.0f);
  if (width <= 0.0f) {
    return 1e20f;
  }
  const float2 diff = float2(texel) + 0.5f - seed_coord;
  /* Radius-weighted distance preserves coverage from a wide seed even when a narrower
   * seed is closer in pixels. A value <= 1/4 is inside the seed's coverage disk. */
  return dot(diff, diff) / max(width * width, 1e-10f);
}

void main()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const int2 extent = imageSize(jfa_out_img);
  if (any(greaterThanEqual(texel, extent))) {
    return;
  }

  float2 best_coord = imageLoad(jfa_in_img, texel).rg;
  float best_dist_sq = outline_seed_distance_squared(texel, best_coord);

  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      const int2 sample_texel = texel + int2(dx, dy) * jfa_step_size;
      if (any(lessThan(sample_texel, int2(0))) || any(greaterThanEqual(sample_texel, extent))) {
        continue;
      }
      const float2 sample_coord = imageLoad(jfa_in_img, sample_texel).rg;
      if (sample_coord.x < -1e9f) {
        continue;
      }
      const float dist_sq = outline_seed_distance_squared(texel, sample_coord);
      if (dist_sq < best_dist_sq) {
        best_dist_sq = dist_sq;
        best_coord = sample_coord;
      }
    }
  }

  imageStore(jfa_out_img, texel, float4(best_coord, 0.0f, 0.0f));
}
