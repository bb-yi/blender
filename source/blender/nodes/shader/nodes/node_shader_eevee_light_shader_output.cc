/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_eevee_light_shader_output_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Color").default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Float>("Intensity").default_value(1.0f).min(0.0f);
  b.add_input<decl::Float>("Attenuation").default_value(1.0f).min(0.0f);
}

static int node_shader_gpu_eevee_light_shader_output(GPUMaterial *mat,
                                                     bNode *node,
                                                     bNodeExecData * /*execdata*/,
                                                     GPUNodeStack *in,
                                                     GPUNodeStack *out)
{
  if (!in[0].hasinput && !in[1].hasinput && !in[2].hasinput && !node_socket_not_white(in[0]) &&
      in[1].socket_is_one() && in[2].socket_is_one())
  {
    return true;
  }

  GPUNodeLink *outlink_light_shader = nullptr;
  GPU_stack_link(mat, node, "node_output_eevee_light_shader", in, out, &outlink_light_shader);
  GPU_material_output_light_shader(mat, outlink_light_shader);
  return true;
}

}  // namespace nodes::node_shader_eevee_light_shader_output_cc

void register_node_type_sh_eevee_light_shader_output()
{
  namespace file_ns = nodes::node_shader_eevee_light_shader_output_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(
      &ntype, "ShaderNodeEeveeLightShaderOutput", SH_NODE_EEVEE_LIGHT_SHADER_OUTPUT);
  ntype.enum_name_legacy = "EEVEE_LIGHT_SHADER_OUTPUT";
  ntype.ui_name = "Light Shader Output";
  ntype.ui_description = "Output custom Eevee direct-light color and attenuation for a light";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = light_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_eevee_light_shader_output;
  ntype.no_muting = true;

  bke::node_register_type(ntype);
}

}  // namespace blender
