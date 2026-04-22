/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_jfa_init)

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const float4 seed = texelFetch(outline_seed_tx, texel, 0);
  if (seed.a > 0.0f) {
    out_jfa_coord = float2(texel) + 0.5f;
  }
  else {
    out_jfa_coord = float2(-1e10f);
  }
}
