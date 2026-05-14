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
#include "eevee_nodetree_frag_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

float4 nodetree_light_shader();

void init_light_shader_globals()
{
  g_data.is_strand = false;
  g_data.hair_diameter = 0.0f;
  g_data.hair_strand_id = 0;
  g_data.ray_type = uniform_buf.pipeline.ray_type;
  g_data.ray_depth = 0.0f;
  g_data.barycentric_coords = float2(0.0f);
  g_data.barycentric_dists = float3(0.0f);
  g_data.curve_T = float3(0.0f);
  g_data.curve_B = float3(0.0f);
  g_data.curve_N = float3(0.0f);
}

void main()
{
  LightData light = light_buf[light_index];

  init_light_shader_globals();

  g_data.P = light_position_get(light);
  g_data.N = g_data.Ni = float3(0.0f, 0.0f, 1.0f);
  g_data.Ng = g_data.N;
  g_data.ray_length = distance(g_data.P, drw_view_position());

  attrib_load(WorldPoint{0});

  float4 result = nodetree_light_shader();
  out_light_shader_buf[light_index] = float4(max(result.rgb, float3(0.0f)), max(result.a, 0.0f));
}
