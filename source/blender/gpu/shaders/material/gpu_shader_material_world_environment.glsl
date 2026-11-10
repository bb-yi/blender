/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_world_environment(float3 direction, [[resource_table]] KernelGlobals &kg,
                            ShadingData &sd, out float4 color)
{
#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
  [[resource_table]] const eevee::PipelineConstants &pipe = resource_table_get(
      eevee::PipelineConstants);
  if (pipe.use_lightprobe_data) [[static_branch]] {
  float3 V = direction;
  if (dot(V, V) < 1e-8f) {
    V = -kg.view_matrices_get(sd).world_incident_vector(sd.P);
  }
  V = safe_normalize(V);

  float3 incoming = kg.view_matrices_get(sd).world_incident_vector(sd.P);
  [[resource_table]] const eevee::LightprobeRenderData &lightprobes = resource_table_get(
      eevee::LightprobeRenderData);
  [[resource_table]] const eevee::LightprobeSphereRenderData &spheres = lightprobes.spheres;
  eevee::LightProbeSample probe_sample = lightprobes.load(sd.frag_co.xy, sd.P, sd.Ng, incoming);
  float3 radiance = spheres.spherical_sample_normalized_with_parallax(
      probe_sample, sd.P, V, 0.0f);
  color = float4(radiance, 1.0f);
  }
  else {
  color = float4(0.0f);
  }
#else
  color = float4(0.0f);
#endif
}
