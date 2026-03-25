/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float2 curvature_rotate(float2 value, float angle)
{
  float s = sin(angle);
  float c = cos(angle);
  return float2(c * value.x - s * value.y, s * value.x + c * value.y);
}

float3 curvature_normalize_or(float3 value, float3 fallback)
{
  float value_len_sq = dot(value, value);
  if (value_len_sq > 1e-8f) {
    return value * inversesqrt(value_len_sq);
  }

  float fallback_len_sq = dot(fallback, fallback);
  if (fallback_len_sq > 1e-8f) {
    return fallback * inversesqrt(fallback_len_sq);
  }

  return float3(0.0f, 0.0f, 1.0f);
}

float curvature_screen_depth_sample(float2 uv)
{
  return textureLod(hiz_tx, uv, 0.0f).r;
}

float curvature_depth_sample(float2 uv)
{
  return drw_depth_screen_to_view(curvature_screen_depth_sample(uv));
}

float curvature_background_depth()
{
  return drw_depth_screen_to_view(1.0f);
}

#if defined(MAT_RAYCAST)
uint curvature_object_id_sample(float2 uv)
{
  int2 tex_size = textureSize(object_id_tx, 0);
  int2 texel = clamp(int2(uv * float2(tex_size)), int2(0), tex_size - int2(1));
  return texelFetch(object_id_tx, texel, 0).x;
}
#endif

float3 curvature_sample_world_position(float2 uv,
                                       float fallback_screen_depth,
                                       bool local_only,
                                       uint self_id)
{
  float2 sample_uv = clamp(uv, float2(0.0f), float2(1.0f));
  float sample_screen_depth = curvature_screen_depth_sample(sample_uv);

#if defined(MAT_RAYCAST)
  if (local_only && curvature_object_id_sample(sample_uv) != self_id) {
    sample_screen_depth = fallback_screen_depth;
  }
#endif

  if (sample_screen_depth >= 1.0f) {
    sample_screen_depth = fallback_screen_depth;
  }

  return drw_point_screen_to_world(float3(sample_uv, sample_screen_depth));
}

float3 curvature_bevel_normal_eval(float samples,
                                   float2 sample_radius_uv,
                                   float3 scale,
                                   float2 uv,
                                   float center_screen_depth,
                                   bool local_only,
                                   uint self_id)
{
  float3 base_normal = curvature_normalize_or(g_data.N, g_data.Ng);
  if (dot(sample_radius_uv, sample_radius_uv) <= 1e-16f) {
    return base_normal;
  }

  float3 center_position = drw_point_screen_to_world(float3(uv, center_screen_depth));
  const int direction_count = 8;
  const int max_ring_count = 8;
  const float angle_step = 0.78539816339f;
  int ring_count = clamp(int(max(samples, 1.0f)), 1, max_ring_count);

  float3 accum = base_normal;
  float accum_weight = 1.0f;

  for (int ring = 1; ring <= max_ring_count; ring++) {
    if (ring > ring_count) {
      break;
    }

    float t = float(ring) / float(ring_count);
    float radial_weight = 1.0f - 0.5f * t;
    float2 ring_offset_scale = sample_radius_uv * t * scale.xy;

    for (int direction_index = 0; direction_index < direction_count; direction_index++) {
      float angle_a = angle_step * float(direction_index);
      float angle_b = angle_step * float((direction_index + 1) % direction_count);

      float2 offset_a = curvature_rotate(float2(1.0f, 0.0f), angle_a) * ring_offset_scale;
      float2 offset_b = curvature_rotate(float2(1.0f, 0.0f), angle_b) * ring_offset_scale;

      float3 position_a = curvature_sample_world_position(
          uv + offset_a, center_screen_depth, local_only, self_id);
      float3 position_b = curvature_sample_world_position(
          uv + offset_b, center_screen_depth, local_only, self_id);

      float3 facet_normal = cross(position_a - center_position, position_b - center_position);
      facet_normal = curvature_normalize_or(facet_normal, base_normal);
      if (dot(facet_normal, base_normal) < 0.0f) {
        facet_normal = -facet_normal;
      }

      accum += facet_normal * radial_weight;
      accum_weight += radial_weight;
    }
  }

  return curvature_normalize_or(accum / accum_weight, base_normal);
}

[[node]]
void node_screenspace_curvature(float samples,
                                float sample_radius,
                                float thickness,
                                float3 scale,
                                out float scene_curvature,
                                out float scene_rim,
                                out float3 bevel_normal)
{
#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
  float2 texel_size = 1.0f / float2(textureSize(hiz_tx, 0));
  float2 sample_radius_uv = texel_size * sample_radius;
  float2 uvs = gl_FragCoord.xy * texel_size;
  float center_screen_depth = curvature_screen_depth_sample(uvs);
  float mid_depth = curvature_depth_sample(uvs);
  float clamp_range = 0.001f;
  int n_samples = int(max(samples, 1.0f));
  float i_samples = 64.0f / float(n_samples);

  float angle_offset = sampling_rng_1D_get(SAMPLING_TRANSPARENCY);
  float accum = 0.0f;
  float rim_accum = 0.0f;

  for (int r = 0; r < 8; r++) {
    float angle = (float(r) + angle_offset) * 3.1415f * 0.25f * 0.5f;
    float2 offset = curvature_rotate(float2(1.0f, 0.0f), angle) * sample_radius_uv * scale.xy;

    for (int i = 1; i <= n_samples; i++) {
      float sample_offset = float(i) * i_samples;
      float left_depth = curvature_depth_sample(uvs + offset * sample_offset);
      float right_depth = curvature_depth_sample(uvs - offset * sample_offset);

      float curve = clamp(left_depth - mid_depth, -clamp_range, clamp_range) +
                    clamp(right_depth - mid_depth, -clamp_range, clamp_range);
      float afac = 1.0f - float(i - 1) / float(n_samples);

      accum += curve * afac * 0.001f;
      rim_accum += min(mid_depth - min(left_depth, right_depth), thickness) * afac;
    }
  }

  scene_curvature = -accum / length(texel_size) * i_samples;
  scene_rim = rim_accum / max(sample_radius, 1e-8f) * clamp_range;
  bevel_normal = curvature_bevel_normal_eval(
      samples, sample_radius_uv, scale, uvs, center_screen_depth, false, 0u);
#else
  scene_curvature = 0.0f;
  scene_rim = 0.0f;
  bevel_normal = curvature_normalize_or(g_data.N, g_data.Ng);
#endif
}

