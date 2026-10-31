/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#ifdef MAT_OUTLINE_SHELL_SELF_OCCLUSION
bool outline_shell_self_occluded()
{
  auto &interp_flat = interface_get(eevee_geom_iface_info, interp_flat);
  draw::ID id{interp_flat.resource_id_raw};
  const uint resource_id = id.resource_id<1>();
  int layer = outline_shell_layer_buf[resource_id] - 1;
  if (layer < 0) {
    return false;
  }
  float own_depth = texelFetch(outline_shell_depth_tx, int3(int2(gl_FragCoord.xy), layer), 0).r;
  /* EEVEE stores reverse-Z. Keep coplanar shell surfaces and clear/background texels. */
  /* A few float ULPs avoid treating the identical rasterized surface as its own occluder. */
  float bias = max(1e-8f, own_depth * (4.0f * 1.1920929e-7f));
  return own_depth > gl_FragCoord.z + bias;
}
#endif
