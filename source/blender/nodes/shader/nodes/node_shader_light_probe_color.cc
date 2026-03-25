/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_light_probe_color_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Direction")
      .default_value(float3(0.0f, 0.0f, 0.0f))
      .hide_value()
      .description("World-space direction used to sample Eevee light probe data");
  b.add_output<decl::Color>("Reflection");
  b.add_output<decl::Color>("Irradiance");
  b.add_output<decl::Color>("Combined");
}

static int node_shader_gpu_light_probe_color(GPUMaterial *mat,
                                             bNode *node,
                                             bNodeExecData * /*execdata*/,
                                             GPUNodeStack *in,
                                             GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_light_probe_color", in, out);
}

}  // namespace nodes::node_shader_light_probe_color_cc

void register_node_type_sh_light_probe_color()
{
  namespace file_ns = nodes::node_shader_light_probe_color_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeLightProbeColor", SH_NODE_LIGHT_PROBE_COLOR);
  ntype.ui_name = "Light Probe Color";
  ntype.ui_description =
      "Output Eevee reflection probe and irradiance probe colors for the surface";
  ntype.enum_name_legacy = "LIGHT_PROBE_COLOR";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_light_probe_color;

  bke::node_register_type(ntype);
}

}  // namespace blender
