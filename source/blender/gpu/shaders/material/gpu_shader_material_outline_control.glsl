/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_output_outline(
    float4 line_color, float line_width, float depth_threshold, float normal_threshold, float outline_id, Closure &dummy)
{
  dummy = Closure(0);
  output_outline(line_color, line_width, depth_threshold, normal_threshold, outline_id);
}
