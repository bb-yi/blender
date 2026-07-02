/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include <algorithm>

#include "BLO_read_write.hh"

#include "DNA_array_utils.hh"

#include "BLI_string.h"

#include "BKE_context.hh"

#include "BLT_translation.hh"

#include "NOD_filter_graph.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"

#include "node_shader_util.hh"

namespace blender {

namespace nodes {

NodeShaderFilterOutput *ensure_shader_filter_output_storage(bNode &node)
{
  if (node.storage != nullptr) {
    NodeShaderFilterOutput *data = static_cast<NodeShaderFilterOutput *>(node.storage);
    if (data->items == nullptr || data->items_num <= 0) {
      MEM_SAFE_DELETE(data->items);
      data->items = MEM_new_array<NodeEeveeFilterGraphSocketItem>(1, __func__);
      data->items[0].name = BLI_strdup(DATA_("Image"));
      data->items[0].identifier = 0;
      data->items_num = 1;
      data->active_index = 0;
      data->next_identifier = std::max(data->next_identifier, 1);
    }
    else {
      int next_identifier = data->next_identifier;
      for (const int i : IndexRange(data->items_num)) {
        next_identifier = std::max(next_identifier, data->items[i].identifier + 1);
      }
      data->next_identifier = next_identifier;
      data->active_index = std::clamp(data->active_index, 0, data->items_num - 1);
    }
    return data;
  }

  NodeShaderFilterOutput *data = MEM_new<NodeShaderFilterOutput>(__func__);
  data->items = MEM_new_array<NodeEeveeFilterGraphSocketItem>(1, __func__);
  data->items[0].name = BLI_strdup(DATA_("Image"));
  data->items[0].identifier = data->next_identifier++;
  data->items_num = 1;
  data->active_index = 0;
  node.storage = data;
  return data;
}

}  // namespace nodes

namespace nodes::node_shader_output_filter_cc {

NODE_STORAGE_FUNCS(NodeShaderFilterOutput);

static std::string color_identifier_for_item(const NodeEeveeFilterGraphSocketItem &item)
{
  return item.identifier == 0 ? "Color" : "Color_" + std::to_string(item.identifier);
}

static std::string alpha_identifier_for_item(const NodeEeveeFilterGraphSocketItem &item)
{
  return item.identifier == 0 ? "Alpha" : "Alpha_" + std::to_string(item.identifier);
}

static std::string color_name_for_item(const NodeEeveeFilterGraphSocketItem &item)
{
  if (item.identifier == 0) {
    return "Color";
  }
  return std::string(item.name ? item.name : "") + " Color";
}

static std::string alpha_name_for_item(const NodeEeveeFilterGraphSocketItem &item)
{
  if (item.identifier == 0) {
    return "Alpha";
  }
  return std::string(item.name ? item.name : "") + " Alpha";
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  const bNode *node = b.node_or_null();
  if (node && node->storage != nullptr) {
    const NodeShaderFilterOutput &storage = node_storage(*node);
    if (storage.items != nullptr && storage.items_num > 0) {
      for (const int i : IndexRange(storage.items_num)) {
        const NodeEeveeFilterGraphSocketItem &item = storage.items[i];
        b.add_input<decl::Color>(color_name_for_item(item), color_identifier_for_item(item))
            .default_value({0.0f, 0.0f, 0.0f, 1.0f})
            .structure_type(StructureType::Dynamic);
        b.add_input<decl::Float>(alpha_name_for_item(item), alpha_identifier_for_item(item))
            .default_value(1.0f)
            .min(0.0f)
            .max(1.0f)
            .structure_type(StructureType::Dynamic);
      }
      return;
    }
  }

  b.add_input<decl::Color>("Color", "Color").default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_input<decl::Float>("Alpha", "Alpha").default_value(1.0f).min(0.0f).max(1.0f);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  ensure_shader_filter_output_storage(*node);
  node->flag |= NODE_DO_OUTPUT;
}

static void node_free_storage(bNode *node)
{
  if (node->storage == nullptr) {
    return;
  }
  socket_items::destruct_array<ShaderFilterOutputItemsAccessor>(*node);
  MEM_delete(static_cast<NodeShaderFilterOutput *>(node->storage));
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  if (src_node->storage == nullptr) {
    ensure_shader_filter_output_storage(*dst_node);
    return;
  }
  const NodeShaderFilterOutput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_new<NodeShaderFilterOutput>(__func__, dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;
  socket_items::copy_array<ShaderFilterOutputItemsAccessor>(*src_node, *dst_node);
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *ptr->data_as<bNode>();
  ensure_shader_filter_output_storage(node);
  socket_items::ui::draw_items_list_with_operators<ShaderFilterOutputItemsAccessor>(
      C, &layout, ntree, node);
}

static int node_shader_gpu_output_filter(GPUMaterial *mat,
                                         bNode *node,
                                         bNodeExecData * /*execdata*/,
                                         GPUNodeStack *in,
                                         GPUNodeStack * /*out*/)
{
  GPU_material_flag_set(mat, GPU_MATFLAG_FILTER_MATERIAL);

  NodeShaderFilterOutput *storage = ensure_shader_filter_output_storage(*node);
  if (storage == nullptr) {
    GPUNodeLink *outlink_filter = nullptr;
    GPUNodeLink *color = in[0].link ? in[0].link : GPU_constant(in[0].vec);
    GPUNodeLink *alpha = in[1].link ? in[1].link : GPU_constant(&in[1].vec[0]);
    if (!GPU_link(mat, "node_output_filter", color, alpha, &outlink_filter)) {
      return false;
    }
    GPU_material_output_filter(mat, outlink_filter);
    return true;
  }

  for (const int i : IndexRange(storage->items_num)) {
    if (i >= eevee_filter_graph_output_cap) {
      break;
    }
    GPUNodeLink *outlink_filter = nullptr;
    GPUNodeLink *color = in[i * 2].link ? in[i * 2].link : GPU_constant(in[i * 2].vec);
    GPUNodeLink *alpha = in[i * 2 + 1].link ? in[i * 2 + 1].link :
                                             GPU_constant(&in[i * 2 + 1].vec[0]);
    if (!GPU_link(mat, "node_output_filter", color, alpha, &outlink_filter)) {
      return false;
    }
    GPU_material_output_filter_item(mat, storage->items[i].identifier, outlink_filter);
  }
  return true;
}

static void node_remove_active_output_item(wmOperatorType *ot,
                                           const char *name,
                                           const char *idname,
                                           const char *description)
{
  ot->name = name;
  ot->idname = idname;
  ot->description = description;
  ot->poll = socket_items::ops::editable_node_active_poll<ShaderFilterOutputItemsAccessor>;
  ot->flag = OPTYPE_UNDO;

  ot->exec = [](bContext *C, wmOperator *op) -> wmOperatorStatus {
    PointerRNA node_ptr = socket_items::ops::get_active_node_to_operate_on(
        C, op, ShaderFilterOutputItemsAccessor::node_idname);
    if (node_ptr.data == nullptr) {
      return OPERATOR_CANCELLED;
    }
    bNode &node = *static_cast<bNode *>(node_ptr.data);
    ensure_shader_filter_output_storage(node);
    socket_items::SocketItemsRef<NodeEeveeFilterGraphSocketItem> ref =
        ShaderFilterOutputItemsAccessor::get_items_from_node(node);
    if (*ref.items_num <= 1) {
      return OPERATOR_CANCELLED;
    }

    int index_to_remove = ref.active_index ? *ref.active_index : 0;
    if (index_to_remove < 0 || index_to_remove >= *ref.items_num) {
      index_to_remove = *ref.items_num - 1;
    }
    dna::array::remove_index(ref.items,
                             ref.items_num,
                             ref.active_index,
                             index_to_remove,
                             ShaderFilterOutputItemsAccessor::destruct_item);
    socket_items::ops::update_after_item_array_change<ShaderFilterOutputItemsAccessor>(C,
                                                                                       node_ptr);
    return OPERATOR_FINISHED;
  };

  socket_items::ops::add_node_identifier_property(ot);
}

static void node_operators()
{
  WM_operatortype_append([](wmOperatorType *ot) {
    socket_items::ops::add_item<ShaderFilterOutputItemsAccessor>(
        ot,
        "Add Item",
        ShaderFilterOutputItemsAccessor::operator_idnames::add_item.c_str(),
        "Add item below active item");
  });
  WM_operatortype_append([](wmOperatorType *ot) {
    node_remove_active_output_item(
        ot,
        "Remove Item",
        ShaderFilterOutputItemsAccessor::operator_idnames::remove_item.c_str(),
        "Remove active item");
  });
  WM_operatortype_append([](wmOperatorType *ot) {
    socket_items::ops::move_active_item<ShaderFilterOutputItemsAccessor>(
        ot,
        "Move Item",
        ShaderFilterOutputItemsAccessor::operator_idnames::move_item.c_str(),
        "Move active item");
  });
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  if (node.storage == nullptr) {
    return;
  }
  socket_items::blend_write<ShaderFilterOutputItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  if (node.storage == nullptr) {
    ensure_shader_filter_output_storage(node);
    return;
  }
  socket_items::blend_read_data<ShaderFilterOutputItemsAccessor>(&reader, node);
  ensure_shader_filter_output_storage(node);
}

}  // namespace nodes::node_shader_output_filter_cc

void register_node_type_sh_output_filter()
{
  namespace file_ns = nodes::node_shader_output_filter_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeOutputFilter", SH_NODE_OUTPUT_FILTER);
  ntype.ui_name = "Filter Output";
  ntype.ui_description = "Output color for an Eevee fullscreen filter material";
  ntype.enum_name_legacy = "OUTPUT_FILTER";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_init;
  ntype.draw_buttons_ex = file_ns::node_layout;
  ntype.add_ui_poll = filter_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_output_filter;
  ntype.register_operators = file_ns::node_operators;
  ntype.blend_write_storage_content = file_ns::node_blend_write;
  ntype.blend_data_read_storage_content = file_ns::node_blend_read;
  ntype.no_muting = true;
  bke::node_type_storage(
      ntype, "NodeShaderFilterOutput", file_ns::node_free_storage, file_ns::node_copy_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
