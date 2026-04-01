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

namespace nodes::node_shader_filter_object_mask_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Mask")
      .description("Mask pixels that belong to the selected object using Eevee Cryptomatte data");
}

static void node_shader_buts_filter_object_mask(ui::Layout &layout,
                                                bContext * /*C*/,
                                                PointerRNA *ptr)
{
  layout.prop(ptr, "object", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static int node_shader_gpu_filter_object_mask(GPUMaterial *mat,
                                              bNode *node,
                                              bNodeExecData * /*execdata*/,
                                              GPUNodeStack * /*in*/,
                                              GPUNodeStack *out)
{
  const float object_index = float(
      GPU_material_filter_object_info_ensure(mat, reinterpret_cast<Object *>(node->id)));
  return GPU_stack_link(
      mat, node, "node_filter_object_mask", nullptr, out, GPU_constant(&object_index));
}

}  // namespace nodes::node_shader_filter_object_mask_cc

void register_node_type_sh_filter_object_mask()
{
  namespace file_ns = nodes::node_shader_filter_object_mask_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeFilterObjectMask", SH_NODE_FILTER_OBJECT_MASK);
  ntype.ui_name = "Filter Object Mask";
  ntype.ui_description =
      "Create a fast object mask inside Eevee filter materials using Cryptomatte object IDs";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_filter_object_mask;
  ntype.add_ui_poll = filter_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_filter_object_mask;

  bke::node_register_type(ntype);
}

}  // namespace blender
