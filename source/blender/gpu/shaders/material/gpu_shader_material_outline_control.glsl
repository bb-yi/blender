/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_output_outline(
    float4 line_color,
    float line_alpha,
    float line_width,
    float depth_threshold,
    float depth_threshold_range,
    float depth_edge_width,
    float normal_threshold,
    float normal_threshold_range,
    float normal_edge_width,
    int outline_id,
    bool id_edge,
    float id_edge_width,
    bool freestyle_edge,
    [[resource_table]] KernelGlobals &kg,
    ShadingData &sd,
    Closure &dummy)
{
  dummy = Closure(0);
  line_color.a = saturate(line_color.a) * saturate(line_alpha);
  output_outline(line_color,
                 line_width,
                 depth_threshold,
                 depth_threshold_range,
                 depth_edge_width,
                 normal_threshold,
                 normal_threshold_range,
                 normal_edge_width,
                 float(outline_id),
                 id_edge,
                 id_edge_width,
                 freestyle_edge,
                 kg.object_infos_get(sd),
                 kg.resource_id_get(sd));
}
