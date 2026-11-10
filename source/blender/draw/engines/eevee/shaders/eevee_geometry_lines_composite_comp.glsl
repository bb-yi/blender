/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_geometry_lines_infos.hh"
COMPUTE_SHADER_CREATE_INFO(eevee_geometry_lines_composite)

void main()
{
  int2 pixel = int2(gl_GlobalInvocationID.xy);
  int2 extent = imageSize(output_img);
  if (any(greaterThanEqual(pixel, extent))) {
    return;
  }
  float2 uv = (float2(pixel) + 0.5f) / float2(extent);
  float4 line = textureLod(line_tx, uv, 0.0f);
  float4 scene = texelFetch(scene_tx, pixel, 0);
  imageStore(output_img, pixel, line + scene * (1.0f - line.a));
}
