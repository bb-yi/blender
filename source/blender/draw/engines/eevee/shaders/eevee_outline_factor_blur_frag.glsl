/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Smooth the per-seed width-variation factor (seed.r) ALONG the contour, using 8-neighbour seed
 * connectivity. The seed pixels form the ~1px-wide outline contour, so averaging the factor among
 * connected same-id seeds is a geodesic blur along the stroke that is independent of screen-space
 * curvature. This removes the per-Voronoi-cell drawn-radius staircase (inner-boundary sawtooth)
 * that a nearest-seed-only resolve produces when adjacent seeds have different factors.
 *
 * Only the factor channel (.r) is blurred; the full line width (.a) is passed through untouched so
 * the downstream JFA flood + resolve coverage test still use a single uniform radius and stay
 * hole-free. Run as a ping-pong pass between the edge detect/freestyle passes and the JFA init. */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_factor_blur)

#include "eevee_outline_lib.glsl"

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const float4 center = texelFetch(outline_seed_tx, texel, 0);

  /* Non-seed pixels are passed through unchanged (preserves .a == 0 emptiness). */
  if (center.a <= 0.0f) {
    out_outline_seed = center;
    return;
  }

  const uint center_id = outline_id_unpack(texelFetch(outline_info_tx, texel, 0).a);
  float factor_sum = clamp(center.r, 0.0f, 1.0f);
  float weight_sum = 1.0f;

  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      const int2 tap = texel + int2(dx, dy);
      const float4 neighbor = texelFetch(outline_seed_tx, tap, 0);
      if (neighbor.a <= 0.0f) {
        continue; /* Not a seed pixel -> not part of the contour. */
      }
      /* Same outline id only: never bleed the factor across different objects' contours. */
      if (outline_id_unpack(texelFetch(outline_info_tx, tap, 0).a) != center_id) {
        continue;
      }
      factor_sum += clamp(neighbor.r, 0.0f, 1.0f);
      weight_sum += 1.0f;
    }
  }

  /* Blur only .r; pass .a (full line width) through unchanged for hole-free coverage. */
  out_outline_seed = float4(factor_sum / weight_sum, 0.0f, 0.0f, center.a);
}
