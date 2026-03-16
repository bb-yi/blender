/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "UI_interface.hh"

namespace blender::nodes::node_shader_render_texture_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector").hide_value();
  b.add_output<decl::Color>("Color");
  b.add_output<decl::Float>("Alpha");
}

static void node_shader_buts(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "render_texture", UI_ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_shader_init_render_texture(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = -1;
}

static int node_shader_gpu_render_texture(GPUMaterial *mat,
                                          bNode * /*node*/,
                                          bNodeExecData * /*execdata*/,
                                          GPUNodeStack * /*in*/,
                                          GPUNodeStack *out)
{
  GPU_material_flag_set(mat, GPU_MATFLAG_RENDER_TEXTURE);
  GPU_link(mat, "set_rgba_zero", &out[0].link);
  GPU_link(mat, "set_value_zero", &out[1].link);
  return true;
}

}  // namespace blender::nodes::node_shader_render_texture_cc

void register_node_type_sh_render_texture()
{
  namespace file_ns = blender::nodes::node_shader_render_texture_cc;

  static blender::bke::bNodeType ntype;

  sh_fn_node_type_base(&ntype, "ShaderNodeRenderTexture", SH_NODE_RENDER_TEXTURE);
  ntype.ui_name = "Render Texture";
  ntype.ui_description = "Sample an Eevee render texture generated from a scene camera";
  ntype.enum_name_legacy = "RENDER_TEXTURE";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts;
  ntype.initfunc = file_ns::node_shader_init_render_texture;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_render_texture;

  blender::bke::node_register_type(&ntype);
}
