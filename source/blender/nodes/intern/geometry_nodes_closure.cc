/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cfloat>
#include <climits>

#include "BKE_node_runtime.hh"

#include "NOD_geometry_nodes_bundle_signature.hh"
#include "NOD_geometry_nodes_closure.hh"

namespace blender::nodes {

static int normalized_socket_dimensions(const eNodeSocketDatatype socket_type,
                                        const int dimensions)
{
  if (socket_type != SOCK_VECTOR) {
    return 0;
  }
  return ELEM(dimensions, 2, 3, 4) ? dimensions : 3;
}

static bool closure_signature_item_types_have_same_abi(const ClosureSignature::Item &a,
                                                        const ClosureSignature::Item &b)
{
  if (a.type == nullptr || b.type == nullptr) {
    return false;
  }
  return a.type->type == b.type->type &&
         normalized_socket_dimensions(a.type->type, a.dimensions) ==
             normalized_socket_dimensions(b.type->type, b.dimensions);
}

static bool merge_closure_signature_item_type(ClosureSignature::Item &dst,
                                              const ClosureSignature::Item &src,
                                              const bool allow_color_vector3_abi)
{
  if (closure_signature_item_types_have_same_abi(dst, src)) {
    return true;
  }
  if (!allow_color_vector3_abi || dst.type == nullptr || src.type == nullptr) {
    return false;
  }

  const bool dst_is_vector3 = dst.type->type == SOCK_VECTOR &&
                              normalized_socket_dimensions(dst.type->type, dst.dimensions) == 3;
  const bool src_is_vector3 = src.type->type == SOCK_VECTOR &&
                              normalized_socket_dimensions(src.type->type, src.dimensions) == 3;
  if (!((dst.type->type == SOCK_RGBA && src_is_vector3) ||
        (src.type->type == SOCK_RGBA && dst_is_vector3)))
  {
    return false;
  }
  /* A mixed Color/Vector callback ABI uses the generic vec3 socket representation. */
  if (src_is_vector3) {
    dst.type = src.type;
  }
  dst.dimensions = 3;
  return true;
}

static ClosureSignature::ItemUIData closure_signature_ui_data_from_socket(
    const bNodeSocket &socket)
{
  ClosureSignature::ItemUIData ui;
  if (socket.label[0] != '\0') {
    ui.label = socket.label;
  }
  if (socket.description[0] != '\0') {
    ui.description = socket.description;
  }
  if (socket.flag & SOCK_HIDE_VALUE) {
    ui.hide_value = true;
  }
  if (socket.type == SOCK_RGBA) {
    ui.subtype = PROP_COLOR;
  }

  if (!socket.default_value) {
    return ui;
  }
  switch (socket.type) {
    case SOCK_FLOAT: {
      const auto &value = *socket.default_value_typed<bNodeSocketValueFloat>();
      if (value.subtype != PROP_NONE) {
        ui.subtype = PropertySubType(value.subtype);
      }
      ui.default_value = ClosureSignature::ItemDefaultValue(value.value);
      if (value.min != -FLT_MAX) {
        ui.min_value = ClosureSignature::ItemMinMaxValue(value.min);
      }
      if (value.max != FLT_MAX) {
        ui.max_value = ClosureSignature::ItemMinMaxValue(value.max);
      }
      break;
    }
    case SOCK_INT: {
      const auto &value = *socket.default_value_typed<bNodeSocketValueInt>();
      if (value.subtype != PROP_NONE) {
        ui.subtype = PropertySubType(value.subtype);
      }
      ui.default_value = ClosureSignature::ItemDefaultValue(value.value);
      if (value.min != INT_MIN) {
        ui.min_value = ClosureSignature::ItemMinMaxValue(value.min);
      }
      if (value.max != INT_MAX) {
        ui.max_value = ClosureSignature::ItemMinMaxValue(value.max);
      }
      break;
    }
    case SOCK_BOOLEAN: {
      const auto &value = *socket.default_value_typed<bNodeSocketValueBoolean>();
      ui.default_value = ClosureSignature::ItemDefaultValue(bool(value.value));
      break;
    }
    case SOCK_VECTOR: {
      const auto &value = *socket.default_value_typed<bNodeSocketValueVector>();
      if (value.subtype != PROP_NONE) {
        ui.subtype = PropertySubType(value.subtype);
      }
      ui.default_value = ClosureSignature::ItemDefaultValue(
          float4(value.value[0], value.value[1], value.value[2], value.value[3]));
      if (value.min != -FLT_MAX) {
        ui.min_value = ClosureSignature::ItemMinMaxValue(value.min);
      }
      if (value.max != FLT_MAX) {
        ui.max_value = ClosureSignature::ItemMinMaxValue(value.max);
      }
      break;
    }
    case SOCK_RGBA: {
      const auto &value = *socket.default_value_typed<bNodeSocketValueRGBA>();
      ui.default_value = ClosureSignature::ItemDefaultValue(
          float4(value.value[0], value.value[1], value.value[2], value.value[3]));
      break;
    }
    default:
      break;
  }
  return ui;
}

static ClosureSignature::Item closure_signature_item_from_socket(
    const StringRef key,
    const bke::bNodeSocketType &type,
    const NodeSocketInterfaceStructureType structure_type,
    const bNodeSocket &socket)
{
  int dimensions = 0;
  if (socket.type == SOCK_VECTOR && socket.default_value) {
    dimensions = socket.default_value_typed<bNodeSocketValueVector>()->dimensions;
  }
  return {.key = key,
          .type = &type,
          .structure_type = structure_type,
          .dimensions = normalized_socket_dimensions(type.type, dimensions),
          .ui = closure_signature_ui_data_from_socket(socket)};
}

bool ClosureSignature::Item::has_same_abi(const Item &other) const
{
  if (this->key != other.key || this->structure_type != other.structure_type) {
    return false;
  }
  return closure_signature_item_types_have_same_abi(*this, other);
}

std::optional<int> ClosureSignature::find_input_index(const StringRef key) const
{
  for (const int i : this->inputs.index_range()) {
    const Item &item = this->inputs[i];
    if (item.key == key) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<int> ClosureSignature::find_output_index(const StringRef key) const
{
  for (const int i : this->outputs.index_range()) {
    const Item &item = this->outputs[i];
    if (item.key == key) {
      return i;
    }
  }
  return std::nullopt;
}

void ClosureSignature::set_auto_structure_types()
{
  for (const Item &item : this->inputs) {
    const_cast<Item &>(item).structure_type = NodeSocketInterfaceStructureType::Auto;
  }
  for (const Item &item : this->outputs) {
    const_cast<Item &>(item).structure_type = NodeSocketInterfaceStructureType::Auto;
  }
}

bool operator==(const ClosureSignature &a, const ClosureSignature &b)
{
  auto ui_data_matches_for_sync = [](const ClosureSignature::ItemUIData &a,
                                     const ClosureSignature::ItemUIData &b) {
    /* The socket default value is also the user's current value. It must not make an otherwise
     * compatible socket appear out of sync. */
    return a.label == b.label && a.description == b.description && a.subtype == b.subtype &&
           a.min_value == b.min_value && a.max_value == b.max_value &&
           a.hide_value == b.hide_value;
  };
  auto items_match_for_sync = [&](const Span<ClosureSignature::Item> a,
                                  const Span<ClosureSignature::Item> b) {
    if (a.size() != b.size()) {
      return false;
    }
    for (const int i : a.index_range()) {
      if (!a[i].has_same_abi(b[i]) || !ui_data_matches_for_sync(a[i].ui, b[i].ui)) {
        return false;
      }
    }
    return true;
  };
  return items_match_for_sync(a.inputs.as_span(), b.inputs.as_span()) &&
         items_match_for_sync(a.outputs.as_span(), b.outputs.as_span());
}

bool operator!=(const ClosureSignature &a, const ClosureSignature &b)
{
  return !(a == b);
}

ClosureSignature ClosureSignature::from_closure_output_node(const bNode &node,
                                                            const bool allow_auto_structure_type)
{
  BLI_assert(node.is_type("NodeClosureOutput"_ustr));
  const bNodeTree &tree = node.owner_tree();
  const bNode *input_node =
      bke::zone_type_by_node_type(node.type_legacy)->get_corresponding_input(tree, node);
  const auto &storage = *static_cast<const NodeClosureOutput *>(node.storage);
  nodes::ClosureSignature signature;
  if (input_node) {
    for (const int i : IndexRange(storage.input_items.items_num)) {
      const NodeClosureInputItem &item = storage.input_items.items[i];
      const bNodeSocket &socket = input_node->output_socket(i);
      if (const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.socket_type))
      {
        const NodeSocketInterfaceStructureType structure_type =
            get_structure_type_for_bundle_signature(
                socket, item.structure_type, allow_auto_structure_type);
        signature.inputs.add(
            closure_signature_item_from_socket(item.name, *stype, structure_type, socket));
      }
    }
  }
  for (const int i : IndexRange(storage.output_items.items_num)) {
    const NodeClosureOutputItem &item = storage.output_items.items[i];
    const bNodeSocket &socket = node.input_socket(i);
    if (const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.socket_type)) {
      const NodeSocketInterfaceStructureType structure_type =
          get_structure_type_for_bundle_signature(
              socket, item.structure_type, allow_auto_structure_type);
      signature.outputs.add(
          closure_signature_item_from_socket(item.name, *stype, structure_type, socket));
    }
  }
  return signature;
}

ClosureSignature ClosureSignature::from_evaluate_closure_node(const bNode &node,
                                                              const bool allow_auto_structure_type)
{
  BLI_assert(node.is_type("NodeEvaluateClosure"_ustr));
  const auto &storage = *static_cast<const NodeEvaluateClosure *>(node.storage);
  nodes::ClosureSignature signature;
  for (const int i : IndexRange(storage.input_items.items_num)) {
    const NodeEvaluateClosureInputItem &item = storage.input_items.items[i];
    const bNodeSocket &socket = node.input_socket(i + 1);
    if (const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.socket_type)) {
      const NodeSocketInterfaceStructureType structure_type =
          get_structure_type_for_bundle_signature(
              socket, item.structure_type, allow_auto_structure_type);
      signature.inputs.add(
          closure_signature_item_from_socket(item.name, *stype, structure_type, socket));
    }
  }
  for (const int i : IndexRange(storage.output_items.items_num)) {
    const NodeEvaluateClosureOutputItem &item = storage.output_items.items[i];
    const bNodeSocket &socket = node.output_socket(i);
    if (const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(item.socket_type)) {
      const NodeSocketInterfaceStructureType structure_type =
          get_structure_type_for_bundle_signature(
              socket, item.structure_type, allow_auto_structure_type);
      signature.outputs.add(
          closure_signature_item_from_socket(item.name, *stype, structure_type, socket));
    }
  }
  return signature;
}

