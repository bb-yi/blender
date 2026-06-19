/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_shell_boundary)

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const float4 center = texelFetch(shell_color_tx, texel, 0);
  if (center.a <= 1.0e-5f) {
    gpu_discard_fragment();
    return;
  }

  /* Composite the visible shell band, not just its projected outer boundary.
   * Boundary-only extraction misses self-occluding shell contours such as the UMA nose bridge and
   * leaves a transparent gap when Shell Width is intentionally exaggerated for testing. */
  out_outline_shell_boundary = center;
}
