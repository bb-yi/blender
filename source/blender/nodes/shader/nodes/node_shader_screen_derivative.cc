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

namespace nodes::node_shader_screen_derivative_cc {

NODE_STORAGE_FUNCS(NodeShaderDerivative)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("Value", "Value_Float").min(-10000.0f).max(10000.0f);
  b.add_input<decl::Vector>("Value", "Value_Vector");
  b.add_input<decl::Color>("Value", "Value_Color").default_value({0.0f, 0.0f, 0.0f, 1.0f});

  b.add_output<decl::Float>("Value", "Value_Float");
  b.add_output<decl::Vector>("Value", "Value_Vector");
  b.add_output<decl::Color>("Value", "Value_Color");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "operation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "data_type", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderDerivative *data = MEM_new<NodeShaderDerivative>(__func__);
  data->operation = NODE_SHADER_DERIVATIVE_DDX;
  data->data_type = SOCK_FLOAT;
  node->storage = data;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const NodeShaderDerivative &storage = node_storage(*node);
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);

  for (bNodeSocket &socket : node->inputs) {
    bke::node_set_socket_availability(*ntree, socket, socket.type == data_type);
  }
  for (bNodeSocket &socket : node->outputs) {
    bke::node_set_socket_availability(*ntree, socket, socket.type == data_type);
  }
}

static int gpu_shader_screen_derivative(GPUMaterial *mat,
                                        bNode *node,
                                        bNodeExecData * /*execdata*/,
                                        GPUNodeStack *in,
                                        GPUNodeStack *out)
{
  const NodeShaderDerivative &storage = node_storage(*node);
  const char *function_name = "node_ddx";
  switch (storage.operation) {
    case NODE_SHADER_DERIVATIVE_DDY:
      function_name = "node_ddy";
      break;
    case NODE_SHADER_DERIVATIVE_DDXY:
      function_name = "node_ddxy";
      break;
    case NODE_SHADER_DERIVATIVE_DDX:
    default:
      break;
  }
  return GPU_stack_link(mat, node, function_name, in, out);
}

}  // namespace nodes::node_shader_screen_derivative_cc

void register_node_type_sh_screen_derivative()
{
  namespace file_ns = nodes::node_shader_screen_derivative_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeScreenDerivative", SH_NODE_SCREEN_DERIVATIVE);
  ntype.ui_name = "Screen Derivative";
  ntype.ui_description = "Partial derivative of the input with respect to screen-space X, Y, or both";
  ntype.enum_name_legacy = "SCREEN_DERIVATIVE";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  ntype.add_ui_poll = eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::gpu_shader_screen_derivative;
  bke::node_type_storage(
      ntype, "NodeShaderDerivative", node_free_standard_storage, node_copy_standard_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
