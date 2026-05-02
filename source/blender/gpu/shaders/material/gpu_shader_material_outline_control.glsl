/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_output_outline(
    float4 line_color,
    float line_alpha,
    float line_width,
    float depth_threshold,
    float normal_threshold,
    float outline_id,
    float id_edge,
    float freestyle_edge,
    Closure &dummy)
{
  dummy = Closure(0);
  line_color.a = saturate(line_color.a) * saturate(line_alpha);
  output_outline(line_color,
                 line_width,
                 depth_threshold,
                 normal_threshold,
                 outline_id,
                 id_edge > 0.5f,
                 freestyle_edge > 0.5f);
}
