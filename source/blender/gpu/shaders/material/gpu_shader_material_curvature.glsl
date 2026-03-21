/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float2 curvature_rotate(float2 value, float angle)
{
  float s = sin(angle);
  float c = cos(angle);
  return float2(c * value.x - s * value.y, s * value.x + c * value.y);
}

[[node]]
void node_screenspace_curvature(float samples,
                                float sample_radius,
                                float thickness,
                                float3 scale,
                                out float scene_curvature,
                                out float scene_rim)
{
#if defined(GPU_FRAGMENT_SHADER) && (defined(MAT_DEFERRED) || defined(MAT_FORWARD))
  float3 view_position = drw_point_world_to_view(g_data.P);
  float2 uvs = drw_point_view_to_screen(view_position).xy * uniform_buf.hiz.uv_scale;

  /* Goo uses a fixed texel size on purpose so the perceived radius stays intuitive. */
  float2 texel_size = float2(1.0f / 1920.0f, 1.0f / 1080.0f);
  float mid_depth = drw_depth_screen_to_view(textureLod(hiz_tx, uvs, 0.0f).r);
  float clamp_range = 0.001f;
  int n_samples = int(max(samples, 1.0f));
  float i_samples = 64.0f / float(n_samples);

  float angle_offset = sampling_rng_1D_get(SAMPLING_TRANSPARENCY);
  float accum = 0.0f;
  float rim_accum = 0.0f;

  for (int r = 0; r < 8; r++) {
    float angle = (float(r) + angle_offset) * 3.1415f * 0.25f * 0.5f;
    float2 offset = curvature_rotate(float2(1.0f, 0.0f), angle) * texel_size * sample_radius *
                    scale.xy;

    for (int i = 1; i <= n_samples; i++) {
      float sample_offset = float(i) * i_samples;
      float left_depth = drw_depth_screen_to_view(
          textureLod(hiz_tx, uvs + offset * sample_offset, 0.0f).r);
      float right_depth = drw_depth_screen_to_view(
          textureLod(hiz_tx, uvs - offset * sample_offset, 0.0f).r);

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
