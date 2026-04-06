/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "BKE_collection.hh"
#include "BKE_context.hh"

#include "DNA_collection_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "BLI_index_range.hh"
#include "BLI_set.hh"

#include "MEM_guardedalloc.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"

#include "WM_api.hh"

#include "NOD_socket_items.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"

namespace blender {

namespace nodes::node_shader_filter_object_mask_cc {

struct FilterMaskItemsAccessor : public socket_items::SocketItemsAccessorDefaults {
  using ItemT = NodeFilterMaskItem;
  static StructRNA **item_srna;
  static constexpr StringRefNull node_idname = "ShaderNodeFilterObjectMask";
  static constexpr bool has_type = false;
  static constexpr bool has_name = false;

  struct operator_idnames {
    static constexpr StringRefNull add_item = "NODE_OT_filter_mask_item_add";
    static constexpr StringRefNull remove_item = "NODE_OT_filter_mask_item_remove";
    static constexpr StringRefNull move_item = "NODE_OT_filter_mask_item_move";
  };

  struct ui_idnames {
    static constexpr StringRefNull list = "DATA_UL_filter_mask_items";
  };

  struct rna_names {
    static constexpr StringRefNull items = "object_items";
    static constexpr StringRefNull active_index = "active_index";
  };

  static socket_items::SocketItemsRef<NodeFilterMaskItem> get_items_from_node(bNode &node);
  static void copy_item(const NodeFilterMaskItem &src, NodeFilterMaskItem &dst)
  {
    dst.object = src.object;
  }
  static void destruct_item(NodeFilterMaskItem * /*item*/) {}
  static void blend_write_item(BlendWriter * /*writer*/, const ItemT & /*item*/) {}
  static void blend_read_data_item(BlendDataReader * /*reader*/, ItemT & /*item*/) {}
  static void init(bNode & /*node*/, NodeFilterMaskItem &item)
  {
    item.object = nullptr;
  }
};

static NodeFilterMask *storage(const bNode &node)
{
  return static_cast<NodeFilterMask *>(node.storage);
}

static NodeFilterMask &ensure_storage(bNode &node)
{
  if (node.storage == nullptr) {
    node.storage = MEM_new<NodeFilterMask>(__func__);
  }
  return *static_cast<NodeFilterMask *>(node.storage);
}

socket_items::SocketItemsRef<NodeFilterMaskItem> FilterMaskItemsAccessor::get_items_from_node(
    bNode &node)
{
  NodeFilterMask &storage = ensure_storage(node);
  return {&storage.items, &storage.items_num, &storage.active_index};
}

static Object *object_from_node_id(const bNode &node)
{
  return (node.id != nullptr && GS(node.id->name) == ID_OB) ? reinterpret_cast<Object *>(node.id) :
                                                              nullptr;
}

static Collection *collection_from_node_id(const bNode &node)
{
  return (node.id != nullptr && GS(node.id->name) == ID_GR) ?
             reinterpret_cast<Collection *>(node.id) :
             nullptr;
}

static const EnumPropertyItem filter_mask_mode_items[] = {
    {SHD_FILTER_MASK_SINGLE_OBJECT, "OBJECT", 0, "Single Object", "Mask a single object"},
    {SHD_FILTER_MASK_OBJECT_LIST,
     "OBJECT_LIST",
     0,
     "Object List",
     "Mask any object contained in the list"},
    {SHD_FILTER_MASK_COLLECTION,
     "COLLECTION",
     0,
     "Collection",
     "Mask any object contained in the selected collection"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Mask")
      .description("Mask pixels that belong to the selected object set using Eevee Cryptomatte data");
  b.add_output<decl::Color>("Color")
      .description("Grayscale preview of the mask for direct visualization");
}

static void draw_filter_mask_list_item(uiList * /*ui_list*/,
                                       const bContext * /*C*/,
                                       ui::Layout &layout,
                                       PointerRNA * /*idataptr*/,
                                       PointerRNA *itemptr,
                                       int /*icon*/,
                                       PointerRNA * /*active_dataptr*/,
                                       const char * /*active_propname*/,
                                       int /*index*/,
                                       int /*flt_flag*/)
{
  ui::Layout &row = layout.row(true);
  row.emboss_set(ui::EmbossType::None);
  row.prop(itemptr, "name", UI_ITEM_NONE, "", ICON_NONE);
}

static const uiListType *filter_mask_items_list_type()
{
  static const uiListType *list = []() {
    uiListType *new_list = MEM_new_zeroed<uiListType>(__func__);
    STRNCPY_UTF8(new_list->idname, FilterMaskItemsAccessor::ui_idnames::list.c_str());
    new_list->draw_item = draw_filter_mask_list_item;
    WM_uilisttype_add(new_list);
    return new_list;
  }();
  return list;
}

static void draw_filter_mask_items(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *ptr->data_as<bNode>();
  NodeFilterMask &node_storage = ensure_storage(node);

  ui::Layout &row = layout.row(false);
  ui::template_list(&row,
                    C,
                    filter_mask_items_list_type()->idname,
                    "",
                    ptr,
                    FilterMaskItemsAccessor::rna_names::items,
                    ptr,
                    FilterMaskItemsAccessor::rna_names::active_index.c_str(),
                    nullptr,
                    3,
                    5,
                    UILST_LAYOUT_DEFAULT,
                    ui::TEMPLATE_LIST_FLAG_NONE);

  ui::Layout &ops_col = row.column(false);
  {
    ui::Layout &add_remove_col = ops_col.column(true);
    PointerRNA op_ptr = add_remove_col.op(
        FilterMaskItemsAccessor::operator_idnames::add_item, "", ICON_ADD);
    RNA_int_set(&op_ptr, "node_identifier", node.identifier);
    op_ptr = add_remove_col.op(
        FilterMaskItemsAccessor::operator_idnames::remove_item, "", ICON_REMOVE);
    RNA_int_set(&op_ptr, "node_identifier", node.identifier);
  }
  {
    ui::Layout &up_down_col = ops_col.column(true);
    PointerRNA op_ptr = up_down_col.op(
        FilterMaskItemsAccessor::operator_idnames::move_item, "", ICON_TRIA_UP);
    RNA_int_set(&op_ptr, "node_identifier", node.identifier);
    RNA_enum_set(&op_ptr, "direction", 0);
    op_ptr = up_down_col.op(
        FilterMaskItemsAccessor::operator_idnames::move_item, "", ICON_TRIA_DOWN);
    RNA_int_set(&op_ptr, "node_identifier", node.identifier);
    RNA_enum_set(&op_ptr, "direction", 1);
  }

  ui::Layout &selection_row = layout.row(true);
  PointerRNA op_ptr = selection_row.op("NODE_OT_filter_mask_items_from_selection",
                                       IFACE_("Use Selection"),
                                       ICON_RESTRICT_SELECT_OFF);
  RNA_int_set(&op_ptr, "node_identifier", node.identifier);
  RNA_boolean_set(&op_ptr, "replace", true);
  op_ptr = selection_row.op(
      "NODE_OT_filter_mask_items_from_selection", IFACE_("Append Selection"), ICON_ADD);
  RNA_int_set(&op_ptr, "node_identifier", node.identifier);
  RNA_boolean_set(&op_ptr, "replace", false);

  if (node_storage.items_num > 0 && node_storage.active_index >= 0 &&
      node_storage.active_index < node_storage.items_num)
  {
    PointerRNA item_ptr = RNA_pointer_create_discrete(
        &tree.id, *FilterMaskItemsAccessor::item_srna, &node_storage.items[node_storage.active_index]);
    layout.use_property_split_set(true);
    layout.use_property_decorate_set(false);
    layout.prop(&item_ptr, "object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void node_shader_buts_filter_object_mask(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);

  bNode &node = *ptr->data_as<bNode>();
  const NodeFilterMaskMode mode = NodeFilterMaskMode(node.custom1);
  switch (mode) {
    case SHD_FILTER_MASK_SINGLE_OBJECT:
      layout.prop(ptr, "object", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
      break;
    case SHD_FILTER_MASK_OBJECT_LIST:
      {
        ui::Layout &box = layout.box();
        box.label(IFACE_("Objects"), ICON_NONE);
        draw_filter_mask_items(box, C, ptr);
      }
      break;
    case SHD_FILTER_MASK_COLLECTION:
      layout.prop(ptr, "collection", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
      break;
  }
}

static void append_mask_object(GPUMaterial *mat, Object *object, int &r_count)
{
  if (object == nullptr) {
    return;
  }
  if (GPU_material_filter_mask_object_append(mat, object) != -1) {
    r_count++;
  }
}

static int node_shader_gpu_filter_object_mask(GPUMaterial *mat,
                                              bNode *node,
                                              bNodeExecData * /*execdata*/,
                                              GPUNodeStack * /*in*/,
                                              GPUNodeStack *out)
{
  int range_count = 0;
  const float range_start = float(GPU_material_filter_mask_object_count(mat));
  const NodeFilterMaskMode mode = NodeFilterMaskMode(node->custom1);

  switch (mode) {
    case SHD_FILTER_MASK_SINGLE_OBJECT: {
      append_mask_object(mat, object_from_node_id(*node), range_count);
      break;
    }
    case SHD_FILTER_MASK_OBJECT_LIST: {
      if (NodeFilterMask *node_storage = storage(*node)) {
        Set<Object *> unique_objects;
        for (const int index : IndexRange(node_storage->items_num)) {
          Object *object = node_storage->items[index].object;
          if (object == nullptr || !unique_objects.add(object)) {
            continue;
          }
          append_mask_object(mat, object, range_count);
        }
      }
      break;
    }
    case SHD_FILTER_MASK_COLLECTION: {
      if (Collection *collection = collection_from_node_id(*node)) {
        Set<Object *> unique_objects;
        FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, object) {
          if (object == nullptr || !unique_objects.add(object)) {
            continue;
          }
          append_mask_object(mat, object, range_count);
        }
        FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
      }
      break;
    }
  }

  const float range_count_value = float(range_count);
  return GPU_stack_link(mat,
                        node,
                        "node_filter_object_mask",
                        nullptr,
                        out,
                        GPU_constant(&range_start),
                        GPU_constant(&range_count_value));
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = SHD_FILTER_MASK_SINGLE_OBJECT;
  node->storage = MEM_new<NodeFilterMask>(__func__);
}

static void node_free_storage(bNode *node)
{
  if (node->storage == nullptr) {
    return;
  }
  socket_items::destruct_array<FilterMaskItemsAccessor>(*node);
  MEM_delete(static_cast<NodeFilterMask *>(node->storage));
  node->storage = nullptr;
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  auto *dst_storage = MEM_new<NodeFilterMask>(__func__);
  dst_node->storage = dst_storage;

  if (const NodeFilterMask *src_storage = storage(*src_node)) {
    dst_storage->items_num = src_storage->items_num;
    dst_storage->active_index = src_storage->active_index;
    socket_items::copy_array<FilterMaskItemsAccessor>(*src_node, *dst_node);
  }
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  if (node.storage != nullptr) {
    socket_items::blend_write<FilterMaskItemsAccessor>(&writer, node);
  }
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  if (node.storage != nullptr) {
    socket_items::blend_read_data<FilterMaskItemsAccessor>(&reader, node);
  }
}

static wmOperatorStatus filter_mask_items_from_selection_exec(bContext *C, wmOperator *op)
{
  PointerRNA node_ptr = socket_items::ops::get_active_node_to_operate_on(
      C, op, FilterMaskItemsAccessor::node_idname);
  if (node_ptr.data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  bNode &node = *static_cast<bNode *>(node_ptr.data);
  if (node.custom1 != SHD_FILTER_MASK_OBJECT_LIST) {
    return OPERATOR_CANCELLED;
  }

  const bool replace = RNA_boolean_get(op->ptr, "replace");
  if (replace) {
    socket_items::clear<FilterMaskItemsAccessor>(node);
  }

  Set<Object *> existing_objects;
  socket_items::SocketItemsRef<NodeFilterMaskItem> items =
      FilterMaskItemsAccessor::get_items_from_node(node);
  for (const int index : IndexRange(*items.items_num)) {
    if ((*items.items)[index].object != nullptr) {
      existing_objects.add((*items.items)[index].object);
    }
  }

  CTX_DATA_BEGIN (C, Object *, object, selected_objects) {
    if (object == nullptr || existing_objects.contains(object)) {
      continue;
    }
    NodeFilterMaskItem *item = socket_items::add_item<FilterMaskItemsAccessor>(node);
    item->object = object;
    existing_objects.add(object);
  }
  CTX_DATA_END;

  socket_items::ops::update_after_node_change(C, node_ptr);
  return OPERATOR_FINISHED;
}

static void node_filter_mask_items_from_selection_operator(wmOperatorType *ot)
{
  ot->name = "Populate Filter Mask Objects";
  ot->idname = "NODE_OT_filter_mask_items_from_selection";
  ot->description = "Populate the object list from the current object selection";
  ot->poll = socket_items::ops::editable_node_active_poll<FilterMaskItemsAccessor>;
  ot->exec = filter_mask_items_from_selection_exec;
  ot->flag = OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "replace", true, "Replace", "Replace the current list instead of appending");
  socket_items::ops::add_node_identifier_property(ot);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<FilterMaskItemsAccessor>();
  WM_operatortype_append(node_filter_mask_items_from_selection_operator);
}

}  // namespace nodes::node_shader_filter_object_mask_cc

namespace nodes {
StructRNA **node_shader_filter_object_mask_cc::FilterMaskItemsAccessor::item_srna = &RNA_FilterMaskItem;
}  // namespace nodes

void register_node_type_sh_filter_object_mask()
{
  namespace file_ns = nodes::node_shader_filter_object_mask_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeFilterObjectMask", SH_NODE_FILTER_OBJECT_MASK);
  ntype.ui_name = "Filter Mask";
  ntype.ui_description =
      "Create fast Eevee filter masks from a single object, an object list, or a collection using Cryptomatte data";
  ntype.nclass = NODE_CLASS_INPUT;
  bke::node_type_storage(ntype, "NodeFilterMask", file_ns::node_free_storage, file_ns::node_copy_storage);
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_init;
  ntype.draw_buttons = file_ns::node_shader_buts_filter_object_mask;
  ntype.add_ui_poll = filter_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_filter_object_mask;
  ntype.register_operators = file_ns::node_operators;
  ntype.blend_write_storage_content = file_ns::node_blend_write;
  ntype.blend_data_read_storage_content = file_ns::node_blend_read;

  bke::node_register_type(ntype);
}

}  // namespace blender
