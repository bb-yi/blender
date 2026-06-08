/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_basis_transform_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .min(-10000.0f)
      .max(10000.0f)
      .hide_value()
      .description("Point, vector, or normal to transform");
  b.add_input<decl::Vector>("Origin"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .min(-10000.0f)
      .max(10000.0f)
      .description("Origin of the custom basis when transforming points");
  b.add_input<decl::Vector>("X Axis"_ustr)
      .default_value({1.0f, 0.0f, 0.0f})
      .min(-10000.0f)
      .max(10000.0f)
      .description("X axis of the custom basis");
  b.add_input<decl::Vector>("Y Axis"_ustr)
      .default_value({0.0f, 1.0f, 0.0f})
      .min(-10000.0f)
      .max(10000.0f)
      .description("Y axis of the custom basis");
  b.add_input<decl::Vector>("Z Axis"_ustr)
      .default_value({0.0f, 0.0f, 1.0f})
      .min(-10000.0f)
      .max(10000.0f)
      .description("Z axis of the custom basis");
  b.add_output<decl::Vector>("Vector"_ustr).description("Transformed value");
}

static void node_shader_buts_basis_transform(ui::Layout &layout,
                                             bContext * /*C*/,
                                             PointerRNA *ptr)
{
  layout.prop(
      ptr, "direction", ui::ITEM_R_SPLIT_EMPTY_NAME | ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.prop(
      ptr, "vector_type", ui::ITEM_R_SPLIT_EMPTY_NAME | ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.prop(
      ptr, "basis_input", ui::ITEM_R_SPLIT_EMPTY_NAME | ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.prop(ptr, "orthonormalize", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "fallback", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_shader_update_basis_transform(bNodeTree *ntree, bNode *node)
{
  const NodeShaderBasisTransform *storage = static_cast<NodeShaderBasisTransform *>(node->storage);
  if (storage == nullptr) {
    return;
  }

  bNodeSocket *origin_socket = bke::node_find_socket(*node, SOCK_IN, "Origin"_ustr);
  bNodeSocket *x_axis_socket = bke::node_find_socket(*node, SOCK_IN, "X Axis"_ustr);
  bNodeSocket *y_axis_socket = bke::node_find_socket(*node, SOCK_IN, "Y Axis"_ustr);
  bNodeSocket *z_axis_socket = bke::node_find_socket(*node, SOCK_IN, "Z Axis"_ustr);

  bke::node_set_socket_availability(
      *ntree, *origin_socket, storage->vector_type == SHD_VECT_TRANSFORM_TYPE_POINT);
  bke::node_set_socket_availability(
      *ntree, *x_axis_socket, storage->basis_input != SHD_BASIS_TRANSFORM_INPUT_YZ);
  bke::node_set_socket_availability(
      *ntree, *y_axis_socket, storage->basis_input != SHD_BASIS_TRANSFORM_INPUT_XZ);
  bke::node_set_socket_availability(
      *ntree, *z_axis_socket, storage->basis_input != SHD_BASIS_TRANSFORM_INPUT_XY);
}

static void node_shader_init_basis_transform(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = MEM_new<NodeShaderBasisTransform>("NodeShaderBasisTransform");
}

static int node_shader_gpu_basis_transform(GPUMaterial *mat,
                                           bNode *node,
                                           bNodeExecData * /*execdata*/,
                                           GPUNodeStack *in,
                                           GPUNodeStack *out)
{
  const NodeShaderBasisTransform *storage = static_cast<NodeShaderBasisTransform *>(node->storage);
  const float vector_type = float(storage->vector_type);
  const float direction = float(storage->direction);
  const float basis_input = float(storage->basis_input);
  const float fallback = float(storage->fallback);
  const float orthonormalize = storage->orthonormalize ? 1.0f : 0.0f;

  return GPU_stack_link(mat,
                        node,
                        "node_basis_transform",
                        in,
                        out,
                        GPU_constant(&vector_type),
                        GPU_constant(&direction),
                        GPU_constant(&basis_input),
                        GPU_constant(&fallback),
                        GPU_constant(&orthonormalize));
}

}  // namespace nodes::node_shader_basis_transform_cc

void register_node_type_sh_basis_transform()
{
  namespace file_ns = nodes::node_shader_basis_transform_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeBasisTransform"_ustr, SH_NODE_BASIS_TRANSFORM);
  ntype.ui_name = "Basis Transform";
  ntype.ui_description =
      "Transform a point, vector, or normal to or from a custom basis defined by axis inputs";
  ntype.enum_name_legacy = "BASIS_TRANSFORM";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_basis_transform;
  ntype.initfunc = file_ns::node_shader_init_basis_transform;
  ntype.updatefunc = file_ns::node_shader_update_basis_transform;
  ntype.add_ui_poll = eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_basis_transform;
  ntype.default_width = 150;
  ntype.minwidth = 120;
  bke::node_type_storage(
      ntype, "NodeShaderBasisTransform", node_free_standard_storage, node_copy_standard_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
