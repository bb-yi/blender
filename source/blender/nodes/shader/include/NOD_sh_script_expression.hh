/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "NOD_socket_items.hh"

#include "BKE_node.hh"

struct BlendDataReader;
struct BlendWriter;
struct StructRNA;

namespace blender::nodes {

struct ShScriptExpressionVariablesAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = NodeShaderScriptExpressionVariable;
  static StructRNA **item_srna;
  static constexpr StringRefNull node_idname = "ShaderNodeScriptExpression";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  static constexpr bool has_name_validation = true;
  static constexpr char unique_name_separator = '_';

  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_sh_script_expression_variable_add";
    static constexpr StringRefNull remove_item = "NODE_OT_sh_script_expression_variable_remove";
    static constexpr StringRefNull move_item = "NODE_OT_sh_script_expression_variable_move";
  };

  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_sh_script_expression_variables";
  };

  struct rna_names {
    static constexpr StringRefNull items = "variables";
    static constexpr StringRefNull active_index = "active_variable_index";
  };

  static socket_items::SocketItemsRef<ItemT> get_items_from_node(bNode &node);
  static void copy_item(const ItemT &src, ItemT &dst);
  static void destruct_item(ItemT *item);
  static eNodeSocketDatatype get_socket_type(const ItemT &item);
  static char **get_name(ItemT &item);
  static bool supports_socket_type(eNodeSocketDatatype socket_type, int ntree_type);
  static std::string validate_name(StringRef name);
  static void init_with_socket_type_and_name(bNode &node,
                                             ItemT &item,
                                             eNodeSocketDatatype socket_type,
                                             const char *name);
  static std::string socket_identifier_for_item(const ItemT &item);
  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);
};

}  // namespace blender::nodes
