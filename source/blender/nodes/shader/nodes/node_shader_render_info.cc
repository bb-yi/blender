/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_render_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Vector>("Frag Coord");
  b.add_output<decl::Float>("Width");
  b.add_output<decl::Float>("Height");
  b.add_output<decl::Vector>("Resolution");
  b.add_output<decl::Float>("Current Sample");
  b.add_output<decl::Float>("Total Samples");
}

static int node_shader_gpu_render_info(GPUMaterial *mat,
                                       bNode *node,
                                       bNodeExecData * /*execdata*/,
                                       GPUNodeStack *in,
                                       GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_render_info", in, out);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  return get_output_default(socket_out_->identifier, NodeItem::Type::Any);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_render_info_cc

void register_node_type_sh_render_info()
{
  namespace file_ns = nodes::node_shader_render_info_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeRenderInfo", SH_NODE_RENDER_INFO);
  ntype.ui_name = "Render Info";
  ntype.ui_description = "Retrieve Eevee render dimensions, sample information, and normalized fragment coordinates";
  ntype.enum_name_legacy = "RENDER_INFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_render_info;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
