/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_npr_refraction_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Image>("Combined Color"_ustr);
  b.add_output<decl::Image>("Position"_ustr);
}

static int node_shader_gpu_npr_refraction(GPUMaterial *mat,
                                          bNode *node,
                                          bNodeExecData * /*execdata*/,
                                          GPUNodeStack *in,
                                          GPUNodeStack *out)
{
  GPU_material_flag_set(mat, GPU_MATFLAG_NPR_REFRACTION);
  return GPU_stack_link(mat, node, "npr_refraction", in, out);
}

}  // namespace nodes::node_shader_npr_refraction_cc

void register_node_type_sh_npr_refraction()
{
  namespace file_ns = nodes::node_shader_npr_refraction_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeNPR_Refraction"_ustr, SH_NODE_NPR_REFRACTION);
  ntype.enum_name_legacy = "NPR_REFRACTION";
  ntype.ui_name = "NPR Refraction";
  ntype.ui_description = "Read refraction buffers inside the NPR shader tree";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = npr_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_npr_refraction;

  bke::node_register_type(ntype);
}

}  // namespace blender