ClosureSignature ClosureSignature::from_glsl_function_sample2d_socket(const bNode &node,
                                                                      const bNodeSocket &socket)
{
  BLI_assert(node.is_type("ShaderNodeGLSLFunction"_ustr));
  BLI_assert(socket.is_input());
  BLI_assert(socket.type == SOCK_CLOSURE);
  UNUSED_VARS_NDEBUG(node, socket);

  ClosureSignature signature;
  if (const bke::bNodeSocketType *input_type = bke::node_socket_type_find_static(SOCK_VECTOR)) {
    signature.inputs.add({.key = "UV",
                          .type = input_type,
                          .structure_type = NodeSocketInterfaceStructureType::Auto,
                          .dimensions = 3});
  }
  if (const bke::bNodeSocketType *output_type = bke::node_socket_type_find_static(SOCK_RGBA)) {
    signature.outputs.add({.key = "Color",
                           .type = output_type,
                           .structure_type = NodeSocketInterfaceStructureType::Auto,
                           .ui = {.subtype = PROP_COLOR}});
  }
  if (const bke::bNodeSocketType *output_type = bke::node_socket_type_find_static(SOCK_FLOAT)) {
    signature.outputs.add({.key = "Alpha",
                           .type = output_type,
                           .structure_type = NodeSocketInterfaceStructureType::Auto,
                           .ui = {.default_value = ClosureSignature::ItemDefaultValue(1.0f)}});
  }
  return signature;
}

