/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "BLO_read_write.hh"

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

void FilterGraphSocketItemsAccessorBase::copy_item(const ItemT &src, ItemT &dst)
{
  dst = src;
  dst.name = BLI_strdup_null(src.name);
}

void FilterGraphSocketItemsAccessorBase::destruct_item(ItemT *item)
{
  MEM_SAFE_DELETE(item->name);
}

char **FilterGraphSocketItemsAccessorBase::get_name(ItemT &item)
{
  return &item.name;
}

socket_items::SocketItemsRef<NodeEeveeFilterGraphSocketItem>
FilterGraphSocketItemsAccessorBase::get_items_from_node(bNode &node)
{
  if (STREQ(node.idname, ShaderFilterGraphInputItemsAccessor::node_idname.c_str())) {
    auto &storage = *static_cast<NodeShaderFilterGraphInput *>(node.storage);
    return {&storage.items, &storage.items_num, &storage.active_index};
  }
  if (STREQ(node.idname, ShaderFilterOutputItemsAccessor::node_idname.c_str())) {
    auto &storage = *ensure_shader_filter_output_storage(node);
    return {&storage.items, &storage.items_num, &storage.active_index};
  }
  auto &storage = *static_cast<NodeEeveeFilterGraphFilterMaterial *>(node.storage);
  return {&storage.items, &storage.items_num, &storage.active_index};
}

void FilterGraphSocketItemsAccessorBase::init_with_name(bNode &node, ItemT &item, const char *name)
{
  if (node.storage == nullptr) {
    return;
  }
  if (STREQ(node.idname, ShaderFilterGraphInputItemsAccessor::node_idname.c_str())) {
    auto &storage = *static_cast<NodeShaderFilterGraphInput *>(node.storage);
    item.identifier = storage.next_identifier++;
  }
  else if (STREQ(node.idname, ShaderFilterOutputItemsAccessor::node_idname.c_str())) {
    auto &storage = *ensure_shader_filter_output_storage(node);
    item.identifier = storage.next_identifier++;
  }
  else if (STREQ(node.idname, EeveeFilterGraphMaterialItemsAccessor::node_idname.c_str())) {
    auto &storage = *static_cast<NodeEeveeFilterGraphFilterMaterial *>(node.storage);
    item.identifier = storage.next_identifier++;
  }
  socket_items::set_item_name_and_make_unique<FilterGraphSocketItemsAccessorBase>(
      node, item, name);
}

std::string FilterGraphSocketItemsAccessorBase::socket_identifier_for_item(const ItemT &item)
{
  return "Image_" + std::to_string(item.identifier);
}

void FilterGraphSocketItemsAccessorBase::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void FilterGraphSocketItemsAccessorBase::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

socket_items::SocketItemsRef<NodeEeveeFilterGraphSocketItem>
ShaderFilterGraphInputItemsAccessor::get_items_from_node(bNode &node)
{
  auto &storage = *static_cast<NodeShaderFilterGraphInput *>(node.storage);
  return {&storage.items, &storage.items_num, &storage.active_index};
}

socket_items::SocketItemsRef<NodeEeveeFilterGraphSocketItem>
ShaderFilterOutputItemsAccessor::get_items_from_node(bNode &node)
{
  auto &storage = *ensure_shader_filter_output_storage(node);
  return {&storage.items, &storage.items_num, &storage.active_index};
}

}  // namespace nodes

