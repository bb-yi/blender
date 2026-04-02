/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float3 screenspace_info_sample_view_position(float3 view_position, float use_explicit_view_position)
{
  return (use_explicit_view_position > 0.5f) ? view_position : drw_point_world_to_view(g_data.P);
}

float2 screenspace_info_project_uv(float3 sample_view_position)
{
  return drw_point_view_to_screen(sample_view_position).xy;
}

float4 screenspace_info_environment_fallback(float3 sample_view_position)
{
  float3 sample_world_position = drw_point_view_to_world(sample_view_position);
  float3 view_direction = safe_normalize(-drw_world_incident_vector(sample_world_position));
  float3 radiance = lightprobe_world_sample(view_direction, 0.0f);
  return float4(radiance, 1.0f);
}

[[node]]
void node_screenspace_info(float3 view_position,
                           float use_explicit_view_position,
                           out float4 scene_color,
                           out float scene_depth)
{
#if defined(GPU_FRAGMENT_SHADER) && (defined(MAT_DEFERRED) || defined(MAT_FORWARD))
  float3 sample_view_position = screenspace_info_sample_view_position(view_position,
                                                                      use_explicit_view_position);
  float2 uv = screenspace_info_project_uv(sample_view_position);
  if (any(lessThan(uv, float2(0.0f))) || any(greaterThan(uv, float2(1.0f)))) {
    scene_color = float4(0.0f);
    scene_depth = 0.0f;
    return;
  }

  int2 extent = textureSize(previous_layer_radiance_tx, 0);
  int2 texel = clamp(int2(uv * float2(extent)), int2(0), extent - int2(1));
  float depth = texelFetch(hiz_prev_tx, texel, 0).r;
  scene_color = texelFetch(previous_layer_radiance_tx, texel, 0);
  scene_color.a = saturate(1.0f - scene_color.a);

  if (depth == 1.0f) {
    if (all(lessThanEqual(abs(scene_color.rgb), float3(1e-6f)))) {
      scene_color = screenspace_info_environment_fallback(sample_view_position);
    }
    scene_depth = 0.0f;
    return;
  }

  scene_depth = -drw_depth_screen_to_view(depth);
#else
  scene_color = float4(0.0f);
  scene_depth = 0.0f;
#endif
}
