/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

#include "BLI_vector_set.hh"
#include "BLI_utildefines.h"

#include "BKE_node.hh"

#include "NOD_geometry_nodes_closure_signature.hh"
#include "NOD_node_in_compute_context.hh"

namespace blender::nodes {

/** Shader Bundle aliases preserve RGB/RGBA, but never silently resize numeric vectors. */
inline bool bundle_socket_types_compatible(eNodeSocketDatatype expected,
                                           int expected_dimensions,
                                           eNodeSocketDatatype actual,
                                           int actual_dimensions)
{
  const int expected_vector = expected_dimensions == 0 ? 3 : expected_dimensions;
  const int actual_vector = actual_dimensions == 0 ? 3 : actual_dimensions;
  if (expected == actual) {
    return expected != SOCK_VECTOR || expected_vector == actual_vector;
  }
  if (expected == SOCK_VECTOR && actual == SOCK_RGBA) {
    return ELEM(expected_vector, 3, 4);
  }
  if (expected == SOCK_RGBA && actual == SOCK_VECTOR) {
    return ELEM(actual_vector, 3, 4) &&
           (expected_dimensions == 0 || expected_dimensions == actual_vector);
  }
  return false;
}

struct BundleSignature {
  struct Item {
    std::string key;
    const bke::bNodeSocketType *type = nullptr;
    NodeSocketInterfaceStructureType structure_type = NodeSocketInterfaceStructureType::Single;
    int dimensions = 0;
    std::shared_ptr<const BundleSignature> nested;
    bool optional = false;
    /** Authoring data for explicit Sync; never part of the Bundle transport ABI. */
    ClosureSignature::ItemUIData ui;
    bool default_is_expression = false;

    friend bool operator==(const Item &a, const Item &b);
  };

  struct ItemKeyGetter {
    StringRefNull operator()(const Item &item)
    {
      return item.key;
    }
  };

  CustomIDVectorSet<Item, ItemKeyGetter> items;
  /** GLSL declarations require exact field types and non-destructive synchronization. */
  bool strict = false;

  friend bool operator==(const BundleSignature &a, const BundleSignature &b);
  friend bool operator!=(const BundleSignature &a, const BundleSignature &b);

  void add(std::string key, const eNodeSocketDatatype socket_type);

  static BundleSignature from_combine_bundle_node(const bNode &node,
                                                  bool allow_auto_structure_type);
  static BundleSignature from_separate_bundle_node(const bNode &node,
                                                   bool allow_auto_structure_type);

  void set_auto_structure_types();
};

/**
 * Multiple bundle signatures that may be linked to a single node.
 */
struct LinkedBundleSignatures {
  struct Item {
    BundleSignature signature;
    bool is_signature_definition = false;
    SocketInContext source_socket;
  };
  Vector<Item> items;
  bool has_type_definition() const;

  std::optional<BundleSignature> get_merged_signature(std::string *r_error = nullptr) const;
};

NodeSocketInterfaceStructureType get_structure_type_for_bundle_signature(
    const bNodeSocket &socket,
    NodeSocketInterfaceStructureType stored_structure_type,
    bool allow_auto_structure_type);

}  // namespace blender::nodes
