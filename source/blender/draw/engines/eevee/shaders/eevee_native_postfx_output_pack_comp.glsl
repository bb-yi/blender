/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_native_postfx_output_infos.hh"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, imageSize(output_img).xy))) {
    return;
  }

  float4 color = texelFetch(input_tx, texel, 0);
#ifdef PACK_VALUE
  imageStore(output_img, int3(texel, output_layer), float4(color.x));
#else
  imageStore(output_img, int3(texel, output_layer), color);
#endif
}
