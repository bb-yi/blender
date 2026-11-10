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
    float expansion_mask,
    float id_combine,
    float edge_priority,
    [[resource_table]] KernelGlobals &kg,
    ShadingData &sd,
    Closure &dummy)
{
  dummy = Closure(0);
  line_color.a = saturate(line_color.a) * saturate(line_alpha);
  uint4 surface_data = uint4(0u);
#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_OUTPUT) && defined(GPU_FRAGMENT_SHADER)
  float source_depth = gl_FragCoord.z;
#  ifdef MAT_DEPTH_OFFSET
  source_depth = material_depth_offset_frag_depth(kg, sd, nodetree_depth_offset(kg, sd));
#  endif
  surface_data = floatBitsToUint(float4(source_depth, sd.N));
#endif
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
                 expansion_mask,
                 id_combine,
                 edge_priority,
                 kg.object_infos_get(sd),
                 kg.resource_id_get(sd), surface_data);
}
