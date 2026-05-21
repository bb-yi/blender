/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Evaluate an Eevee light data-block node tree for a UV-space color bake surface.
 */

#include "infos/eevee_light_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_light_shader_bake)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.glsl"
#include "eevee_light_lib.glsl"
#include "eevee_light_shader_common_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

float4 nodetree_light_shader();

void main()
{
  light_shader_globals_init();

  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = textureSize(bake_light_shader_position_tx, 0);
  if (any(greaterThanEqual(texel, extent))) {
    out_light_shader = float4(1.0f);
    return;
  }

  const float4 position = texelFetch(bake_light_shader_position_tx, texel, 0);
  const float4 normal = texelFetch(bake_light_shader_normal_tx, texel, 0);
  if (position.a <= 0.0f || normal.a <= 0.0f || !any(greaterThan(abs(normal.xyz), float3(0.0f))))
  {
    out_light_shader = float4(1.0f);
    return;
  }

  g_data.P = position.xyz;
  g_data.N = g_data.Ni = normalize(normal.xyz);
  g_data.Ng = g_data.N;
  g_data.ray_length = distance(g_data.P, drw_view_position());

  attrib_load(WorldPoint{0});

  out_light_shader = light_shader_result_clamp(nodetree_light_shader());
}
