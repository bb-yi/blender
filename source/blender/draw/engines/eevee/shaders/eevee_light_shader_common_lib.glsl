/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_nodetree_lib.bsl.hh"

void light_shader_globals_init(out ShadingData sd)
{
  sd = ShadingData{};
  sd.is_strand = false;
  sd.hair_diameter = 0.0f;
  sd.hair_strand_id = 0;
  sd.ray_type = uchar(RAY_TYPE_CAMERA);
  sd.ray_depth = 0.0f;
  sd.barycentric_coords = float2(0.0f);
  sd.barycentric_dists = float3(0.0f);
  sd.curve_T = float3(0.0f);
  sd.curve_B = float3(0.0f);
  sd.curve_N = float3(0.0f);
}

float4 light_shader_result_clamp(float4 result)
{
  return float4(max(result.rgb, float3(0.0f)), max(result.a, 0.0f));
}
