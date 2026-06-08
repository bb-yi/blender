/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_screenspace_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("View Position"_ustr).hide_value();
  b.add_output<decl::Color>("Scene Color"_ustr);
  b.add_output<decl::Float>("Scene Depth"_ustr);
}

static int node_shader_gpu_screenspace_info(GPUMaterial *mat,
                                            bNode *node,
                                            bNodeExecData * /*execdata*/,
                                            GPUNodeStack *in,
                                            GPUNodeStack *out)
{
  const float use_explicit_view_position = (in[0].link != nullptr) ? 1.0f : 0.0f;

  GPU_material_flag_set(mat, GPU_MATFLAG_SCREENSPACE_INFO | GPU_MATFLAG_LIGHTPROBE_ACCESS);
  return GPU_stack_link(
      mat, node, "node_screenspace_info", in, out, GPU_constant(&use_explicit_view_position));
}

}  // namespace nodes::node_shader_screenspace_info_cc

void register_node_type_sh_screenspace_info()
{
  namespace file_ns = nodes::node_shader_screenspace_info_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeScreenspaceInfo"_ustr, SH_NODE_SCREENSPACE_INFO);
  ntype.ui_name = "Screenspace Info";
  ntype.ui_description = "Sample the Eevee scene color and depth behind the current layer";
  ntype.enum_name_legacy = "SCREENSPACEINFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_screenspace_info;

  bke::node_register_type(ntype);
}

}  // namespace blender
