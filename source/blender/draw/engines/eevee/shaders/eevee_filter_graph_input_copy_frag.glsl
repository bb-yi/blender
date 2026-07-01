/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_filter_material_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_filter_graph_input_copy)

void main()
{
  int2 texel = int2(gl_FragCoord.xy);
  out_color = texelFetch(input_tx, int3(texel, input_layer), 0);
}
