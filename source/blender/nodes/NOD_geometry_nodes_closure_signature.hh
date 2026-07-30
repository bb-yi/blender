/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <variant>

#include "BKE_node.hh"

#include "BLI_math_vector_types.hh"
#include "BLI_vector_set.hh"

#include "NOD_node_in_compute_context.hh"

namespace blender::nodes {

/** Describes the names and types of the inputs and outputs of a closure. */
class ClosureSignature {
 public:
  using ItemDefaultValue = std::variant<float, int, bool, float4>;
  using ItemMinMaxValue = std::variant<float, int>;

  struct ItemUIData {
    std::optional<std::string> label;
    std::optional<std::string> description;
    std::optional<PropertySubType> subtype;
    std::optional<ItemDefaultValue> default_value;
    std::optional<ItemMinMaxValue> min_value;
    std::optional<ItemMinMaxValue> max_value;
    std::optional<bool> hide_value;

    friend bool operator==(const ItemUIData &a, const ItemUIData &b) = default;
  };

  struct Item {
    using DefaultValue = ItemDefaultValue;
    using MinMaxValue = ItemMinMaxValue;
    using UIData = ItemUIData;

    std::string key;
    const bke::bNodeSocketType *type = nullptr;
    NodeSocketInterfaceStructureType structure_type;
    /** Vector dimensions in the range [2, 4]. Zero for all non-vector socket types. */
    int dimensions = 0;
    /** UI-only data. This does not participate in closure ABI compatibility. */
    ItemUIData ui;

    bool has_same_abi(const Item &other) const;

    friend bool operator==(const Item &a, const Item &b) = default;
  };

  struct ItemKeyGetter {
    std::string operator()(const Item &item)
    {
      return item.key;
    }
  };

  CustomIDVectorSet<Item, ItemKeyGetter> inputs;
  CustomIDVectorSet<Item, ItemKeyGetter> outputs;

  std::optional<int> find_input_index(StringRef key) const;
  std::optional<int> find_output_index(StringRef key) const;

  friend bool operator==(const ClosureSignature &a, const ClosureSignature &b);
  friend bool operator!=(const ClosureSignature &a, const ClosureSignature &b);

  static ClosureSignature from_closure_output_node(const bNode &node,
                                                   bool allow_auto_structure_type);
  static ClosureSignature from_evaluate_closure_node(const bNode &node,
                                                     bool allow_auto_structure_type);
  static ClosureSignature from_glsl_function_sample2d_socket(const bNode &node,
                                                             const bNodeSocket &socket);
  static ClosureSignature from_parallax_height_source_socket(const bNode &node,
                                                             const bNodeSocket &socket);
  static ClosureSignature from_closure_to_list_node(const bNode &node);

  void set_auto_structure_types();
};

/**
 * Multiple closure signatures that may be linked to a single node.
 */
struct LinkedClosureSignatures {
  struct Item {
    ClosureSignature signature;
    bool define_signature = false;
    SocketInContext socket;
    /** Allow Color and 3D Vector to share the callback's vec3 transport ABI. */
    bool allow_color_vector3_abi = false;
  };
  Vector<Item> items;
  bool has_type_definition() const;

  std::optional<ClosureSignature> get_merged_signature() const;
};

}  // namespace blender::nodes
