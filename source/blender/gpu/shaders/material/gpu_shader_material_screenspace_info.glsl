/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float3 screenspace_info_sample_view_position(KernelGlobals &kg, ShadingData &sd,
                                             float3 view_position, float use_explicit_view_position)
{
  return (use_explicit_view_position > 0.5f) ? view_position :
      kg.view_matrices_get(sd).point_world_to_view(sd.P);
}

float2 screenspace_info_project_uv(KernelGlobals &kg, ShadingData &sd, float3 sample_view_position)
{
  return kg.view_matrices_get(sd).point_view_to_screen(sample_view_position).xy;
}

float4 screenspace_info_environment_fallback(KernelGlobals &kg, ShadingData &sd,
                                             float3 sample_view_position)
{
  const ViewMatrices view = kg.view_matrices_get(sd);
  float3 sample_world_position = view.point_view_to_world(sample_view_position);
  float3 view_direction = safe_normalize(-view.world_incident_vector(sample_world_position));
  [[resource_table]] const eevee::LightprobeRenderData &lightprobes = resource_table_get(
      eevee::LightprobeRenderData);
  [[resource_table]] const eevee::LightprobeSphereRenderData &spheres = lightprobes.spheres;
  eevee::LightProbeSample probe_sample = lightprobes.load(
      sd.frag_co.xy, sd.P, sd.Ng, view.world_incident_vector(sd.P));
  float3 radiance = spheres.spherical_sample_normalized_with_parallax(
      probe_sample, sd.P, view_direction, 0.0f);
  return float4(radiance, 1.0f);
}

[[node]]
void node_screenspace_info(float3 view_position,
                           float use_explicit_view_position,
                           [[resource_table]] KernelGlobals &kg,
                           ShadingData &sd,
                           out float4 scene_color,
                           out float scene_depth)
{
#if defined(GPU_FRAGMENT_SHADER) && (defined(MAT_DEFERRED) || defined(MAT_FORWARD))
  float3 sample_view_position = screenspace_info_sample_view_position(kg, sd, view_position,
                                                                      use_explicit_view_position);
  float2 uv = screenspace_info_project_uv(kg, sd, sample_view_position);
  if (any(lessThan(uv, float2(0.0f))) || any(greaterThan(uv, float2(1.0f)))) {
    scene_color = float4(0.0f);
    scene_depth = 0.0f;
    return;
  }

  int2 extent = textureSize(kg.previous_layer_radiance.previous_layer_radiance_tx, 0);
  int2 texel = clamp(int2(uv * float2(extent)), int2(0), extent - int2(1));
  float depth = texelFetch(kg.previous_layer_hiz.hiz_prev_tx, texel, 0).r;
  scene_color = texelFetch(kg.previous_layer_radiance.previous_layer_radiance_tx, texel, 0);
  scene_color.a = saturate(1.0f - scene_color.a);

  if (depth == 1.0f) {
    if (all(lessThanEqual(abs(scene_color.rgb), float3(1e-6f)))) {
      scene_color = screenspace_info_environment_fallback(kg, sd, sample_view_position);
    }
    scene_depth = 0.0f;
    return;
  }

  scene_depth = -kg.view_matrices_get(sd).depth_screen_to_view(depth);
#else
  scene_color = float4(0.0f);
  scene_depth = 0.0f;
#endif
}
