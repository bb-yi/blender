/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Evaluate a point-independent Eevee light data-block node tree once per light.
 */

#include "infos/eevee_light_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_light_shader_uniform)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.glsl"
#include "eevee_light_lib.glsl"
#include "eevee_light_shader_common_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

float4 nodetree_light_shader();

void main()
{
  LightData light = light_buf[light_index];

  light_shader_globals_init();

  g_data.P = light_position_get(light);
  g_data.N = g_data.Ni = float3(0.0f, 0.0f, 1.0f);
  g_data.Ng = g_data.N;
  g_data.ray_length = distance(g_data.P, drw_view_position());

  attrib_load(WorldPoint{0});

  out_light_shader_buf[light_index] = light_shader_result_clamp(nodetree_light_shader());
}