[[node]]
void node_screenspace_curvature_local(float samples,
                                      float sample_radius,
                                      float thickness,
                                      float3 scale,
                                      out float scene_curvature,
                                      out float scene_rim,
                                      out float3 bevel_normal)
{
#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER)) && defined(MAT_RAYCAST)
  float2 screen_texel_size = 1.0f / float2(textureSize(object_id_tx, 0));
  float2 hiz_texel_size = 1.0f / float2(textureSize(hiz_tx, 0));
  float2 screen_sample_radius_uv = screen_texel_size * sample_radius;
  float2 hiz_sample_radius_uv = hiz_texel_size * sample_radius;
  float2 screen_uv = gl_FragCoord.xy * screen_texel_size;
  float2 hiz_uv = gl_FragCoord.xy * hiz_texel_size;
  float center_screen_depth = curvature_screen_depth_sample(hiz_uv);
  float mid_depth = curvature_depth_sample(hiz_uv);
  float background_depth = curvature_background_depth();
  float clamp_range = 0.001f;
  int n_samples = int(max(samples, 1.0f));
  float i_samples = 64.0f / float(n_samples);

  float angle_offset = sampling_rng_1D_get(SAMPLING_TRANSPARENCY);
  float accum = 0.0f;
  float rim_accum = 0.0f;
  float curve_weight = 0.0f;
  float curve_weight_full = 0.0f;
  float rim_weight = 0.0f;
  float rim_weight_full = 0.0f;
  uint center_id = curvature_object_id_sample(screen_uv);
  uint self_id = (center_id != 0u) ? center_id : (drw_resource_id() & 0xFFFFu);

  for (int r = 0; r < 8; r++) {
    float angle = (float(r) + angle_offset) * 3.1415f * 0.25f * 0.5f;
    float2 screen_offset = curvature_rotate(float2(1.0f, 0.0f), angle) * screen_sample_radius_uv *
                           scale.xy;
    float2 hiz_offset = curvature_rotate(float2(1.0f, 0.0f), angle) * hiz_sample_radius_uv *
                        scale.xy;

    for (int i = 1; i <= n_samples; i++) {
      float sample_offset = float(i) * i_samples;
      float2 left_screen_uv = screen_uv + screen_offset * sample_offset;
      float2 right_screen_uv = screen_uv - screen_offset * sample_offset;
      float2 left_hiz_uv = hiz_uv + hiz_offset * sample_offset;
      float2 right_hiz_uv = hiz_uv - hiz_offset * sample_offset;

      uint left_id = curvature_object_id_sample(left_screen_uv);
      uint right_id = curvature_object_id_sample(right_screen_uv);
      float left_depth = curvature_depth_sample(left_hiz_uv);
      float right_depth = curvature_depth_sample(right_hiz_uv);

      float afac = 1.0f - float(i - 1) / float(n_samples);
      curve_weight_full += afac * 2.0f;
      rim_weight_full += afac;

      if (left_id == self_id) {
        accum += clamp(left_depth - mid_depth, -clamp_range, clamp_range) * afac * 0.001f;
        curve_weight += afac;
      }
      if (right_id == self_id) {
        accum += clamp(right_depth - mid_depth, -clamp_range, clamp_range) * afac * 0.001f;
        curve_weight += afac;
      }

      float left_rim_depth = (left_id == self_id) ? left_depth : background_depth;
      float right_rim_depth = (right_id == self_id) ? right_depth : background_depth;
      float nearest_depth = min(left_rim_depth, right_rim_depth);
      rim_accum += min(mid_depth - nearest_depth, thickness) * afac;
      rim_weight += afac;
    }
  }

  float curve_scale = (curve_weight > 0.0f) ? (curve_weight_full / curve_weight) : 0.0f;
  float rim_scale = (rim_weight > 0.0f) ? (rim_weight_full / rim_weight) : 0.0f;

  scene_curvature = -accum * curve_scale / length(screen_texel_size) * i_samples;
  scene_rim = rim_accum * rim_scale / max(sample_radius, 1e-8f) * clamp_range;
  bevel_normal = curvature_bevel_normal_eval(
      samples, screen_sample_radius_uv, scale, screen_uv, center_screen_depth, true, self_id);
#else
  scene_curvature = 0.0f;
  scene_rim = 0.0f;
  bevel_normal = curvature_normalize_or(g_data.N, g_data.Ng);
#endif
}
