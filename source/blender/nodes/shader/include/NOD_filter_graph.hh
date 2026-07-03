/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "BKE_node.hh"

#include "NOD_socket_items.hh"

struct BlendDataReader;
struct BlendWriter;
struct Material;
struct StructRNA;

namespace blender {
struct bContext;
struct Main;
struct PointerRNA;
}

namespace blender::nodes {

constexpr int eevee_filter_graph_input_cap = 32;
constexpr int eevee_filter_graph_output_cap = 32;
constexpr StringRefNull eevee_filter_graph_tree_idname = "EeveeFilterGraphNodeTree";

struct FilterGraphSocketItemsAccessorBase : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = NodeEeveeFilterGraphSocketItem;
  static constexpr bool has_type = false;
  static constexpr bool has_name = true;

  static void copy_item(const ItemT &src, ItemT &dst);
  static void destruct_item(ItemT *item);
  static char **get_name(ItemT &item);
  static socket_items::SocketItemsRef<ItemT> get_items_from_node(bNode &node);
  static void init_with_name(bNode &node, ItemT &item, const char *name);
  static std::string socket_identifier_for_item(const ItemT &item);
  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);
};

struct ShaderFilterGraphInputItemsAccessor : public FilterGraphSocketItemsAccessorBase {
  static StructRNA **item_srna;
  static constexpr bool has_post_item_change = true;
  static constexpr StringRefNull node_idname = "ShaderNodeFilterGraphInput";
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_shader_filter_graph_input_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_shader_filter_graph_input_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_shader_filter_graph_input_item_move";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_shader_filter_graph_inputs";
  };
  struct rna_names {
    static constexpr StringRefNull items = "interface_items";
    static constexpr StringRefNull active_index = "active_index";
  };

  static socket_items::SocketItemsRef<ItemT> get_items_from_node(bNode &node);
  static void post_item_change(bContext *C, PointerRNA node_ptr);
  static void post_item_change(Main &bmain, bNodeTree &ntree, bNode &node);
};

struct ShaderFilterOutputItemsAccessor : public FilterGraphSocketItemsAccessorBase {
  static StructRNA **item_srna;
  static constexpr bool has_post_item_change = true;
  static constexpr StringRefNull node_idname = "ShaderNodeOutputFilter";
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_shader_filter_output_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_shader_filter_output_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_shader_filter_output_item_move";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_shader_filter_outputs";
  };
  struct rna_names {
    static constexpr StringRefNull items = "interface_items";
    static constexpr StringRefNull active_index = "active_index";
  };

  static socket_items::SocketItemsRef<ItemT> get_items_from_node(bNode &node);
  static void post_item_change(bContext *C, PointerRNA node_ptr);
  static void post_item_change(Main &bmain, bNodeTree &ntree, bNode &node);
};

struct EeveeFilterGraphMaterialItemsAccessor : public FilterGraphSocketItemsAccessorBase {
  static StructRNA **item_srna;
  static constexpr bool has_post_item_change = true;
  static constexpr StringRefNull node_idname = "EeveeFilterGraphNodeFilterMaterial";
  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_eevee_filter_graph_material_input_add";
    static constexpr StringRefNull remove_item = "NODE_OT_eevee_filter_graph_material_input_remove";
    static constexpr StringRefNull move_item = "NODE_OT_eevee_filter_graph_material_input_move";
    static constexpr StringRefNull sync_interface =
        "NODE_OT_eevee_filter_graph_material_sync_interface";
  };
  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_eevee_filter_graph_material_inputs";
  };
  struct rna_names {
    static constexpr StringRefNull items = "input_items";
    static constexpr StringRefNull active_index = "active_index";
  };

  static socket_items::SocketItemsRef<ItemT> get_items_from_node(bNode &node);
  static void post_item_change(bContext *C, PointerRNA node_ptr);
  static void post_item_change(Main &bmain, bNodeTree &ntree, bNode &node);
};

void register_node_tree_type_eevee_filter_graph();
void register_node_type_eevee_filter_graph_scene_color();
void register_node_type_eevee_filter_graph_aov_input();
void register_node_type_eevee_filter_graph_filter_material();
void register_node_type_eevee_filter_graph_stage_output();

void filter_graph_stage_output_activate(bNodeTree &ntree, bNode &node);
void filter_graph_stage_outputs_ensure(bNodeTree &ntree);
void filter_graph_tag_tree_changed(Main &bmain, bNodeTree &ntree);
bool filter_graph_filter_pass_has_pass_input(const bNode &node);
void filter_graph_filter_pass_material_changed(Main &bmain, bNodeTree &ntree, bNode &node);
void filter_graph_filter_pass_interface_changed(Main &bmain, bNodeTree &ntree, bNode &node);
void filter_graph_pass_input_interface_changed(Main &bmain, bNodeTree &ntree, bNode &node);
void filter_graph_filter_output_interface_changed(Main &bmain, bNodeTree &ntree, bNode &node);
NodeShaderFilterOutput *ensure_shader_filter_output_storage(bNode &node);
bool filter_graph_sync_material_interface_to_filter_pass(Main &bmain, bNodeTree &ntree, bNode &node);
bool filter_graph_sync_filter_pass_interface_from_material_storage(bNodeTree &ntree, bNode &node);
void filter_graph_sync_filter_pass_interfaces_from_materials(bNodeTree &ntree);

}  // namespace blender::nodes
