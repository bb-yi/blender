/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_render_info(out float3 frag_coord, out float width, out float height)
{
  width = float(uniform_buf.film.extent.x);
  height = float(uniform_buf.film.extent.y);
#ifdef GPU_FRAGMENT_SHADER
  float2 extent = max(float2(width, height), float2(1.0f));
  frag_coord = float3(gl_FragCoord.xy / extent, gl_FragCoord.z);
#else
  frag_coord = float3(0.0f);
#endif
}
