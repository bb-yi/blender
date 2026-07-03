/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include <algorithm>

#include "BLO_read_write.hh"

#include "DNA_array_utils.hh"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "BLI_string.h"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "NOD_filter_graph.hh"
#include "NOD_socket.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface_layout.hh"
#include "UI_interface_c.hh"
#include "UI_resources.hh"

#include "WM_api.hh"

#include "RNA_access.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender {
namespace nodes {

socket_items::SocketItemsRef<NodeEeveeFilterGraphSocketItem>
EeveeFilterGraphMaterialItemsAccessor::get_items_from_node(bNode &node)
{
  auto &storage = *static_cast<NodeEeveeFilterGraphFilterMaterial *>(node.storage);
  return {&storage.items, &storage.items_num, &storage.active_index};
}

static Material *filter_graph_filter_pass_material(const bNode &node)
{
  if (node.id == nullptr || GS(node.id->name) != ID_MA) {
    return nullptr;
  }
  return reinterpret_cast<Material *>(node.id);
}

static bNode *filter_graph_find_pass_input_node(Material &material)
{
  if (material.nodetree == nullptr) {
    return nullptr;
  }
  for (bNode *node = static_cast<bNode *>(material.nodetree->nodes.first); node != nullptr;
       node = node->next)
  {
    if (STREQ(node->idname, ShaderFilterGraphInputItemsAccessor::node_idname.c_str()) &&
        node->storage != nullptr)
    {
      return node;
    }
  }
  return nullptr;
}

static bNode *filter_graph_find_filter_output_node(Material &material)
{
  if (material.nodetree == nullptr) {
    return nullptr;
  }
  bNode *output = nullptr;
  for (bNode *node = static_cast<bNode *>(material.nodetree->nodes.first); node != nullptr;
       node = node->next)
  {
    if (node->type_legacy != SH_NODE_OUTPUT_FILTER) {
      continue;
    }
    if (output == nullptr) {
      output = node;
    }
    else if ((node->flag & NODE_DO_OUTPUT) && !(output->flag & NODE_DO_OUTPUT)) {
      output = node;
    }
  }
  return output;
}

static Material *filter_graph_material_from_pass_input_tree(Main &bmain, const bNodeTree &ntree)
{
  for (Material &material : bmain.materials) {
    if (material.nodetree == &ntree) {
      return &material;
    }
  }
  return nullptr;
}

static void filter_graph_tag_node_changed(Main &bmain, bNodeTree &ntree, bNode &node)
{
  BKE_ntree_update_tag_node_property(&ntree, &node);
  filter_graph_tag_tree_changed(bmain, ntree);
}

static void filter_graph_copy_socket_items(
    socket_items::SocketItemsRef<NodeEeveeFilterGraphSocketItem> src_ref,
    socket_items::SocketItemsRef<NodeEeveeFilterGraphSocketItem> dst_ref)
{
  for (const int i : IndexRange(*dst_ref.items_num)) {
    FilterGraphSocketItemsAccessorBase::destruct_item(&(*dst_ref.items)[i]);
  }
  MEM_SAFE_DELETE(*dst_ref.items);

  const int items_num = *src_ref.items_num;
  *dst_ref.items_num = items_num;
  *dst_ref.items = items_num > 0 ? MEM_new_array<NodeEeveeFilterGraphSocketItem>(items_num,
                                                                                  __func__) :
                                   nullptr;
  for (const int i : IndexRange(items_num)) {
    FilterGraphSocketItemsAccessorBase::copy_item((*src_ref.items)[i], (*dst_ref.items)[i]);
  }

  int active_index = src_ref.active_index ? *src_ref.active_index : 0;
  if (items_num == 0) {
    active_index = -1;
  }
  else if (active_index < 0) {
    active_index = 0;
  }
  else if (active_index >= items_num) {
    active_index = items_num - 1;
  }
  if (dst_ref.active_index != nullptr) {
    *dst_ref.active_index = active_index;
  }
}

static bool filter_graph_socket_items_match(
    socket_items::SocketItemsRef<NodeEeveeFilterGraphSocketItem> src_ref,
    socket_items::SocketItemsRef<NodeEeveeFilterGraphSocketItem> dst_ref)
{
  if (*src_ref.items_num != *dst_ref.items_num) {
    return false;
  }
  if (src_ref.active_index != nullptr && dst_ref.active_index != nullptr &&
      *src_ref.active_index != *dst_ref.active_index)
  {
    return false;
  }
  for (const int i : IndexRange(*src_ref.items_num)) {
    const NodeEeveeFilterGraphSocketItem &src = (*src_ref.items)[i];
    const NodeEeveeFilterGraphSocketItem &dst = (*dst_ref.items)[i];
    if (src.identifier != dst.identifier) {
      return false;
    }
    if (!STREQ(src.name ? src.name : "", dst.name ? dst.name : "")) {
      return false;
    }
  }
  return true;
}

static void filter_graph_copy_pass_input_to_filter_pass(const bNode &source_node,
                                                        bNode &target_node)
{
  const NodeShaderFilterGraphInput &source_storage =
      *static_cast<const NodeShaderFilterGraphInput *>(source_node.storage);
  NodeEeveeFilterGraphFilterMaterial &target_storage =
      *static_cast<NodeEeveeFilterGraphFilterMaterial *>(target_node.storage);
  filter_graph_copy_socket_items(
      ShaderFilterGraphInputItemsAccessor::get_items_from_node(const_cast<bNode &>(source_node)),
      EeveeFilterGraphMaterialItemsAccessor::get_items_from_node(target_node));
  target_storage.next_identifier = source_storage.next_identifier;
}

static bool filter_graph_pass_input_matches_filter_pass(const bNode &source_node,
                                                        const bNode &target_node)
{
  const NodeShaderFilterGraphInput &source_storage =
      *static_cast<const NodeShaderFilterGraphInput *>(source_node.storage);
  const NodeEeveeFilterGraphFilterMaterial &target_storage =
      *static_cast<const NodeEeveeFilterGraphFilterMaterial *>(target_node.storage);
  if (source_storage.next_identifier != target_storage.next_identifier) {
    return false;
  }
  return filter_graph_socket_items_match(
      ShaderFilterGraphInputItemsAccessor::get_items_from_node(const_cast<bNode &>(source_node)),
      EeveeFilterGraphMaterialItemsAccessor::get_items_from_node(
          const_cast<bNode &>(target_node)));
}

static void filter_graph_copy_filter_pass_to_pass_input(const bNode &source_node,
                                                        bNode &target_node)
{
  const NodeEeveeFilterGraphFilterMaterial &source_storage =
      *static_cast<const NodeEeveeFilterGraphFilterMaterial *>(source_node.storage);
  NodeShaderFilterGraphInput &target_storage =
      *static_cast<NodeShaderFilterGraphInput *>(target_node.storage);
  filter_graph_copy_socket_items(
      EeveeFilterGraphMaterialItemsAccessor::get_items_from_node(const_cast<bNode &>(source_node)),
      ShaderFilterGraphInputItemsAccessor::get_items_from_node(target_node));
  target_storage.next_identifier = source_storage.next_identifier;
}

static void filter_graph_clear_filter_pass_interface(bNode &node)
{
  if (node.storage == nullptr) {
    return;
  }
  NodeEeveeFilterGraphFilterMaterial &storage =
      *static_cast<NodeEeveeFilterGraphFilterMaterial *>(node.storage);
  socket_items::SocketItemsRef<NodeEeveeFilterGraphSocketItem> ref =
      EeveeFilterGraphMaterialItemsAccessor::get_items_from_node(node);
  for (const int i : IndexRange(*ref.items_num)) {
    FilterGraphSocketItemsAccessorBase::destruct_item(&(*ref.items)[i]);
  }
  MEM_SAFE_DELETE(*ref.items);
  storage.items_num = 0;
  storage.active_index = -1;
  storage.next_identifier = 0;
}

static bool filter_graph_filter_pass_interface_is_empty(const bNode &node)
{
  if (node.storage == nullptr) {
    return true;
  }
  const NodeEeveeFilterGraphFilterMaterial &storage =
      *static_cast<const NodeEeveeFilterGraphFilterMaterial *>(node.storage);
  return storage.items_num == 0 && storage.active_index == -1 && storage.next_identifier == 0;
}

static bool filter_graph_clear_filter_pass_interface_if_needed(bNode &node)
{
  if (filter_graph_filter_pass_interface_is_empty(node)) {
    return false;
  }
  filter_graph_clear_filter_pass_interface(node);
  return true;
}

static bool filter_graph_sync_material_to_all_filter_passes(Main &bmain,
                                                            Material &material,
                                                            const bNodeTree *skip_tree = nullptr,
                                                            const bNode *skip_node = nullptr)
{
  bNode *pass_input_node = filter_graph_find_pass_input_node(material);
  const bool has_pass_input = pass_input_node != nullptr;

  for (Scene &scene : bmain.scenes) {
    bNodeTree *filter_graph = scene.eevee.filter_graph;
    if (filter_graph == nullptr ||
        !STREQ(filter_graph->idname, eevee_filter_graph_tree_idname.c_str()))
    {
      continue;
    }
    for (bNode *node = static_cast<bNode *>(filter_graph->nodes.first); node != nullptr;
         node = node->next)
    {
      if (filter_graph == skip_tree && node == skip_node) {
        continue;
      }
      if (node->type_legacy != EEVEE_FILTER_GRAPH_NODE_FILTER_MATERIAL ||
          node->storage == nullptr || node->id != &material.id)
      {
        continue;
      }
      if (has_pass_input) {
        filter_graph_copy_pass_input_to_filter_pass(*pass_input_node, *node);
      }
      else {
        filter_graph_clear_filter_pass_interface(*node);
      }
      BKE_ntree_update_tag_node_property(filter_graph, node);
      BKE_main_ensure_invariants(bmain, filter_graph->id);
      update_node_declaration_and_sockets(*filter_graph, *node);
      filter_graph_tag_node_changed(bmain, *filter_graph, *node);
    }
  }

  return has_pass_input;
}

bool filter_graph_filter_pass_has_pass_input(const bNode &node)
{
  Material *material = filter_graph_filter_pass_material(node);
  return material != nullptr && filter_graph_find_pass_input_node(*material) != nullptr;
}

void filter_graph_pass_input_interface_changed(Main &bmain, bNodeTree &ntree, bNode &node)
{
  filter_graph_tag_node_changed(bmain, ntree, node);
  Material *material = filter_graph_material_from_pass_input_tree(bmain, ntree);
  if (material == nullptr) {
    return;
  }
  filter_graph_sync_material_to_all_filter_passes(bmain, *material);
}

void filter_graph_filter_output_interface_changed(Main &bmain, bNodeTree &ntree, bNode &node)
{
  filter_graph_tag_node_changed(bmain, ntree, node);
  Material *material = filter_graph_material_from_pass_input_tree(bmain, ntree);
  if (material == nullptr) {
    return;
  }
  filter_graph_sync_material_to_all_filter_passes(bmain, *material);
}

void filter_graph_filter_pass_interface_changed(Main &bmain, bNodeTree &ntree, bNode &node)
{
  Material *material = filter_graph_filter_pass_material(node);
  bNode *pass_input_node = material ? filter_graph_find_pass_input_node(*material) : nullptr;
  if (pass_input_node == nullptr) {
    update_node_declaration_and_sockets(ntree, node);
    filter_graph_tag_node_changed(bmain, ntree, node);
    return;
  }

  filter_graph_copy_filter_pass_to_pass_input(node, *pass_input_node);
  update_node_declaration_and_sockets(ntree, node);
  update_node_declaration_and_sockets(*material->nodetree, *pass_input_node);
  filter_graph_tag_node_changed(bmain, ntree, node);
  filter_graph_tag_node_changed(bmain, *material->nodetree, *pass_input_node);
  filter_graph_sync_material_to_all_filter_passes(bmain, *material, &ntree, &node);
}

void filter_graph_filter_pass_material_changed(Main &bmain, bNodeTree &ntree, bNode &node)
{
  filter_graph_sync_material_interface_to_filter_pass(bmain, ntree, node);
}

bool filter_graph_sync_material_interface_to_filter_pass(Main &bmain, bNodeTree &ntree, bNode &node)
{
  Material *material = filter_graph_filter_pass_material(node);
  if (material == nullptr) {
    filter_graph_clear_filter_pass_interface(node);
    filter_graph_tag_node_changed(bmain, ntree, node);
    return false;
  }
  bNode *pass_input_node = filter_graph_find_pass_input_node(*material);
  if (pass_input_node == nullptr) {
    filter_graph_clear_filter_pass_interface(node);
    filter_graph_tag_node_changed(bmain, ntree, node);
    filter_graph_sync_material_to_all_filter_passes(bmain, *material);
    return false;
  }
  filter_graph_copy_pass_input_to_filter_pass(*pass_input_node, node);
  filter_graph_tag_node_changed(bmain, ntree, node);
  filter_graph_sync_material_to_all_filter_passes(bmain, *material);
  return true;
}

bool filter_graph_sync_filter_pass_interface_from_material_storage(bNodeTree &ntree, bNode &node)
{
  if (node.type_legacy != EEVEE_FILTER_GRAPH_NODE_FILTER_MATERIAL || node.storage == nullptr) {
    return false;
  }

  Material *material = filter_graph_filter_pass_material(node);
  bNode *pass_input_node = material ? filter_graph_find_pass_input_node(*material) : nullptr;
  bool changed = false;
  if (pass_input_node == nullptr) {
    changed = filter_graph_clear_filter_pass_interface_if_needed(node);
  }
  else if (!filter_graph_pass_input_matches_filter_pass(*pass_input_node, node)) {
    filter_graph_copy_pass_input_to_filter_pass(*pass_input_node, node);
    changed = true;
  }

  if (changed) {
    update_node_declaration_and_sockets(ntree, node);
  }
  return changed;
}

void filter_graph_sync_filter_pass_interfaces_from_materials(bNodeTree &ntree)
{
  for (bNode *node = static_cast<bNode *>(ntree.nodes.first); node != nullptr; node = node->next) {
    filter_graph_sync_filter_pass_interface_from_material_storage(ntree, *node);
  }
}

void ShaderFilterGraphInputItemsAccessor::post_item_change(bContext *C, PointerRNA node_ptr)
{
  Main *bmain = CTX_data_main(C);
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(node_ptr.owner_id);
  bNode &node = *static_cast<bNode *>(node_ptr.data);
  post_item_change(*bmain, ntree, node);
}

void ShaderFilterOutputItemsAccessor::post_item_change(bContext *C, PointerRNA node_ptr)
{
  Main *bmain = CTX_data_main(C);
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(node_ptr.owner_id);
  bNode &node = *static_cast<bNode *>(node_ptr.data);
  post_item_change(*bmain, ntree, node);
}

void ShaderFilterGraphInputItemsAccessor::post_item_change(Main &bmain,
                                                           bNodeTree &ntree,
                                                           bNode &node)
{
  filter_graph_pass_input_interface_changed(bmain, ntree, node);
}

void ShaderFilterOutputItemsAccessor::post_item_change(Main &bmain, bNodeTree &ntree, bNode &node)
{
  ensure_shader_filter_output_storage(node);
  update_node_declaration_and_sockets(ntree, node);
  filter_graph_filter_output_interface_changed(bmain, ntree, node);
}

void EeveeFilterGraphMaterialItemsAccessor::post_item_change(bContext *C, PointerRNA node_ptr)
{
  Main *bmain = CTX_data_main(C);
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(node_ptr.owner_id);
  bNode &node = *static_cast<bNode *>(node_ptr.data);
  post_item_change(*bmain, ntree, node);
}

void EeveeFilterGraphMaterialItemsAccessor::post_item_change(Main &bmain,
                                                             bNodeTree &ntree,
                                                             bNode &node)
{
  filter_graph_filter_pass_interface_changed(bmain, ntree, node);
}

}  // namespace nodes

