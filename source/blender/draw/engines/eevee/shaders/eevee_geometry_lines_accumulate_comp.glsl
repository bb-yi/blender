/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_geometry_lines_infos.hh"
COMPUTE_SHADER_CREATE_INFO(eevee_geometry_lines_accumulate)

void main()
{
  int2 pixel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(pixel, imageSize(accumulation_img)))) {
    return;
  }
  float4 value = texelFetch(input_tx, pixel, 0);
  /* The first sample never reads uninitialized accumulation storage. Keep the independent
   * line history outside Film/DLSS and use float32 to preserve narrow coverage over many samples. */
  if (sample_weight < 1.0f) {
    value = mix(imageLoad(accumulation_img, pixel), value, sample_weight);
  }
  imageStore(accumulation_img, pixel, value);
}
