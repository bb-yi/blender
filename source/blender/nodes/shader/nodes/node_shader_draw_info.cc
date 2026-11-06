/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_draw_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Result"_ustr)
      .description("One when the selected drawing context is active, otherwise zero");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "query", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static int node_shader_gpu_draw_info(GPUMaterial *mat,
                                     bNode *node,
                                     bNodeExecData * /*execdata*/,
                                     GPUNodeStack * /*in*/,
                                     GPUNodeStack *out)
{
  const float query = float(node->custom1);
  return GPU_link(mat, "node_draw_info", GPU_constant(&query), &out[0].link);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  return val(0.0f);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_draw_info_cc

void register_node_type_sh_draw_info()
{
  namespace file_ns = nodes::node_shader_draw_info_cc;

  static bke::bNodeType ntype;
  sh_node_type_base(&ntype, "ShaderNodeDrawInfo"_ustr, SH_NODE_DRAW_INFO);
  ntype.ui_name = "Draw Info";
  ntype.ui_description =
      "Test the current Eevee material, drawing, capture, or evaluation context";
  ntype.enum_name_legacy = "DRAW_INFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.default_width = 220;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.add_ui_poll = eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_draw_info;
  ntype.materialx_fn = file_ns::node_shader_materialx;
  bke::node_register_type(ntype);
}

}  // namespace blender
