/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_stencil_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_stencil_snapshot)

void main()
{
  out_value = texelFetch(stencil_source_tx, int2(gl_FragCoord.xy), 0).r & 15u;
}
