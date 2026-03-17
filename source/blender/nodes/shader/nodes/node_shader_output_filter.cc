/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_output_filter_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Color").default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_input<decl::Float>("Opacity").default_value(1.0f).min(0.0f).max(1.0f);
}

static int node_shader_gpu_output_filter(GPUMaterial *mat,
                                         bNode *node,
                                         bNodeExecData * /*execdata*/,
                                         GPUNodeStack *in,
                                         GPUNodeStack * /*out*/)
{
  GPUNodeLink *outlink_filter = nullptr;
  GPUNodeLink *color = in[0].link ? in[0].link : GPU_constant(in[0].vec);
  GPUNodeLink *opacity = in[1].link ? in[1].link : GPU_constant(&in[1].vec[0]);

  GPU_material_flag_set(mat, GPU_MATFLAG_FILTER_MATERIAL);
  if (!GPU_link(mat, "node_output_filter", color, opacity, &outlink_filter)) {
    return false;
  }
  GPU_material_output_filter(mat, outlink_filter);
  return true;
}

}  // namespace blender::nodes::node_shader_output_filter_cc

void register_node_type_sh_output_filter()
{
  namespace file_ns = blender::nodes::node_shader_output_filter_cc;

  static blender::bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeOutputFilter", SH_NODE_OUTPUT_FILTER);
  ntype.ui_name = "Filter Output";
  ntype.ui_description = "Output color for an Eevee fullscreen filter material";
  ntype.enum_name_legacy = "OUTPUT_FILTER";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_output_filter;
  ntype.no_muting = true;

  blender::bke::node_register_type(&ntype);
}
