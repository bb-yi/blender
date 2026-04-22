/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender {

namespace nodes::node_shader_outline_control_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Line Color").default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_input<decl::Float>("Line Width").default_value(2.0f).min(0.0f).max(20.0f);
  b.add_input<decl::Float>("Depth Threshold").default_value(0.1f).min(0.0f).max(1.0f);
  b.add_input<decl::Float>("Normal Threshold").default_value(0.5f).min(0.0f).max(1.0f);
  b.add_input<decl::Int>("Outline ID").default_value(0).min(0).max(65535);
}

static int node_shader_gpu_outline_control(GPUMaterial *mat,
                                           bNode *node,
                                           bNodeExecData * /*execdata*/,
                                           GPUNodeStack *in,
                                           GPUNodeStack *out)
{
  GPUNodeLink *outlink = nullptr;
  GPU_stack_link(mat, node, "node_output_outline", in, out, &outlink);
  GPU_material_add_output_link_outline(mat, outlink);
  return true;
}

}  // namespace nodes::node_shader_outline_control_cc

void register_node_type_sh_outline_control()
{
  namespace file_ns = nodes::node_shader_outline_control_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeOutlineControl", SH_NODE_OUTLINE_CONTROL);
  ntype.enum_name_legacy = "OUTLINE_CONTROL";
  ntype.ui_name = "Outline Control";
  ntype.ui_description = "Write Eevee outline parameters for the built-in screen-space outline pass";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_outline_control;
  ntype.no_muting = true;

  bke::node_register_type(ntype);
}

}  // namespace blender