namespace nodes::node_shader_filter_graph_input_cc {

NODE_STORAGE_FUNCS(NodeShaderFilterGraphInput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  if (node && tree && node->storage != nullptr) {
    const NodeShaderFilterGraphInput &storage = node_storage(*node);
    for (const int i : IndexRange(storage.items_num)) {
      const NodeEeveeFilterGraphSocketItem &item = storage.items[i];
      const StringRefNull name = item.name ? item.name : "";
      const std::string identifier =
          ShaderFilterGraphInputItemsAccessor::socket_identifier_for_item(item);
      b.add_output<decl::Image>(name, identifier)
          .socket_name_ptr(
              &tree->id, *ShaderFilterGraphInputItemsAccessor::item_srna, &item, "name")
          .structure_type(StructureType::Dynamic);
    }
  }
  b.add_output<decl::Extend>("", "__extend__").structure_type(StructureType::Dynamic);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeShaderFilterGraphInput *data = MEM_new<NodeShaderFilterGraphInput>(__func__);
  data->items = MEM_new_array<NodeEeveeFilterGraphSocketItem>(1, __func__);
  data->items[0].name = BLI_strdup(DATA_("Image"));
  data->items[0].identifier = data->next_identifier++;
  data->items_num = 1;
  data->active_index = 0;
  node->storage = data;
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<ShaderFilterGraphInputItemsAccessor>(*node);
  MEM_delete(static_cast<NodeShaderFilterGraphInput *>(node->storage));
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeShaderFilterGraphInput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_new<NodeShaderFilterGraphInput>(__func__, dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;
  socket_items::copy_array<ShaderFilterGraphInputItemsAccessor>(*src_node, *dst_node);
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *ptr->data_as<bNode>();
  socket_items::ui::draw_items_list_with_operators<ShaderFilterGraphInputItemsAccessor>(
      C, &layout, ntree, node);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  NodeEeveeFilterGraphSocketItem *new_item = nullptr;
  const bool keep_link =
      socket_items::try_add_item_via_any_extend_socket<ShaderFilterGraphInputItemsAccessor>(
          params.ntree, params.node, params.node, params.link, std::nullopt, &new_item);
  Main *bmain = params.C ? CTX_data_main(params.C) : params.bmain;
  if (keep_link && new_item != nullptr && bmain != nullptr) {
    ShaderFilterGraphInputItemsAccessor::post_item_change(*bmain, params.ntree, params.node);
  }
  return keep_link;
}

static int node_shader_gpu(GPUMaterial *mat,
                           bNode *node,
                           bNodeExecData * /*execdata*/,
                           GPUNodeStack * /*in*/,
                           GPUNodeStack *out)
{
  if (node->storage == nullptr) {
    return true;
  }

  const NodeShaderFilterGraphInput &storage = node_storage(*node);
  for (const int i : IndexRange(storage.items_num)) {
    if (i >= eevee_filter_graph_input_cap) {
      break;
    }
    float index = float(i);
    GPUNodeLink *outlink = nullptr;
    GPU_link(mat, "node_filter_graph_input", GPU_constant(&index), &outlink);
    out[i].link = outlink;
  }
  return true;
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ShaderFilterGraphInputItemsAccessor>();
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<ShaderFilterGraphInputItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<ShaderFilterGraphInputItemsAccessor>(&reader, node);
}

}  // namespace nodes::node_shader_filter_graph_input_cc

namespace nodes {
StructRNA **ShaderFilterGraphInputItemsAccessor::item_srna = &RNA_EeveeFilterGraphSocketItem;
StructRNA **ShaderFilterOutputItemsAccessor::item_srna = &RNA_EeveeFilterGraphSocketItem;
}  // namespace nodes

void register_node_type_sh_filter_graph_input()
{
  namespace file_ns = nodes::node_shader_filter_graph_input_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeFilterGraphInput", SH_NODE_FILTER_GRAPH_INPUT);
  ntype.ui_name = "Pass Input";
  ntype.ui_description = "Read image handles provided by the scene filter graph invocation";
  ntype.enum_name_legacy = "FILTER_GRAPH_INPUT";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_init;
  ntype.insert_link = file_ns::node_insert_link;
  ntype.draw_buttons_ex = file_ns::node_layout;
  ntype.gpu_fn = file_ns::node_shader_gpu;
  ntype.register_operators = file_ns::node_operators;
  ntype.add_ui_poll = filter_eevee_shader_nodes_poll;
  ntype.blend_write_storage_content = file_ns::node_blend_write;
  ntype.blend_data_read_storage_content = file_ns::node_blend_read;
  bke::node_type_storage(
      ntype, "NodeShaderFilterGraphInput", file_ns::node_free_storage, file_ns::node_copy_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
