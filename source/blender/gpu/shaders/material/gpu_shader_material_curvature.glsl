/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float2 curvature_rotate(float2 value, float angle)
{
  float s = sin(angle);
  float c = cos(angle);
  return float2(c * value.x - s * value.y, s * value.x + c * value.y);
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

[[node]]
void node_screenspace_curvature(float samples,
                                float sample_radius,
                                float thickness,
                                float3 scale,
                                out float scene_curvature,
                                out float scene_rim)
{
#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
  float2 texel_size = 1.0f / float2(textureSize(hiz_tx, 0));
  float2 sample_radius_uv = texel_size * sample_radius;
  float2 uvs = gl_FragCoord.xy * texel_size;
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
#else
  scene_curvature = 0.0f;
  scene_rim = 0.0f;
#endif
}

[[node]]
void node_screenspace_curvature_local(float samples,
                                      float sample_radius,
                                      float thickness,
                                      float3 scale,
                                      out float scene_curvature,
                                      out float scene_rim)
{
#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER)) && defined(MAT_RAYCAST)
  float2 screen_texel_size = 1.0f / float2(textureSize(object_id_tx, 0));
  float2 hiz_texel_size = 1.0f / float2(textureSize(hiz_tx, 0));
  float2 screen_sample_radius_uv = screen_texel_size * sample_radius;
  float2 hiz_sample_radius_uv = hiz_texel_size * sample_radius;
  float2 screen_uv = gl_FragCoord.xy * screen_texel_size;
  float2 hiz_uv = gl_FragCoord.xy * hiz_texel_size;
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
#else
  scene_curvature = 0.0f;
  scene_rim = 0.0f;
#endif
}
