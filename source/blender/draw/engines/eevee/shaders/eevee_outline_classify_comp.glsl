/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_outline_classify)

shared uint group_flags;

void main()
{
  if (gl_LocalInvocationIndex == 0u) {
    group_flags = 0u;
  }
  barrier();

  const int2 texel = int2(gl_GlobalInvocationID.xy);
  uint flags = 0u;
  if (classify_source != 0) {
    if (all(lessThan(texel, textureSize(outline_info_tx, 0).xy))) {
      const uint4 info = texelFetch(outline_info_tx, int3(texel, 0), 0);
      /* R/G bits 16..23 store the depth/normal threshold ranges. Use the exact original
       * detector whenever an outline source can taper, independent of node input wiring. */
      if ((info.r & 0xffffu) != 0u && ((info.r | info.g) & 0x00ff0000u) != 0u) {
        flags = OUTLINE_NEEDS_ORIGINAL_DETECT;
      }
    }
  }
  else if (all(lessThan(texel, textureSize(outline_seed_tx, 0)))) {
    const float4 seed = texelFetch(outline_seed_tx, texel, 0);
    if (any(isnan(seed.ra)) || any(isinf(seed.ra))) {
      flags = OUTLINE_NEEDS_FACTOR_BLUR | OUTLINE_NEEDS_JFA;
    }
    else if (seed.a > 0.0f) {
      /* Even a factor rounded to zero may receive a nonzero value from its neighbors. */
      if (seed.r != 1.0f) {
        flags |= OUTLINE_NEEDS_FACTOR_BLUR;
      }
      /* Blur only averages clamped factors, so full width is a conservative radius bound. */
      if (seed.a > 1.0f) {
        flags |= OUTLINE_NEEDS_JFA;
      }
    }
  }
  if (flags != 0u) {
    atomicOr(group_flags, flags);
  }
  barrier();
  if (gl_LocalInvocationIndex == 0u && group_flags != 0u) {
    atomicOr(outline_flags.x, group_flags);
  }
}
