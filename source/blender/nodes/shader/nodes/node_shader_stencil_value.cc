/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_stencil_value_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("UV"_ustr).hide_value().description(
      "Screen coordinates matching Render Info Frag Coord; unconnected reads the current pixel");
  b.add_output<decl::Float>("Value"_ustr)
      .description(
          "Integer user stencil value from 0 to 15, captured after early markers in this render "
          "sample. "
          "Zero outside Eevee surface shading. Cannot drive opacity or geometry");
}

static int node_gpu(GPUMaterial *mat,
                    bNode *node,
                    bNodeExecData * /*execdata*/,
                    GPUNodeStack *in,
                    GPUNodeStack *out)
{
  GPU_material_stencil_value_used_set(mat);
  const float use_uv = in[0].link != nullptr ? 1.0f : 0.0f;
  return GPU_stack_link(mat, node, "node_stencil_value", in, out, GPU_constant(&use_uv));
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  return get_output_default(socket_out_->identifier, NodeItem::Type::Any);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace blender::nodes::node_shader_stencil_value_cc

namespace blender {
void register_node_type_sh_stencil_value()
{
  namespace file_ns = nodes::node_shader_stencil_value_cc;
  static bke::bNodeType ntype;
  sh_node_type_base(&ntype, "ShaderNodeStencilValue"_ustr, SH_NODE_STENCIL_VALUE);
  ntype.ui_name = "Stencil Value";
  ntype.ui_description = "Read the current view's immutable early user stencil snapshot";
  ntype.enum_name_legacy = "STENCIL_VALUE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_gpu;
  ntype.materialx_fn = file_ns::node_shader_materialx;
  bke::node_register_type(ntype);
}
}  // namespace blender
