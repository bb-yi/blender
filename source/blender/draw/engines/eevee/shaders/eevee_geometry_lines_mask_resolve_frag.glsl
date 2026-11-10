/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_geometry_lines_infos.hh"
FRAGMENT_SHADER_CREATE_INFO(eevee_geometry_lines_mask_resolve)

void main()
{
  float alpha = textureLod(mask_tx, screen_uv, 0.0f).r;
#ifdef GEOMETRY_LINES_PACKED_COVERAGE
  float4 sum = textureLod(mask_tx, screen_uv, 0.0f);
  if (sample_count > 4) { sum += textureLod(mask_1_tx, screen_uv, 0.0f); }
  if (sample_count > 8) { sum += textureLod(mask_2_tx, screen_uv, 0.0f); }
  if (sample_count > 12) { sum += textureLod(mask_3_tx, screen_uv, 0.0f); }
  alpha = dot(sum, float4(1.0f)) / float(sample_count);
#endif
  out_color = mapped_color ? textureLod(mapped_color_tx, screen_uv, 0.0f) : float4(brush_color.rgb * alpha, alpha);
}
