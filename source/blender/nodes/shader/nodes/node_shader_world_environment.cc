/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "BLI_math_vector.h"

namespace blender {

namespace nodes::node_shader_world_environment_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Direction")
      .default_value(float3(0.0f, 0.0f, 0.0f))
      .hide_value()
      .description("World-space direction used to sample the world environment");
  b.add_output<decl::Color>("Color");
}

static int node_shader_gpu_world_environment(GPUMaterial *mat,
                                             bNode *node,
                                             bNodeExecData * /*execdata*/,
                                             GPUNodeStack *in,
                                             GPUNodeStack *out)
{
  if (!in[0].link && is_zero_v3(in[0].vec)) {
    GPU_link(mat, "world_view_direction_get", &in[0].link);
  }
  return GPU_stack_link(mat, node, "node_world_environment", in, out);
}

}  // namespace nodes::node_shader_world_environment_cc

void register_node_type_sh_world_environment()
{
  namespace file_ns = nodes::node_shader_world_environment_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeWorldEnvironment", SH_NODE_WORLD_ENVIRONMENT);
  ntype.ui_name = "World Environment";
  ntype.ui_description =
      "Sample Eevee world environment color from a custom direction, ignoring "
      "occluding geometry";
  ntype.enum_name_legacy = "WORLD_ENVIRONMENT";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_world_environment;

  bke::node_register_type(ntype);
}

}  // namespace blender
