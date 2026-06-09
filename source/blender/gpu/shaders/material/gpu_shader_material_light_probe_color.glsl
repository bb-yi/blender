/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float3 light_probe_color_safe_direction(float3 value, float3 fallback)
{
  float value_len_squared = dot(value, value);
  if (value_len_squared > 1e-16f) {
    return value * inversesqrt(value_len_squared);
  }

  float fallback_len_squared = dot(fallback, fallback);
  if (fallback_len_squared > 1e-16f) {
    return fallback * inversesqrt(fallback_len_squared);
  }

  return float3(0.0f, 0.0f, 1.0f);
}

float3 light_probe_color_resolve_shading_normal()
{
  return light_probe_color_safe_direction(g_data.N, g_data.Ng);
}

[[node]]
void node_light_probe_color(float3 direction,
                            float roughness,
                            out float4 reflection,
                            out float4 irradiance,
                            out float4 combined)
{
#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
#  ifdef SPHERE_PROBE
  float3 geometry_normal = light_probe_color_safe_direction(g_data.Ng, float3(0.0f, 0.0f, 1.0f));
  float3 shading_normal = light_probe_color_resolve_shading_normal();
  const ViewMatrices view = view_matrices_get();
  float3 view_vector = view.world_incident_vector(g_data.P);
  float3 reflected_view_direction = light_probe_color_safe_direction(
      -reflect(view_vector, shading_normal), -view_vector);
  float3 reflection_direction = light_probe_color_safe_direction(direction,
                                                                reflected_view_direction);
  float3 irradiance_direction = light_probe_color_safe_direction(direction, shading_normal);
  float reflection_lod = lightprobe::sphere::roughness_to_lod(saturate(roughness));

  [[resource_table]] const eevee::LightprobeRenderData &lightprobes = resource_table_get(
      eevee::LightprobeRenderData);
  [[resource_table]] const eevee::LightprobeSphereRenderData &lp_spheres = lightprobes.spheres;
  eevee::LightProbeSample probe_sample = lightprobes.load(
      gl_FragCoord.xy, g_data.P, geometry_normal, view_vector);
  probe_sample.volume_irradiance = spherical_harmonics::clamp_energy(
      probe_sample.volume_irradiance, uniform_buf.clamp.surface_indirect);

  float3 reflection_rgb = lp_spheres.spherical_sample_normalized_with_parallax(
      probe_sample, g_data.P, reflection_direction, reflection_lod);
  float3 irradiance_rgb = max(
      probe_sample.volume_irradiance.evaluate_lambert(irradiance_direction).rgb, float3(0.0f));
  float3 combined_rgb = reflection_rgb + irradiance_rgb;

  reflection = float4(reflection_rgb, 1.0f);
  irradiance = float4(irradiance_rgb, 1.0f);
  combined = float4(combined_rgb, 1.0f);
#  else
  reflection = float4(0.0f);
  irradiance = float4(0.0f);
  combined = float4(0.0f);
#  endif
#else
  reflection = float4(0.0f);
  irradiance = float4(0.0f);
  combined = float4(0.0f);
#endif
}
