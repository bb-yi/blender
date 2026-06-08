/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_npr_input_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Image>("Combined Color"_ustr);
  b.add_output<decl::Image>("Diffuse Color"_ustr);
  b.add_output<decl::Image>("Diffuse Direct"_ustr);
  b.add_output<decl::Image>("Diffuse Indirect"_ustr);
  b.add_output<decl::Image>("Specular Color"_ustr);
  b.add_output<decl::Image>("Specular Direct"_ustr);
  b.add_output<decl::Image>("Specular Indirect"_ustr);
  b.add_output<decl::Image>("Position"_ustr);
  b.add_output<decl::Image>("Normal"_ustr);
}

static int node_shader_gpu_npr_input(GPUMaterial *mat,
                                     bNode *node,
                                     bNodeExecData * /*execdata*/,
                                     GPUNodeStack *in,
                                     GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "npr_input", in, out);
}

}  // namespace nodes::node_shader_npr_input_cc

void register_node_type_sh_npr_input()
{
  namespace file_ns = nodes::node_shader_npr_input_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeNPR_Input"_ustr, SH_NODE_NPR_INPUT);
  ntype.enum_name_legacy = "NPR_INPUT";
  ntype.ui_name = "NPR Input";
  ntype.ui_description = "Read NPR render buffers inside the NPR shader tree";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = npr_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_npr_input;

  bke::node_register_type(ntype);
}

}  // namespace blender