ClosureSignature ClosureSignature::from_parallax_height_source_socket(const bNode &node,
                                                                      const bNodeSocket &socket)
{
  BLI_assert(node.is_type("ShaderNodeParallax"_ustr));
  BLI_assert(socket.is_input());
  BLI_assert(socket.type == SOCK_CLOSURE);
  UNUSED_VARS_NDEBUG(node, socket);

  ClosureSignature signature;
  if (const bke::bNodeSocketType *input_type = bke::node_socket_type_find_static(SOCK_VECTOR)) {
    signature.inputs.add({.key = "UV",
                          .type = input_type,
                          .structure_type = NodeSocketInterfaceStructureType::Auto,
                          .dimensions = 3});
  }
  if (const bke::bNodeSocketType *output_type = bke::node_socket_type_find_static(SOCK_FLOAT)) {
    signature.outputs.add({"Height", output_type, NodeSocketInterfaceStructureType::Auto});
  }
  return signature;
}

ClosureSignature ClosureSignature::from_closure_to_list_node(const bNode &node)
{
  BLI_assert(node.is_type("GeometryNodeClosureToList"_ustr));
  const auto &storage = *static_cast<const GeometryNodeClosureToList *>(node.storage);
  ClosureSignature signature;
  signature.inputs.add({.key = "Index",
                        .type = bke::node_socket_type_find("NodeSocketInt"),
                        .structure_type = NodeSocketInterfaceStructureType::Single});
  for (const int i : IndexRange(storage.items_num)) {
    const GeometryNodeClosureToListItem &item = storage.items[i];
    const auto type = eNodeSocketDatatype(item.socket_type);
    if (const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(type)) {
      signature.outputs.add(
          closure_signature_item_from_socket(item.name,
                                             *stype,
                                             NodeSocketInterfaceStructureType(item.structure_type),
                                             node.output_socket(i)));
    }
  }
  return signature;
}

