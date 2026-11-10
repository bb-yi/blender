/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_geometry_lines_infos.hh"
FRAGMENT_SHADER_CREATE_INFO(eevee_geometry_lines_present)

void main()
{
  out_color = textureLod(line_tx, screen_uv, 0.0f);
}
