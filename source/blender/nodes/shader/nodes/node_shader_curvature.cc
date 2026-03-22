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

namespace nodes::node_shader_curvature_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("Samples").min(1.0f).max(64.0f).default_value(8.0f);
  b.add_input<decl::Float>("Sample Radius").min(0.0f).max(1000.0f).default_value(1.0f);
  b.add_input<decl::Float>("Thickness").min(0.0001f).max(1000.0f).default_value(1.0f);
  b.add_input<decl::Vector>("Scale").default_value(float3(1.0f, 1.0f, 0.0f));
  b.add_output<decl::Float>("Scene Curvature");
  b.add_output<decl::Float>("Scene Rim");
}

static void node_shader_init_curvature(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = 0; /* Local */
}

static void node_shader_buts_curvature(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "local", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static int node_shader_gpu_curvature(GPUMaterial *mat,
                                     bNode *node,
                                     bNodeExecData * /*execdata*/,
                                     GPUNodeStack *in,
                                     GPUNodeStack *out)
{
  GPU_material_flag_set(mat, GPU_MATFLAG_DIFFUSE);
  if (node->custom1) {
    GPU_material_flag_set(mat, GPU_MATFLAG_RAYCAST);
    return GPU_stack_link(mat, node, "node_screenspace_curvature_local", in, out);
  }
  return GPU_stack_link(mat, node, "node_screenspace_curvature", in, out);
}

}  // namespace nodes::node_shader_curvature_cc

void register_node_type_sh_curvature()
{
  namespace file_ns = nodes::node_shader_curvature_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeCurvature", SH_NODE_CURVATURE);
  ntype.ui_name = "Curvature";
  ntype.ui_description = "Sample Goo-style screen-space curvature and rim information";
  ntype.enum_name_legacy = "CURVATURE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.initfunc = file_ns::node_shader_init_curvature;
  ntype.draw_buttons = file_ns::node_shader_buts_curvature;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_curvature;

  bke::node_register_type(ntype);
}

}  // namespace blender