bool LinkedClosureSignatures::has_type_definition() const
{
  for (const Item &item : this->items) {
    if (item.define_signature) {
      return true;
    }
  }
  return false;
}

std::optional<ClosureSignature> LinkedClosureSignatures::get_merged_signature() const
{
  struct UIConflicts {
    bool label = false;
    bool description = false;
    bool subtype = false;
    bool default_value = false;
    bool min_value = false;
    bool max_value = false;
    bool hide_value = false;
  };

  auto merge_optional = [](auto &dst, const auto &src, bool &has_conflict) {
    if (!has_conflict && dst != src) {
      dst.reset();
      has_conflict = true;
    }
  };
  auto merge_ui_data = [&](ClosureSignature::ItemUIData &dst,
                           const ClosureSignature::ItemUIData &src,
                           UIConflicts &conflicts) {
    merge_optional(dst.label, src.label, conflicts.label);
    merge_optional(dst.description, src.description, conflicts.description);
    merge_optional(dst.subtype, src.subtype, conflicts.subtype);
    merge_optional(dst.default_value, src.default_value, conflicts.default_value);
    merge_optional(dst.min_value, src.min_value, conflicts.min_value);
    merge_optional(dst.max_value, src.max_value, conflicts.max_value);
    merge_optional(dst.hide_value, src.hide_value, conflicts.hide_value);
  };

  ClosureSignature signature;
  Map<std::string, UIConflicts> input_ui_conflicts;
  Map<std::string, UIConflicts> output_ui_conflicts;
  Map<std::string, bool> input_callback_abi;
  Map<std::string, bool> output_callback_abi;
  for (const Item &src_signature : this->items) {
    for (const ClosureSignature::Item &item : src_signature.signature.inputs) {
      if (signature.inputs.add(item)) {
        input_callback_abi.add_new(item.key, src_signature.allow_color_vector3_abi);
      }
      else {
        ClosureSignature::Item &existing_item = const_cast<ClosureSignature::Item &>(
            *signature.inputs.lookup_key_ptr_as(item.key));
        bool &existing_callback_abi = input_callback_abi.lookup(item.key);
        if (!merge_closure_signature_item_type(existing_item,
                                               item,
                                               existing_callback_abi ||
                                                   src_signature.allow_color_vector3_abi))
        {
          return std::nullopt;
        }
        existing_callback_abi |= src_signature.allow_color_vector3_abi;
        if (existing_item.structure_type != item.structure_type) {
          existing_item.structure_type = NodeSocketInterfaceStructureType::Dynamic;
        }
        merge_ui_data(
            existing_item.ui, item.ui, input_ui_conflicts.lookup_or_add_default(item.key));
      }
    }
    for (const ClosureSignature::Item &item : src_signature.signature.outputs) {
      if (signature.outputs.add(item)) {
        output_callback_abi.add_new(item.key, src_signature.allow_color_vector3_abi);
      }
      else {
        ClosureSignature::Item &existing_item = const_cast<ClosureSignature::Item &>(
            *signature.outputs.lookup_key_ptr_as(item.key));
        bool &existing_callback_abi = output_callback_abi.lookup(item.key);
        if (!merge_closure_signature_item_type(existing_item,
                                               item,
                                               existing_callback_abi ||
                                                   src_signature.allow_color_vector3_abi))
        {
          return std::nullopt;
        }
        existing_callback_abi |= src_signature.allow_color_vector3_abi;
        if (existing_item.structure_type != item.structure_type) {
          existing_item.structure_type = NodeSocketInterfaceStructureType::Dynamic;
        }
        merge_ui_data(
            existing_item.ui, item.ui, output_ui_conflicts.lookup_or_add_default(item.key));
      }
    }
  }
  return signature;
}

}  // namespace blender::nodes
