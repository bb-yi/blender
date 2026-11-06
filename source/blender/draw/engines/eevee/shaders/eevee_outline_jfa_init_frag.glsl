/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

#ifdef OUTLINE_JFA_PACKED
FRAGMENT_SHADER_CREATE_INFO(eevee_outline_jfa_init_packed)
#else
FRAGMENT_SHADER_CREATE_INFO(eevee_outline_jfa_init)
#endif

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const float4 seed = texelFetch(outline_seed_tx, texel, 0);
#ifdef OUTLINE_JFA_PACKED
  const float width = seed.a * clamp(seed.r, 0.0f, 1.0f);
  out_jfa_coord = (seed.a > 0.0f && seed.r > 0.0f) ?
                      uint2(uint(texel.x) | (uint(texel.y) << 16u), floatBitsToUint(width)) :
                      uint2(0xffffffffu, 0u);
#else
  if (seed.a > 0.0f && seed.r > 0.0f) {
    out_jfa_coord = float2(texel) + 0.5f;
  }
  else {
    out_jfa_coord = float2(-1e10f);
  }
#endif
}
