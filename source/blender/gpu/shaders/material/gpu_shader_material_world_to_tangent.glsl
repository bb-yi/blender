/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_safe_lib.glsl"

#ifdef OBINFO_LIB
[[node]]
void node_world_to_tangent(float3 world_vector, float4 tangent, float3 &tangent_vector)
{
  float3 T;
  float3 B;
  float3 N;

  if (all(equal(tangent.xyz, float3(0.0f)))) {
    if (g_data.is_strand) {
      T = safe_normalize(g_data.curve_T);
      B = safe_normalize(g_data.curve_B);
      N = safe_normalize(g_data.curve_N);
    }
    else {
      tangent_vector = float3(0.0f);
      return;
    }
  }
  else {
#if defined(GPU_FRAGMENT_SHADER)
    tangent *= (FrontFacing ? 1.0f : -1.0f);
#endif
    N = safe_normalize(g_data.N);
    T = safe_normalize(tangent.xyz);
    /* Re-orthogonalize the tangent so the tangent-space basis matches the shading normal. */
    T = cross(N, safe_normalize(cross(T, N)));
    B = tangent.w * safe_normalize(cross(N, T));
    B *= (drw_object_infos().flag & OBJECT_NEGATIVE_SCALE) != 0 ? -1.0f : 1.0f;
  }

  tangent_vector = float3(dot(world_vector, T), dot(world_vector, B), dot(world_vector, N));
}
#endif