namespace nodes::node_filter_graph_nodes_cc {

static bool graph_node_poll(const bke::bNodeType * /*ntype*/,
                            const bNodeTree *ntree,
                            const char **r_disabled_hint)
{
  if (!STREQ(ntree->idname, eevee_filter_graph_tree_idname.c_str())) {
    *r_disabled_hint = RPT_("Not an Eevee filter graph node tree");
    return false;
  }
  return true;
}

static void graph_node_type_base(bke::bNodeType &ntype,
                                 StringRefNull idname,
                                 const int16_t legacy_type)
{
  bke::node_type_base(ntype, idname, legacy_type);
  ntype.poll = graph_node_poll;
  ntype.insert_link = node_insert_link_default;
  ntype.gather_link_search_ops = nodes::search_link_ops_for_basic_node;
}

namespace scene_color {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Image>("Color Image");
  b.add_output<decl::Image>("Depth Image");
  b.add_output<decl::Image>("Normal Image");
  b.add_output<decl::Image>("Position Image");
}

}  // namespace scene_color

namespace aov_input {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Image>("Color");
  b.add_output<decl::Image>("Value");
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->storage = MEM_new<NodeEeveeFilterGraphAOVInput>(__func__);
}

static void draw_buttons(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "aov_name", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

}  // namespace aov_input

