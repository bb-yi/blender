/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_scene_color_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector").hide_value();
  b.add_output<decl::Color>("Color");
  b.add_output<decl::Float>("Alpha");
}

static int node_shader_gpu_scene_color(GPUMaterial *mat,
                                       bNode *node,
                                       bNodeExecData * /*execdata*/,
                                       GPUNodeStack *in,
                                       GPUNodeStack *out)
{
  const float use_explicit_vector = (in[0].link != nullptr) ? 1.0f : 0.0f;

  GPU_material_flag_set(mat, GPU_MATFLAG_SCENE_COLOR);
  return GPU_stack_link(
      mat, node, "node_scene_color", in, out, GPU_constant(&use_explicit_vector));
}

}  // namespace blender::nodes::node_shader_scene_color_cc

void register_node_type_sh_scene_color()
{
  namespace file_ns = blender::nodes::node_shader_scene_color_cc;

  static blender::bke::bNodeType ntype;

  sh_fn_node_type_base(&ntype, "ShaderNodeSceneColor", SH_NODE_SCENE_COLOR);
  ntype.ui_name = "Scene Color";
  ntype.ui_description = "Read the resolved Eevee scene color for filter materials";
  ntype.enum_name_legacy = "SCENE_COLOR";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = filter_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_scene_color;

  blender::bke::node_register_type(&ntype);
}
