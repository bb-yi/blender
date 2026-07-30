/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstring>

#include "node_geometry_util.hh"

#include "BLI_string_utf8.h"

#include "BKE_idprop.hh"

#include "NOD_geo_closure.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"
#include "NOD_sync_sockets.hh"

#include "BLO_read_write.hh"
#include "shader/node_shader_util.hh"

namespace blender {

namespace nodes::node_geo_closure_cc {

static bool gpu_link_zero_value(GPUMaterial *mat, const GPUType type, GPUNodeLink **r_link)
{
  const char *function_name = type == GPU_FLOAT ? "set_value_zero" :
                              ELEM(type, GPU_VEC2, GPU_VEC3) ? "set_rgb_zero" :
                              type == GPU_VEC4 ? "set_rgba_zero" :
                                                 nullptr;
  return function_name != nullptr && GPU_link(mat, function_name, r_link);
}

static const bNodeSocket *find_socket_by_identifier(const bNode &node,
                                                    const eNodeSocketInOut in_out,
                                                    const StringRef identifier)
{
  const ListBaseT<bNodeSocket> &sockets = in_out == SOCK_IN ? node.inputs : node.outputs;
  for (const bNodeSocket &socket : sockets) {
    if (socket.identifier == identifier) {
      return &socket;
    }
  }
  return nullptr;
}

static int normalized_vector_dimensions(const bNodeSocketValueVector &value)
{
  return ELEM(value.dimensions, 2, 3, 4) ? value.dimensions : 3;
}

static void configure_socket_declaration(BaseSocketDeclarationBuilder &decl,
                                         const bNodeSocket *socket,
                                         const eNodeSocketInOut in_out,
                                         const StringRef identifier,
                                         const StringRef key)
{
  if (socket) {
    decl.description(socket->description).hide_value(socket->flag & SOCK_HIDE_VALUE);
  }
  decl.label_fn([in_out, identifier = std::string(identifier), key = std::string(key)](
                    const bNode &node) -> StringRefNull {
    const bNodeSocket *socket = find_socket_by_identifier(node, in_out, identifier);
    return socket && socket->label[0] != '\0' ? StringRefNull(socket->label) : StringRefNull(key);
  });
}

static void configure_socket_declaration(decl::FloatBuilder &decl, const bNodeSocket *socket)
{
  if (!socket || socket->type != SOCK_FLOAT || !socket->default_value) {
    return;
  }
  const auto &value = *socket->default_value_typed<bNodeSocketValueFloat>();
  decl.default_value(value.value)
      .min(value.min)
      .max(value.max)
      .subtype(PropertySubType(value.subtype));
}

static void configure_socket_declaration(decl::IntBuilder &decl, const bNodeSocket *socket)
{
  if (!socket || socket->type != SOCK_INT || !socket->default_value) {
    return;
  }
  const auto &value = *socket->default_value_typed<bNodeSocketValueInt>();
  decl.default_value(value.value)
      .min(value.min)
      .max(value.max)
      .subtype(PropertySubType(value.subtype));
}

static void configure_socket_declaration(decl::BoolBuilder &decl, const bNodeSocket *socket)
{
  if (!socket || socket->type != SOCK_BOOLEAN || !socket->default_value) {
    return;
  }
  const auto &value = *socket->default_value_typed<bNodeSocketValueBoolean>();
  decl.default_value(bool(value.value));
}

static void configure_socket_declaration(decl::VectorBuilder &decl, const bNodeSocket *socket)
{
  if (!socket || socket->type != SOCK_VECTOR || !socket->default_value) {
    return;
  }
  const auto &value = *socket->default_value_typed<bNodeSocketValueVector>();
  decl.default_value(float4(value.value[0], value.value[1], value.value[2], value.value[3]))
      .dimensions(normalized_vector_dimensions(value))
      .min(value.min)
      .max(value.max)
      .subtype(PropertySubType(value.subtype));
}

static void configure_socket_declaration(decl::ColorBuilder &decl, const bNodeSocket *socket)
{
  if (!socket || socket->type != SOCK_RGBA || !socket->default_value) {
    return;
  }
  const auto &value = *socket->default_value_typed<bNodeSocketValueRGBA>();
  decl.default_value(
      ColorGeometry4f(value.value[0], value.value[1], value.value[2], value.value[3]));
}

static BaseSocketDeclarationBuilder &add_closure_input_declaration(
    DeclarationListBuilder &b,
    const eNodeSocketDatatype socket_type,
    const UString name,
    const UString identifier,
    const bNodeSocket *socket)
{
  switch (socket_type) {
    case SOCK_FLOAT: {
      auto &decl = b.add_input<decl::Float>(name, identifier);
      configure_socket_declaration(decl, socket);
      return decl;
    }
    case SOCK_INT: {
      auto &decl = b.add_input<decl::Int>(name, identifier);
      configure_socket_declaration(decl, socket);
      return decl;
    }
    case SOCK_BOOLEAN: {
      auto &decl = b.add_input<decl::Bool>(name, identifier);
      configure_socket_declaration(decl, socket);
      return decl;
    }
    case SOCK_VECTOR: {
      auto &decl = b.add_input<decl::Vector>(name, identifier);
      configure_socket_declaration(decl, socket);
      return decl;
    }
    case SOCK_RGBA: {
      auto &decl = b.add_input<decl::Color>(name, identifier);
      configure_socket_declaration(decl, socket);
      return decl;
    }
    default:
      return b.add_input(socket_type, name, identifier);
  }
}

static BaseSocketDeclarationBuilder &add_closure_output_declaration(
    DeclarationListBuilder &b,
    const eNodeSocketDatatype socket_type,
    const UString name,
    const UString identifier,
    const bNodeSocket *socket)
{
  switch (socket_type) {
    case SOCK_FLOAT: {
      auto &decl = b.add_output<decl::Float>(name, identifier);
      configure_socket_declaration(decl, socket);
      return decl;
    }
    case SOCK_INT: {
      auto &decl = b.add_output<decl::Int>(name, identifier);
      configure_socket_declaration(decl, socket);
      return decl;
    }
    case SOCK_BOOLEAN: {
      auto &decl = b.add_output<decl::Bool>(name, identifier);
      configure_socket_declaration(decl, socket);
      return decl;
    }
    case SOCK_VECTOR: {
      auto &decl = b.add_output<decl::Vector>(name, identifier);
      configure_socket_declaration(decl, socket);
      return decl;
    }
    case SOCK_RGBA: {
      auto &decl = b.add_output<decl::Color>(name, identifier);
      configure_socket_declaration(decl, socket);
      return decl;
    }
    default:
      return b.add_output(socket_type, name, identifier);
  }
}

BaseSocketDeclarationBuilder &add_closure_socket_declaration(
    DeclarationListBuilder &builder,
    const eNodeSocketDatatype socket_type,
    const eNodeSocketInOut in_out,
    UString name,
    UString identifier,
    const bNodeSocket *socket,
    const StringRef key)
{
  BaseSocketDeclarationBuilder &decl = in_out == SOCK_IN ?
                                            add_closure_input_declaration(
                                                builder, socket_type, name, identifier, socket) :
                                            add_closure_output_declaration(
                                                builder, socket_type, name, identifier, socket);
  configure_socket_declaration(decl, socket, in_out, identifier.ref(), key);
  return decl;
}

/** Shared between closure input and output node. */
static void node_layout_ex(ui::Layout &layout, bContext *C, PointerRNA *current_node_ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(current_node_ptr->owner_id);
  bNode *current_node = static_cast<bNode *>(current_node_ptr->data);

  const bke::bNodeTreeZones *zones = ntree.zones();
  if (!zones) {
    return;
  }
  const bke::bNodeTreeZone *zone = zones->get_zone_by_node(current_node->identifier);
  if (!zone) {
    return;
  }
  if (!zone->output_node_id) {
    return;
  }
  bNode &output_node = const_cast<bNode &>(*zone->output_node());

  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  PointerRNA output_node_ptr = RNA_pointer_create_discrete(&ntree.id, RNA_Node, &output_node);

  layout.op("node.sockets_sync", IFACE_("Sync"), ICON_FILE_REFRESH);
  layout.prop(&output_node_ptr, "define_signature", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (current_node->type_legacy == NODE_CLOSURE_INPUT) {
    if (ui::Layout *panel = layout.panel(C, "input_items", false, IFACE_("Input Items"))) {
      socket_items::ui::draw_items_list_with_operators<ClosureInputItemsAccessor>(
          C, panel, ntree, output_node);
      socket_items::ui::draw_active_item_props<ClosureInputItemsAccessor>(
          ntree, output_node, [&](PointerRNA *item_ptr) {
            panel->use_property_split_set(true);
            panel->use_property_decorate_set(false);
            panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
            panel->prop(item_ptr, "structure_type", UI_ITEM_NONE, IFACE_("Shape"), ICON_NONE);
          });
    }
  }
  else {
    if (ui::Layout *panel = layout.panel(C, "output_items", false, IFACE_("Output Items"))) {
      socket_items::ui::draw_items_list_with_operators<ClosureOutputItemsAccessor>(
          C, panel, ntree, output_node);
      socket_items::ui::draw_active_item_props<ClosureOutputItemsAccessor>(
          ntree, output_node, [&](PointerRNA *item_ptr) {
            panel->use_property_split_set(true);
            panel->use_property_decorate_set(false);
            panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
            panel->prop(item_ptr, "structure_type", UI_ITEM_NONE, IFACE_("Shape"), ICON_NONE);
          });
    }
  }
}

namespace input_node {

NODE_STORAGE_FUNCS(NodeClosureInput);

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  if (node && tree) {
    const NodeClosureInput &storage = node_storage(*node);
    const bNode *output_node = tree->node_by_id(storage.output_node_id);
    if (output_node) {
      const auto &output_storage = *static_cast<const NodeClosureOutput *>(output_node->storage);
      for (const int i : IndexRange(output_storage.input_items.items_num)) {
        const NodeClosureInputItem &item = output_storage.input_items.items[i];
        const eNodeSocketDatatype socket_type = item.socket_type;
        const UString identifier(ClosureInputItemsAccessor::socket_identifier_for_item(item));
        const bNodeSocket *socket = find_socket_by_identifier(*node, SOCK_OUT, identifier.ref());
        auto &decl = add_closure_socket_declaration(
            b, socket_type, SOCK_OUT, UString(item.name), identifier, socket, item.name);
        decl.socket_name_ptr(&tree->id, *ClosureInputItemsAccessor::item_srna, &item, "name");
        if (item.structure_type != NodeSocketInterfaceStructureType::Auto) {
          decl.structure_type(StructureType(item.structure_type));
        }
        else {
          decl.structure_type(StructureType::Dynamic);
        }
      }
    }
  }
  b.add_output<decl::Extend>(""_ustr, "__extend__"_ustr)
      .custom_draw(socket_items::ui::draw_extend_socket_fn<ClosureInputItemsAccessor>());
}

static void node_label(const bNodeTree * /*ntree*/,
                       const bNode * /*node*/,
                       char *label,
                       const int label_maxncpy)
{
  BLI_strncpy_utf8(label, CTX_IFACE_(BLT_I18NCONTEXT_ID_NODETREE, "Closure"), label_maxncpy);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeClosureInput *data = MEM_new<NodeClosureInput>(__func__);
  node->storage = data;
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  bNode *output_node = params.ntree.node_by_id(node_storage(params.node).output_node_id);
  if (!output_node) {
    return true;
  }
  return socket_items::try_add_item_via_any_extend_socket<ClosureInputItemsAccessor>(
      params.ntree, params.node, *output_node, params.link);
}

static int gpu_shader_closure_input(GPUMaterial *mat,
                                    bNode *node,
                                    bNodeExecData * /*execdata*/,
                                    GPUNodeStack * /*in*/,
                                    GPUNodeStack *out)
{
  const int closure_output_node_id = node_storage(*node).output_node_id;
  struct CallbackBinding {
    bool found = false;
    int function_input_index = -1;
    GPUType transport_type = GPU_NONE;
  };
  Vector<CallbackBinding> callback_bindings;
  bool found_callback_input = false;
  int output_index = 0;
  for (const bNodeSocket *socket : node->output_sockets()) {
    if (out[output_index].end) {
      break;
    }

    GPUType callback_type = GPU_NONE;
    int function_input_index = -1;
    bool is_ancestor_capture = false;
    if (GPU_material_closure_callback_input_find(
            mat,
            closure_output_node_id,
            socket->name,
            callback_type,
            function_input_index,
            is_ancestor_capture))
    {
      found_callback_input = true;
      const bool extract_xy = callback_type == GPU_VEC3 && out[output_index].type == GPU_VEC2;
      if ((!extract_xy && callback_type != out[output_index].type) || callback_type == GPU_NONE ||
          function_input_index < 0)
      {
        GPU_material_closure_callback_input_frame_error_set(
            mat,
            "Closure Input item '" + std::string(socket->name) +
                "' has an incompatible callback transport type or input index");
        return 0;
      }
      callback_bindings.append({true, function_input_index, callback_type});
    }
    else
    {
      if (is_ancestor_capture && out[output_index].hasoutput)
      {
        GPU_material_closure_callback_input_frame_error_set(
            mat,
            "Nested closure callback cannot capture Closure Input item '" +
                std::string(socket->name) + "' from an outer callback frame");
        return 0;
      }
      callback_bindings.append({});
    }
    output_index++;
  }
  if (found_callback_input) {
    for (const int index : callback_bindings.index_range()) {
      if (!out[index].hasoutput) {
        continue;
      }
      const CallbackBinding &binding = callback_bindings[index];
      if (!binding.found) {
        GPU_material_closure_callback_input_frame_error_set(
            mat,
            "Active closure callback frame has no binding for Closure Input item '" +
                std::string(node->output_socket(index).name) + "'");
        return 0;
      }
      const std::string input_expr = "$OUT = in" +
                                     std::to_string(binding.function_input_index);
      GPUNodeLink *input_link = GPU_function_call(input_expr.c_str());
      const char *set_function = binding.transport_type == GPU_FLOAT ? "set_value" :
                                 binding.transport_type == GPU_VEC3  ? "set_rgb" :
                                 binding.transport_type == GPU_VEC4  ? "set_rgba" :
                                                                      nullptr;
      if (set_function == nullptr || !GPU_link(mat, set_function, input_link, &out[index].link)) {
        GPU_material_closure_callback_input_frame_error_set(
            mat,
            "Could not materialize Closure Input item '" +
                std::string(node->output_socket(index).name) + "' as a typed GPU value");
        return 0;
      }
    }
    return 1;
  }

  const StringRefNull uv_source = GPU_material_closure_uv_source_get(mat);
  if (uv_source.is_empty()) {
    if (GPU_material_closure_callback_input_frame_error_set(
            mat,
            "Active closure callback frame has no bindings for Closure Input node '" +
                std::string(node->name) + "'"))
    {
      return 0;
    }
    for (int i = 0; !out[i].end; i++) {
      if (out[i].type != GPU_NONE && out[i].hasoutput) {
        if (!gpu_link_zero_value(mat, out[i].type, &out[i].link)) {
          return 0;
        }
      }
    }
    return 1;
  }
  const GPUType uv_source_type = GPU_material_closure_uv_source_type_get(mat);

  int uv_output_index = -1;
  output_index = 0;
  for (const bNodeSocket *socket : node->output_sockets()) {
    if (socket->type != SOCK_VECTOR || !STREQ(socket->name, "UV")) {
      output_index++;
      continue;
    }
    uv_output_index = output_index;
    if (!out[output_index].hasoutput) {
      break;
    }
    if (uv_source_type == GPU_VEC3) {
      const std::string uv_attr_expr = "$OUT = " + std::string(uv_source);
      out[output_index].link = GPU_function_call(uv_attr_expr.c_str());
    }
    else {
      const std::string uv_attr_expr = "$OUT = float4(" + std::string(uv_source) + ", 0.0, 1.0)";
      GPUNodeLink *uv_attr_link = GPU_function_call(uv_attr_expr.c_str());
      GPU_link(mat, "node_uvmap", uv_attr_link, &out[output_index].link);
      node_shader_gpu_bump_tex_coord(mat, node, &out[output_index].link);
    }
    break;
  }

  for (int i = 0; !out[i].end; i++) {
    if (i != uv_output_index && out[i].type != GPU_NONE && out[i].hasoutput) {
      if (!gpu_link_zero_value(mat, out[i].type, &out[i].link)) {
        return 0;
      }
    }
  }

  return 1;
}

static void node_register()
{
  static bke::bNodeType ntype;
  sh_geo_node_type_base(&ntype, "NodeClosureInput"_ustr, NODE_CLOSURE_INPUT);
  ntype.ui_name = "Closure Input";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = nullptr;
  ntype.initfunc = node_init;
  ntype.labelfunc = node_label;
  ntype.no_muting = true;
  ntype.insert_link = node_insert_link;
  ntype.gpu_fn = gpu_shader_closure_input;
  ntype.draw_buttons_ex = node_layout_ex;
  bke::node_type_storage(
      ntype, "NodeClosureInput", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace input_node

namespace output_node {

NODE_STORAGE_FUNCS(NodeClosureOutput);

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (node && tree) {
    const NodeClosureOutput &storage = node_storage(*node);
    for (const int i : IndexRange(storage.output_items.items_num)) {
      const NodeClosureOutputItem &item = storage.output_items.items[i];
      const eNodeSocketDatatype socket_type = item.socket_type;
      const UString identifier(ClosureOutputItemsAccessor::socket_identifier_for_item(item));
      const bNodeSocket *socket = find_socket_by_identifier(*node, SOCK_IN, identifier.ref());
      auto &decl = add_closure_socket_declaration(
          b, socket_type, SOCK_IN, UString(item.name), identifier, socket, item.name);
      decl.socket_name_ptr(&tree->id, *ClosureOutputItemsAccessor::item_srna, &item, "name");
      if (item.structure_type != NodeSocketInterfaceStructureType::Auto) {
        decl.structure_type(StructureType(item.structure_type));
      }
      else {
        decl.structure_type(StructureType::Dynamic);
      }
    }
  }
  b.add_input<decl::Extend>(""_ustr, "__extend__"_ustr)
      .custom_draw(socket_items::ui::draw_extend_socket_fn<ClosureOutputItemsAccessor>());
  b.add_output<decl::Closure>("Closure"_ustr);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeClosureOutput *data = MEM_new<NodeClosureOutput>(__func__);
  node->storage = data;
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeClosureOutput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_new<NodeClosureOutput>(__func__, dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;

  socket_items::copy_array<ClosureInputItemsAccessor>(*src_node, *dst_node);
  socket_items::copy_array<ClosureOutputItemsAccessor>(*src_node, *dst_node);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<ClosureInputItemsAccessor>(*node);
  socket_items::destruct_array<ClosureOutputItemsAccessor>(*node);
  MEM_delete(static_cast<NodeClosureOutput *>(node->storage));
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  if (params.C && params.link.fromnode == &params.node && params.link.tosock->type == SOCK_CLOSURE)
  {
    const NodeClosureOutput &storage = node_storage(params.node);
    if (storage.input_items.items_num == 0 && storage.output_items.items_num == 0) {
      SpaceNode *snode = CTX_wm_space_node(params.C);
      if (snode && snode->edittree == &params.ntree) {
        bNode *input_node = bke::zone_type_by_node_type(NODE_CLOSURE_OUTPUT)
                                ->get_corresponding_input(params.ntree, params.node);
        if (input_node) {
          sync_sockets_closure(*snode, *input_node, params.node, nullptr, params.link.tosock);
        }
      }
    }
    return true;
  }
  return socket_items::try_add_item_via_any_extend_socket<ClosureOutputItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ClosureInputItemsAccessor>();
  socket_items::ops::make_common_operators<ClosureOutputItemsAccessor>();
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const bNodeSocket &other_socket = params.other_socket();
  if (other_socket.type != SOCK_CLOSURE) {
    return;
  }
  if (other_socket.in_out == SOCK_OUT) {
    return;
  }
  params.add_item_full_name(IFACE_("Closure"), [](LinkSearchOpParams &params) {
    bNode &input_node = params.add_node("NodeClosureInput"_ustr);
    bNode &output_node = params.add_node("NodeClosureOutput"_ustr);
    output_node.location[0] = 300;

    auto &input_storage = *static_cast<NodeClosureInput *>(input_node.storage);
    input_storage.output_node_id = output_node.identifier;

    params.connect_available_socket(output_node, "Closure"_ustr);

    SpaceNode &snode = *CTX_wm_space_node(&params.C);
    sync_sockets_closure(snode, input_node, output_node, nullptr);
  });
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<ClosureInputItemsAccessor>(&writer, node);
  socket_items::blend_write<ClosureOutputItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<ClosureInputItemsAccessor>(&reader, node);
  socket_items::blend_read_data<ClosureOutputItemsAccessor>(&reader, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  sh_geo_node_type_base(&ntype, "NodeClosureOutput"_ustr, NODE_CLOSURE_OUTPUT);
  ntype.ui_name = "Closure Output";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.labelfunc = input_node::node_label;
  ntype.no_muting = true;
  ntype.register_operators = node_operators;
  ntype.gather_link_search_ops = node_gather_link_searches;
  ntype.insert_link = node_insert_link;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  bke::node_type_storage(ntype, "NodeClosureOutput", node_free_storage, node_copy_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace output_node

}  // namespace nodes::node_geo_closure_cc

namespace nodes {

StructRNA **ClosureInputItemsAccessor::item_srna = &RNA_NodeClosureInputItem;

void ClosureInputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  writer->write_string(item.name);
}

void ClosureInputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

StructRNA **ClosureOutputItemsAccessor::item_srna = &RNA_NodeClosureOutputItem;

void ClosureOutputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  writer->write_string(item.name);
}

void ClosureOutputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace nodes
}  // namespace blender
