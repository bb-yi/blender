/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender {

namespace nodes::node_shader_portal_out_cc {

NODE_STORAGE_FUNCS(NodeShaderPortal)

static bool is_portal_in_node(const bNode &node)
{
  return node.type_legacy == SH_NODE_PORTAL_IN && node.storage != nullptr;
}

static const bNode *find_single_portal_input(const bNodeTree &ntree)
{
  const bNode *single_input = nullptr;
  for (const bNode &node : ntree.nodes) {
    if (!is_portal_in_node(node)) {
      continue;
    }
    const NodeShaderPortal &storage = *static_cast<const NodeShaderPortal *>(node.storage);
    if (storage.name[0] == '\0') {
      continue;
    }
    if (single_input != nullptr) {
      return nullptr;
    }
    single_input = &node;
  }
  return single_input;
}

static void initialize_portal_storage(const bNodeTree *ntree, NodeShaderPortal &storage)
{
  storage.data_type = SOCK_FLOAT;
  STRNCPY(storage.name, "Portal");
  if (ntree == nullptr) {
    return;
  }
  const bNode *single_input = find_single_portal_input(*ntree);
  if (single_input == nullptr) {
    return;
  }
  const NodeShaderPortal &input_storage = *static_cast<const NodeShaderPortal *>(
      single_input->storage);
  STRNCPY(storage.name, input_storage.name);
  storage.data_type = input_storage.data_type;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Output", "Output_Float");
  b.add_output<decl::Vector>("Output", "Output_Vector");
  b.add_output<decl::Color>("Output", "Output_Color");
  b.add_output<decl::Shader>("Output", "Output_Shader");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  ui::Layout *row = &layout.row(true);
  row->prop(ptr, "portal_name", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  if (const bNode *node = static_cast<const bNode *>(ptr->data)) {
    PointerRNA op_ptr = row->op("node.jump_to_shader_portal_in", "", ICON_VIEWZOOM);
    RNA_int_set(&op_ptr, "portal_out_node_id", node->identifier);
  }
  layout.prop(ptr, "data_type", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  NodeShaderPortal *data = MEM_new<NodeShaderPortal>(__func__);
  initialize_portal_storage(ntree, *data);
  node->storage = data;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const NodeShaderPortal &storage = node_storage(*node);
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);
  for (bNodeSocket &socket : node->outputs) {
    bke::node_set_socket_availability(*ntree, socket, socket.type == data_type);
  }
}

static int node_shader_gpu_portal_out(GPUMaterial * /*mat*/,
                                      bNode *node,
                                      bNodeExecData *execdata,
                                      GPUNodeStack * /*in*/,
                                      GPUNodeStack *out)
{
  node_shader_gpu_stack_from_portal_out(*node, static_cast<bNodeStack *>(execdata->data), out);
  return 1;
}

}  // namespace nodes::node_shader_portal_out_cc

void register_node_type_sh_portal_out()
{
  namespace file_ns = nodes::node_shader_portal_out_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodePortalOut", SH_NODE_PORTAL_OUT);
  ntype.ui_name = "Portal Out";
  ntype.ui_description =
      "Read a named typed value from a portal input in the same shader tree";
  ntype.enum_name_legacy = "PORTAL_OUT";
  ntype.nclass = NODE_CLASS_LAYOUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  ntype.gpu_fn = file_ns::node_shader_gpu_portal_out;
  bke::node_type_storage(
      ntype, "NodeShaderPortal", node_free_standard_storage, node_copy_standard_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
