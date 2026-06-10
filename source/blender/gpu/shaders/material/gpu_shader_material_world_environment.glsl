/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_world_environment(float3 direction, out float4 color)
{
#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
#  if defined(CREATE_INFO_eevee_LightprobeRenderData)
  float3 V = direction;
  if (dot(V, V) < 1e-8f) {
    V = -drw_world_incident_vector(g_data.P);
  }
  V = safe_normalize(V);

  float3 radiance = lightprobe_world_sample(V, 0.0f);
  color = float4(radiance, 1.0f);
#  else
  color = float4(0.0f);
#  endif
#else
  color = float4(0.0f);
#endif
}
