/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_eevee_light_shader_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Default Color"_ustr);
  b.add_output<decl::Float>("Default Intensity"_ustr);
  b.add_output<decl::Float>("Default Attenuation"_ustr);
  b.add_output<decl::Float>("Distance"_ustr);
  b.add_output<decl::Vector>("Light Space"_ustr);
  b.add_output<decl::Vector>("Direction"_ustr);
  b.add_output<decl::Vector>("World Position"_ustr);
  b.add_output<decl::Vector>("Rotation"_ustr).description("XYZ Euler rotation of the light in radians");
}

static int node_shader_gpu_eevee_light_shader_info(GPUMaterial *mat,
                                                   bNode *node,
                                                   bNodeExecData * /*execdata*/,
                                                   GPUNodeStack *in,
                                                   GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_eevee_light_shader_info", in, out);
}

}  // namespace nodes::node_shader_eevee_light_shader_info_cc

void register_node_type_sh_eevee_light_shader_info()
{
  namespace file_ns = nodes::node_shader_eevee_light_shader_info_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeEeveeLightShaderInfo"_ustr, SH_NODE_EEVEE_LIGHT_SHADER_INFO);
  ntype.enum_name_legacy = "EEVEE_LIGHT_SHADER_INFO";
  ntype.ui_name = "Light Shader Info";
  ntype.ui_description = "Read the current Eevee light shader evaluation inputs";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = light_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_eevee_light_shader_info;

  bke::node_register_type(ntype);
}

}  // namespace blender
