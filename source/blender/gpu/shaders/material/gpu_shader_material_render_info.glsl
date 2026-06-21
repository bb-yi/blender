/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_render_info(out float3 frag_coord,
                      out float width,
                      out float height,
                      out float3 resolution,
                      out float current_sample,
                      out float total_samples,
                      out float3 sample_offset)
{
  width = float(uniform_buf.film.extent.x);
  height = float(uniform_buf.film.extent.y);
  resolution = float3(width, height, 0.0f);
#ifdef GPU_FRAGMENT_SHADER
  float2 extent = max(float2(width, height), float2(1.0f));
  float2 uv = gl_FragCoord.xy / extent;
  frag_coord = float3(uv * uniform_buf.camera.uv_scale + uniform_buf.camera.uv_bias,
                      gl_FragCoord.z);
#else
  frag_coord = float3(0.0f);
#endif
#ifdef EEVEE_SAMPLING_DATA
  current_sample = float(sampling_buf.sample_index);
  total_samples = float(max(sampling_buf.sample_count, 1));
  sample_offset = float3(sampling_buf.sample_offset, 0.0f);
#else
  current_sample = 0.0f;
  total_samples = 1.0f;
  sample_offset = float3(0.0f);
#endif
}
