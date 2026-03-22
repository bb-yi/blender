/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_render_info(out float width, out float height, out float3 frag_coord)
{
  width = float(uniform_buf.film.extent.x);
  height = float(uniform_buf.film.extent.y);
#ifdef GPU_FRAGMENT_SHADER
  frag_coord = gl_FragCoord.xyz;
#else
  frag_coord = float3(0.0f);
#endif
}