namespace filter_material {

NODE_STORAGE_FUNCS(NodeEeveeFilterGraphFilterMaterial);

static void draw_buttons(ui::Layout &layout, bContext *C, PointerRNA *ptr);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_layout(draw_buttons);

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  if (node && tree && node->storage != nullptr && filter_graph_filter_pass_has_pass_input(*node)) {
    const NodeEeveeFilterGraphFilterMaterial &storage = node_storage(*node);
    for (const int i : IndexRange(storage.items_num)) {
      const NodeEeveeFilterGraphSocketItem &item = storage.items[i];
      const StringRefNull name = item.name ? item.name : "";
      const std::string identifier =
          EeveeFilterGraphMaterialItemsAccessor::socket_identifier_for_item(item);
      b.add_input<decl::Image>(name, identifier)
          .hide_value()
          .socket_name_ptr(
              &tree->id, *EeveeFilterGraphMaterialItemsAccessor::item_srna, &item, "name")
          .structure_type(StructureType::Dynamic);
    }
    b.add_input<decl::Extend>("", "__extend__").structure_type(StructureType::Dynamic);
  }

  bool declared_output = false;
  if (node != nullptr) {
    Material *material = filter_graph_filter_pass_material(*node);
    bNode *filter_output_node = material ? filter_graph_find_filter_output_node(*material) : nullptr;
    if (filter_output_node != nullptr && filter_output_node->storage != nullptr) {
      const NodeShaderFilterOutput &output_storage =
          *static_cast<const NodeShaderFilterOutput *>(filter_output_node->storage);
      for (const int i : IndexRange(output_storage.items_num)) {
        const NodeEeveeFilterGraphSocketItem &item = output_storage.items[i];
        const StringRefNull name = item.name ? item.name : "";
        const std::string identifier = item.identifier == 0 ?
                                           "Image" :
                                           "Image_" + std::to_string(item.identifier);
        b.add_output<decl::Image>(name, identifier).structure_type(StructureType::Dynamic);
        declared_output = true;
      }
    }
  }
  if (!declared_output) {
    b.add_output<decl::Image>("Image", "Image");
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeEeveeFilterGraphFilterMaterial *data =
      MEM_new<NodeEeveeFilterGraphFilterMaterial>(__func__);
  data->items = nullptr;
  data->items_num = 0;
  data->active_index = -1;
  data->next_identifier = 0;
  data->resolution_scale = 1.0f;
  node->storage = data;
  node->flag |= NODE_OPTIONS;
  node->width = 240.0f;
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<EeveeFilterGraphMaterialItemsAccessor>(*node);
  MEM_delete(static_cast<NodeEeveeFilterGraphFilterMaterial *>(node->storage));
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeEeveeFilterGraphFilterMaterial &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_new<NodeEeveeFilterGraphFilterMaterial>(
      __func__, dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;
  socket_items::copy_array<EeveeFilterGraphMaterialItemsAccessor>(*src_node, *dst_node);
  dst_node->flag |= NODE_OPTIONS;
}

static void node_update(bNodeTree * /*tree*/, bNode *node)
{
  node->flag |= NODE_OPTIONS;
  node->width = std::max(node->width, 240.0f);
}

static void draw_buttons(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  layout.use_property_split_set(false);
  layout.use_property_decorate_set(false);
  ui::Layout &material_row = layout.row(true);
  template_id(&material_row, C, ptr, "material", "node.filter_pass_new_material", nullptr, nullptr);
  layout.prop(ptr, "execution_resolution", ui::ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Res"), ICON_NONE);
}

static void draw_buttons_ex(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  layout.use_property_split_set(false);
  layout.use_property_decorate_set(false);
  ui::Layout &material_row = layout.row(true);
  template_id(&material_row, C, ptr, "material", "node.filter_pass_new_material", nullptr, nullptr);
  layout.prop(ptr, "execution_resolution", ui::ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Resolution"), ICON_NONE);

  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *ptr->data_as<bNode>();
  if (filter_graph_filter_pass_has_pass_input(node)) {
    if (ui::Layout *panel = layout.panel(C, "filter_graph_inputs", false, IFACE_("Inputs"))) {
      socket_items::ui::draw_items_list_with_operators<EeveeFilterGraphMaterialItemsAccessor>(
          C, panel, ntree, node);
    }
  }
}

static void node_label(const bNodeTree * /*tree*/,
                       const bNode *node,
                       char *label,
                       int label_maxncpy)
{
  if (node->id != nullptr && GS(node->id->name) == ID_MA) {
    BLI_strncpy(label, node->id->name + 2, label_maxncpy);
    return;
  }
  BLI_strncpy(label, IFACE_("No Filter Material"), label_maxncpy);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  NodeEeveeFilterGraphSocketItem *new_item = nullptr;
  if (!filter_graph_filter_pass_has_pass_input(params.node)) {
    const bool touches_virtual_socket =
        (params.link.fromnode == &params.node &&
         STREQ(params.link.fromsock->idname, "NodeSocketVirtual")) ||
        (params.link.tonode == &params.node &&
         STREQ(params.link.tosock->idname, "NodeSocketVirtual"));
    if (touches_virtual_socket) {
      return false;
    }
  }
  const bool keep_link =
      socket_items::try_add_item_via_any_extend_socket<EeveeFilterGraphMaterialItemsAccessor>(
          params.ntree, params.node, params.node, params.link, std::nullopt, &new_item);
  Main *bmain = params.C ? CTX_data_main(params.C) : params.bmain;
  if (keep_link && new_item != nullptr && bmain != nullptr) {
    EeveeFilterGraphMaterialItemsAccessor::post_item_change(*bmain, params.ntree, params.node);
  }
  return keep_link;
}

static void node_operators()
{
  socket_items::ops::make_common_operators<EeveeFilterGraphMaterialItemsAccessor>();
  WM_operatortype_append([](wmOperatorType *ot) {
    ot->name = "Sync Pass Input Interface";
    ot->idname = EeveeFilterGraphMaterialItemsAccessor::operator_idnames::sync_interface.c_str();
    ot->description = "Copy the material Pass Input interface to this graph node";
    ot->poll = socket_items::ops::editable_node_active_poll<EeveeFilterGraphMaterialItemsAccessor>;
    ot->flag = OPTYPE_UNDO;

    ot->exec = [](bContext *C, wmOperator *op) -> wmOperatorStatus {
      PointerRNA node_ptr = socket_items::ops::get_active_node_to_operate_on(
          C, op, EeveeFilterGraphMaterialItemsAccessor::node_idname);
      if (node_ptr.data == nullptr) {
        return OPERATOR_CANCELLED;
      }

      bNode &node = *static_cast<bNode *>(node_ptr.data);
      bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(node_ptr.owner_id);
      Main *bmain = CTX_data_main(C);
      Material *material = filter_graph_filter_pass_material(node);
      if (material == nullptr) {
        filter_graph_clear_filter_pass_interface(node);
        filter_graph_tag_node_changed(*bmain, ntree, node);
        BKE_report(op->reports, RPT_ERROR, "Filter Material node has no material");
        return OPERATOR_CANCELLED;
      }
      if (!filter_graph_sync_material_interface_to_filter_pass(*bmain, ntree, node)) {
        BKE_report(op->reports, RPT_ERROR, "Filter Material has no Pass Input node");
        return OPERATOR_CANCELLED;
      }
      return OPERATOR_FINISHED;
    };

    socket_items::ops::add_node_identifier_property(ot);
  });
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<EeveeFilterGraphMaterialItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<EeveeFilterGraphMaterialItemsAccessor>(&reader, node);
  node.flag |= NODE_OPTIONS;
  node.width = std::max(node.width, 220.0f);
}

}  // namespace filter_material

namespace stage_output {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Image>("Image").hide_value();
}

static void node_init(bNodeTree *tree, bNode *node)
{
  node->custom1 = SCE_EEVEE_FILTER_STAGE_BEFORE_POSTFX;
  if (tree != nullptr) {
    filter_graph_stage_output_activate(*tree, *node);
  }
  else {
    node->flag |= NODE_DO_OUTPUT;
  }
}

static void draw_buttons(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "execution_stage", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

}  // namespace stage_output

}  // namespace nodes::node_filter_graph_nodes_cc

namespace nodes {
StructRNA **EeveeFilterGraphMaterialItemsAccessor::item_srna =
    &RNA_EeveeFilterGraphSocketItem;
}  // namespace nodes

void register_node_type_eevee_filter_graph_scene_color()
{
  namespace file_ns = nodes::node_filter_graph_nodes_cc;
  static bke::bNodeType ntype;
  file_ns::graph_node_type_base(
      ntype, "EeveeFilterGraphNodeSceneColor", EEVEE_FILTER_GRAPH_NODE_SCENE_COLOR);
  ntype.ui_name = "Scene Color";
  ntype.ui_description = "Current stage input image handles";
  ntype.enum_name_legacy = "SCENE_COLOR";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::scene_color::node_declare;
  bke::node_register_type(ntype);
}

void register_node_type_eevee_filter_graph_aov_input()
{
  namespace file_ns = nodes::node_filter_graph_nodes_cc;
  static bke::bNodeType ntype;
  file_ns::graph_node_type_base(
      ntype, "EeveeFilterGraphNodeAOVInput", EEVEE_FILTER_GRAPH_NODE_AOV_INPUT);
  ntype.ui_name = "AOV Input";
  ntype.ui_description = "Read a view-layer AOV as image handles";
  ntype.enum_name_legacy = "AOV_INPUT";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::aov_input::node_declare;
  ntype.initfunc = file_ns::aov_input::node_init;
  ntype.draw_buttons = file_ns::aov_input::draw_buttons;
  bke::node_type_storage(
      ntype, "NodeEeveeFilterGraphAOVInput", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);
}

void register_node_type_eevee_filter_graph_filter_material()
{
  namespace file_ns = nodes::node_filter_graph_nodes_cc;
  static bke::bNodeType ntype;
  file_ns::graph_node_type_base(
      ntype, "EeveeFilterGraphNodeFilterMaterial", EEVEE_FILTER_GRAPH_NODE_FILTER_MATERIAL);
  ntype.ui_name = "Filter Pass";
  ntype.ui_description = "Invoke an Eevee filter material with image-handle inputs";
  ntype.enum_name_legacy = "FILTER_MATERIAL";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::filter_material::node_declare;
  ntype.initfunc = file_ns::filter_material::node_init;
  ntype.updatefunc = file_ns::filter_material::node_update;
  ntype.insert_link = file_ns::filter_material::node_insert_link;
  ntype.labelfunc = file_ns::filter_material::node_label;
  ntype.draw_buttons = file_ns::filter_material::draw_buttons;
  ntype.draw_buttons_ex = file_ns::filter_material::draw_buttons_ex;
  ntype.register_operators = file_ns::filter_material::node_operators;
  ntype.blend_write_storage_content = file_ns::filter_material::node_blend_write;
  ntype.blend_data_read_storage_content = file_ns::filter_material::node_blend_read;
  bke::node_type_storage(ntype,
                         "NodeEeveeFilterGraphFilterMaterial",
                         file_ns::filter_material::node_free_storage,
                         file_ns::filter_material::node_copy_storage);
  bke::node_register_type(ntype);
}

void register_node_type_eevee_filter_graph_stage_output()
{
  namespace file_ns = nodes::node_filter_graph_nodes_cc;
  static bke::bNodeType ntype;
  file_ns::graph_node_type_base(
      ntype, "EeveeFilterGraphNodeStageOutput", EEVEE_FILTER_GRAPH_NODE_STAGE_OUTPUT);
  ntype.ui_name = "Stage Output";
  ntype.ui_description = "Output the filter graph result for a render stage";
  ntype.enum_name_legacy = "STAGE_OUTPUT";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::stage_output::node_declare;
  ntype.initfunc = file_ns::stage_output::node_init;
  ntype.draw_buttons = file_ns::stage_output::draw_buttons;
  ntype.no_muting = true;
  bke::node_register_type(ntype);
}

}  // namespace blender
