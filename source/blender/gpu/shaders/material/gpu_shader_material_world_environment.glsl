/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_world_environment(float3 direction, out float4 color)
{
#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
#  ifdef SPHERE_PROBE
  float3 V = direction;
  if (dot(V, V) < 1e-8f) {
    V = -drw_world_incident_vector(g_data.P);
  }
  V = safe_normalize(V);

  LightProbeSample samp = lightprobe_load(g_data.P, g_data.Ng, V);
  float3 radiance = lightprobe_spherical_sample_normalized_with_parallax(samp, g_data.P, V, 0.0);
  color = float4(radiance, 1.0f);
#  else
  color = float4(0.0f);
#  endif
#else
  color = float4(0.0f);
#endif
}
