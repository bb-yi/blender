/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "DNA_object_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_filter_object_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Vector>("Location")
      .description("World-space location of the selected object");
  b.add_output<decl::Vector>("Rotation")
      .description("World-space Euler rotation of the selected object in radians");
  b.add_output<decl::Vector>("Scale")
      .description("World-space scale of the selected object");
  b.add_output<decl::Color>("Color")
      .description("Viewport display color of the selected object");
}

static void node_shader_buts_filter_object_info(ui::Layout &layout,
                                                bContext * /*C*/,
                                                PointerRNA *ptr)
{
  layout.prop(ptr, "object", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static int node_shader_gpu_filter_object_info(GPUMaterial *mat,
                                              bNode *node,
                                              bNodeExecData * /*execdata*/,
                                              GPUNodeStack * /*in*/,
                                              GPUNodeStack *out)
{
  const float object_index = float(
      GPU_material_filter_object_info_ensure(mat, reinterpret_cast<Object *>(node->id)));
  return GPU_stack_link(mat,
                        node,
                        "node_filter_object_info",
                        nullptr,
                        out,
                        GPU_constant(&object_index));
}

}  // namespace nodes::node_shader_filter_object_info_cc

void register_node_type_sh_filter_object_info()
{
  namespace file_ns = nodes::node_shader_filter_object_info_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeFilterObjectInfo", SH_NODE_FILTER_OBJECT_INFO);
  ntype.ui_name = "Filter Object Info";
  ntype.ui_description =
      "Read transform and display color data from a chosen object inside Eevee filter materials";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_filter_object_info;
  ntype.add_ui_poll = filter_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_filter_object_info;

  bke::node_register_type(ntype);
}

}  // namespace blender
