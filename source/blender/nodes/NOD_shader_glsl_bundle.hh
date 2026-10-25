/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"
#include "DNA_node_types.h"
#include "RNA_types.hh"

namespace blender {
struct bNode;
struct bNodeSocket;
struct bNodeTree;
struct Main;

namespace nodes {

/** Source-owned schema. Never contains evaluated nodes, contexts, or GPU resources. */
struct GLSLBundleField {
  std::string name;
  std::string type_name;
  eNodeSocketDatatype socket_type = SOCK_FLOAT;
  int dimensions = 0;
  bool matrix = false;
  Vector<GLSLBundleField, 0> members;
  std::string label;
  std::string description;
  std::optional<std::string> default_expression;
  bool literal_default = false;
  float4 default_value = float4(0.0f);
  int int_default = 0;
  std::optional<float> min_value;
  std::optional<float> max_value;
  std::optional<int> int_min_value;
  std::optional<int> int_max_value;
  PropertySubType subtype = PROP_NONE;

  bool is_optional() const;
};

struct GLSLBundleSocket {
  std::string identifier;
  eNodeSocketInOut in_out = SOCK_IN;
  GLSLBundleField field;
};

struct GLSLBundleInterface {
  Vector<GLSLBundleSocket> sockets;
};

/** Only attached to a compilation-local node. Presence survives constant folding. */
struct GLSLBundleLowering {
  Set<std::string> default_fields;
};

bool shader_glsl_bundle_interface(const bNode &node,
                                  GLSLBundleInterface &r_interface,
                                  std::string &r_error);
std::string shader_glsl_bundle_leaf_identifier(StringRef root_identifier, StringRef field_path);
void shader_glsl_bundle_lower_node(bNodeTree &tree,
                                   bNode &node,
                                   std::shared_ptr<const GLSLBundleLowering> lowering);
std::string shader_glsl_bundle_error_signature(const bNode &node);
std::string shader_glsl_bundle_error_get(const bNode &node);
bool shader_glsl_bundle_create_input(Main &bmain,
                                     bNodeTree &tree,
                                     bNode &node,
                                     StringRef socket_identifier,
                                     StringRef override_path,
                                     std::string &r_error);

}  // namespace nodes
}  // namespace blender
