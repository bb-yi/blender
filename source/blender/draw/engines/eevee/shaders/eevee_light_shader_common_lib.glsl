/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

void light_shader_globals_init()
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

float4 light_shader_result_clamp(float4 result)
{
  return float4(max(result.rgb, float3(0.0f)), max(result.a, 0.0f));
}
