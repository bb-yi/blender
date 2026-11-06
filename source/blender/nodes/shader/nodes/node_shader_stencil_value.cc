/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_shader_stencil_value_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("UV"_ustr).hide_value().description(
      "Screen coordinates matching Render Info Frag Coord; unconnected reads the current pixel");
  b.add_output<decl::Float>("Value"_ustr)
      .default_value(0.0f)
      .description(
          "Integer value from 0 to 15: native stencil snapshot, or nearest writer surface "
          "reference. "
          "Zero outside Eevee surface shading. Cannot drive opacity or geometry");
  b.add_output<decl::Float>("Depth"_ustr)
      .default_value(0.0f)
      .description(
          "Positive linear view Z depth of the nearest stencil writer surface; zero in snapshot "
          "mode");
  b.add_output<decl::Float>("Valid"_ustr)
      .default_value(0.0f)
      .description(
          "One for a valid writer surface, including reference zero; zero in snapshot mode");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "source", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static int node_gpu(GPUMaterial *mat,
                    bNode *node,
                    bNodeExecData * /*execdata*/,
                    GPUNodeStack *in,
                    GPUNodeStack *out)
{
  const float use_uv = in[0].link != nullptr ? 1.0f : 0.0f;
  if (node->custom1 == 1) {
    GPU_material_stencil_surface_used_set(mat);
    return GPU_stack_link(mat, node, "node_stencil_surface", in, out, GPU_constant(&use_uv),
                          GPU_kernel_globals(), GPU_shading_data());
  }
  GPU_material_stencil_value_used_set(mat);
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
  ntype.ui_description =
      "Sample the current view's stencil snapshot or nearest stencil writer surface";
  ntype.enum_name_legacy = "STENCIL_VALUE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_gpu;
  ntype.materialx_fn = file_ns::node_shader_materialx;
  bke::node_register_type(ntype);
}
}  // namespace blender
