/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utils.hh"

#include "UI_interface_layout.hh"

#include "BKE_node_runtime.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender {

namespace nodes::node_shader_portal_in_cc {

NODE_STORAGE_FUNCS(NodeShaderPortal)

static bool is_portal_in_node(const bNode &node)
{
  return node.type_legacy == SH_NODE_PORTAL_IN && node.storage != nullptr;
}

static std::string make_unique_portal_name(const bNodeTree &ntree, const bNode *skip_node)
{
  return BLI_uniquename_cb(
      [&](const StringRef name) {
        for (const bNode &other_node : ntree.nodes) {
          if (&other_node == skip_node || !is_portal_in_node(other_node)) {
            continue;
          }
          const NodeShaderPortal &other_storage = *static_cast<const NodeShaderPortal *>(
              other_node.storage);
          if (StringRefNull(other_storage.name) == name) {
            return true;
          }
        }
        return false;
      },
      '.',
      "Portal");
}

static void assign_unique_portal_name(const bNodeTree *ntree, bNode &node)
{
  NodeShaderPortal &storage = node_storage(node);
  STRNCPY(storage.name, "Portal");
  if (ntree == nullptr) {
    return;
  }
  const std::string unique_name = make_unique_portal_name(*ntree, &node);
  STRNCPY(storage.name, unique_name.c_str());
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Input", "Input_Float")
      .min(-10000.0f)
      .max(10000.0f)
      .is_default_link_socket();
  b.add_input<decl::Vector>("Input", "Input_Vector").is_default_link_socket();
  b.add_input<decl::Color>("Input", "Input_Color")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .is_default_link_socket();
  b.add_input<decl::Shader>("Input", "Input_Shader").is_default_link_socket();
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "portal_name", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "data_type", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree *ntree, bNode *node)
{
  NodeShaderPortal *data = MEM_new<NodeShaderPortal>(__func__);
  data->data_type = SOCK_FLOAT;
  node->storage = data;
  assign_unique_portal_name(ntree, *node);
}

static void node_copy(bNodeTree *dest_ntree, bNode *dest_node, const bNode *src_node)
{
  node_copy_standard_storage(dest_ntree, dest_node, src_node);
  const bNodeTree *src_ntree = (src_node->runtime != nullptr) ? src_node->runtime->owner_tree :
                                                             nullptr;
  if (dest_ntree != nullptr && src_ntree == dest_ntree) {
    assign_unique_portal_name(dest_ntree, *dest_node);
  }
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const NodeShaderPortal &storage = node_storage(*node);
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);
  for (bNodeSocket &socket : node->inputs) {
    bke::node_set_socket_availability(*ntree, socket, socket.type == data_type);
  }
}

}  // namespace nodes::node_shader_portal_in_cc

void register_node_type_sh_portal_in()
{
  namespace file_ns = nodes::node_shader_portal_in_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodePortalIn", SH_NODE_PORTAL_IN);
  ntype.ui_name = "Portal In";
  ntype.ui_description =
      "Store a named typed value that can be reused by portal outputs in the same shader tree";
  ntype.enum_name_legacy = "PORTAL_IN";
  ntype.nclass = NODE_CLASS_LAYOUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  bke::node_type_storage(ntype, "NodeShaderPortal", node_free_standard_storage, file_ns::node_copy);

  bke::node_register_type(ntype);
}

}  // namespace blender
