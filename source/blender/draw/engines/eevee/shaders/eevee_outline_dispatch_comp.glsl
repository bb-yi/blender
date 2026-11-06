/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_outline_dispatch)

void main()
{
  if (classify_source != 0) {
    const bool original = (outline_flags.x & OUTLINE_NEEDS_ORIGINAL_DETECT) != 0u;
    original_detect_draw_buf = uint4(3u, original ? 1u : 0u, 0u, 0u);
    cached_detect_draw_buf = uint4(3u, original ? 0u : 1u, 0u, 0u);
    return;
  }
  const bool blur = (outline_flags.x & OUTLINE_NEEDS_FACTOR_BLUR) != 0u;
  const bool flood = (outline_flags.x & OUTLINE_NEEDS_JFA) != 0u;
  factor_blur_draw_buf = uint4(3u, blur ? 1u : 0u, 0u, 0u);
  jfa_init_draw_buf = uint4(3u, flood ? 1u : 0u, 0u, 0u);
  jfa_dispatch_buf = flood ? uint4(uint3(jfa_dispatch_size), 0u) : uint4(0u);
}
