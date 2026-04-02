/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_scene_time_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("Scale").default_value(1.0f).min(1e-6f).max(1000000.0f);
  b.add_output<decl::Float>("Frame");
  b.add_output<decl::Float>("Seconds");
  b.add_output<decl::Float>("Timeline");
  b.add_output<decl::Float>("Scaled Frame");
}

static int node_shader_gpu_scene_time(GPUMaterial *mat,
                                      bNode *node,
                                      bNodeExecData * /*execdata*/,
                                      GPUNodeStack *in,
                                      GPUNodeStack *out)
{
  GPU_material_set_time_dependent(mat);
  return GPU_stack_link(mat, node, "node_scene_time", in, out);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  return get_output_default(socket_out_->identifier, NodeItem::Type::Any);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_scene_time_cc

void register_node_type_sh_scene_time()
{
  namespace file_ns = nodes::node_shader_scene_time_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeSceneTime", SH_NODE_SCENE_TIME);
  ntype.ui_name = "Scene Time";
  ntype.ui_description =
      "Retrieve the current scene time in frames, seconds, normalized timeline, or scaled frames";
  ntype.enum_name_legacy = "SCENE_TIME";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_scene_time;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
