/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_npr_output_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Color").hide_value();
}

static int node_shader_gpu_npr_output(GPUMaterial *mat,
                                      bNode * /*node*/,
                                      bNodeExecData * /*execdata*/,
                                      GPUNodeStack *in,
                                      GPUNodeStack * /*out*/)
{
  if (in[0].link != nullptr) {
    GPU_material_output_npr(mat, in[0].link);
  }
  return true;
}

}  // namespace nodes::node_shader_npr_output_cc

void register_node_type_sh_npr_output()
{
  namespace file_ns = nodes::node_shader_npr_output_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeNPR_Output", SH_NODE_NPR_OUTPUT);
  ntype.enum_name_legacy = "NPR_OUTPUT";
  ntype.ui_name = "NPR Output";
  ntype.ui_description = "Output color from the NPR shader tree";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = npr_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_npr_output;

  bke::node_register_type(ntype);
}

}  // namespace blender
