/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_scene_color_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr).hide_value();
  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Float>("Alpha"_ustr);
}

static void node_shader_buts(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "source", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_shader_init_scene_color(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = SHD_SCENE_SOURCE_COLOR;
}

static int node_shader_gpu_scene_color(GPUMaterial *mat,
                                       bNode *node,
                                       bNodeExecData * /*execdata*/,
                                       GPUNodeStack *in,
                                       GPUNodeStack *out)
{
  const float use_explicit_vector = (in[0].link != nullptr) ? 1.0f : 0.0f;
  const float scene_source = float(node->custom1);

  GPU_material_flag_set(mat, GPU_MATFLAG_SCENE_COLOR);
  return GPU_stack_link(mat,
                        node,
                        "node_scene_color",
                        in,
                        out,
                        GPU_constant(&use_explicit_vector),
                        GPU_constant(&scene_source));
}

}  // namespace nodes::node_shader_scene_color_cc

void register_node_type_sh_scene_color()
{
  namespace file_ns = nodes::node_shader_scene_color_cc;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeSceneColor"_ustr, SH_NODE_SCENE_COLOR);
  ntype.ui_name = "Scene Color";
  ntype.ui_description =
      "Read Eevee scene color, depth, normal, or position for filter materials";
  ntype.enum_name_legacy = "SCENE_COLOR";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts;
  ntype.initfunc = file_ns::node_shader_init_scene_color;
  ntype.add_ui_poll = filter_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_scene_color;

  bke::node_register_type(ntype);
}

}  // namespace blender
