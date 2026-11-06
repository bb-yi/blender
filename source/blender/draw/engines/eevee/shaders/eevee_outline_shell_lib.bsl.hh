/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_model.bsl.hh"
#include "eevee_defines.hh"

namespace eevee {
struct OutlineShellOcclusionData {
  [[storage(OUTLINE_SHELL_LAYER_BUF_SLOT, read)]] const int (&outline_shell_layer_buf)[];
  [[sampler(OUTLINE_SHELL_DEPTH_TEX_SLOT)]] sampler2DArray outline_shell_depth_tx;
};
}  // namespace eevee

#ifdef MAT_OUTLINE_SHELL_SELF_OCCLUSION
bool outline_shell_self_occluded(uint resource_id_raw, float4 frag_co)
{
  [[resource_table]] const eevee::OutlineShellOcclusionData &srt =
      resource_table_get(eevee::OutlineShellOcclusionData);
  draw::ID id{resource_id_raw};
  const uint resource_id = id.resource_id<1>();
  int layer = srt.outline_shell_layer_buf[resource_id] - 1;
  if (layer < 0) {
    return false;
  }
  float own_depth = texelFetch(srt.outline_shell_depth_tx, int3(int2(frag_co.xy), layer), 0).r;
  /* EEVEE stores reverse-Z. Keep coplanar shell surfaces and clear/background texels. */
  /* A few float ULPs avoid treating the identical rasterized surface as its own occluder. */
  float bias = max(1e-8f, own_depth * (4.0f * 1.1920929e-7f));
  return own_depth > frag_co.z + bias;
}
#endif
