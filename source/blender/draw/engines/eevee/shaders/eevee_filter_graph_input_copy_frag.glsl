/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_filter_material_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_filter_graph_input_copy)

void main()
{
  int2 source_extent = int2(textureSize(input_tx, 0).xy);
  float2 uv = gl_FragCoord.xy / float2(target_extent);
  uv = clamp(uv, float2(0.0f), float2(1.0f));

  if (resample_mode == FILTER_GRAPH_RESAMPLE_LINEAR) {
    out_color = texture(input_tx, float3(uv, float(input_layer)));
    return;
  }

  int2 texel = clamp(int2(uv * float2(source_extent)), int2(0), source_extent - int2(1));
  out_color = texelFetch(input_tx, int3(texel, input_layer), 0);
}
