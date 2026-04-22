/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_outline_jfa_step)

void main()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const int2 extent = imageSize(jfa_out_img);
  if (any(greaterThanEqual(texel, extent))) {
    return;
  }

  float2 best_coord = imageLoad(jfa_in_img, texel).rg;
  float best_dist_sq = 1e20f;
  if (best_coord.x > -1e9f) {
    float2 diff = float2(texel) + 0.5f - best_coord;
    best_dist_sq = dot(diff, diff);
  }

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
      const float2 diff = float2(texel) + 0.5f - sample_coord;
      const float dist_sq = dot(diff, diff);
      if (dist_sq < best_dist_sq) {
        best_dist_sq = dist_sq;
        best_coord = sample_coord;
      }
    }
  }

  imageStore(jfa_out_img, texel, float4(best_coord, 0.0f, 0.0f));
}
