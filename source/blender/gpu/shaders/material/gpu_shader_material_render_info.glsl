/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_render_info(out float3 frag_coord,
                      out float width,
                      out float height,
                      out float3 resolution,
                      out float current_sample,
                      out float total_samples)
{
#ifdef MAT_FILTER
  float2 render_extent = float2(filter_graph_output_extent());
#else
  float2 render_extent = float2(uniform_buf.film.extent);
#endif
  width = render_extent.x;
  height = render_extent.y;
  resolution = float3(width, height, 0.0f);
#ifdef GPU_FRAGMENT_SHADER
  float2 extent = max(float2(width, height), float2(1.0f));
  float2 uv = gl_FragCoord.xy / extent;
  frag_coord = float3(uv * uniform_buf.camera.uv_scale + uniform_buf.camera.uv_bias,
                      gl_FragCoord.z);
#else
  frag_coord = float3(0.0f);
#endif
  float2 samples = render_info_samples();
  current_sample = samples.x;
  total_samples = samples.y;
}
