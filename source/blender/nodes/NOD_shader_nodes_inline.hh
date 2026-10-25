/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "DNA_node_types.h"

namespace blender {

struct bNodeTree;
struct bNode;

namespace nodes {

/** Stable source identity. No evaluated data or compute-context pointers escape inlining. */
struct ShaderUniformGroupStep {
  uint32_t parent_tree_uid = 0;
  int group_node_identifier = 0;
  uint32_t child_tree_uid = 0;

  bool operator==(const ShaderUniformGroupStep &other) const
  {
    return parent_tree_uid == other.parent_tree_uid &&
           group_node_identifier == other.group_node_identifier &&
           child_tree_uid == other.child_tree_uid;
  }
};

struct ShaderUniformSource {
  uint32_t tree_uid = 0;
  int node_identifier = 0;
  std::string socket_identifier;
  eNodeSocketInOut in_out = SOCK_IN;
  eNodeSocketDatatype socket_type = SOCK_FLOAT;
  int components = 0;
  Vector<ShaderUniformGroupStep> group_path;

  bool operator==(const ShaderUniformSource &other) const
  {
    return tree_uid == other.tree_uid && node_identifier == other.node_identifier &&
           socket_identifier == other.socket_identifier && in_out == other.in_out &&
           socket_type == other.socket_type && components == other.components &&
           group_path == other.group_path;
  }
};

/** Check target identity as well as its address after preprocessing mutates the local tree. */
struct ShaderUniformTarget {
  ShaderUniformSource source;
  int node_identifier = 0;
  std::string node_name;
  std::string socket_identifier;
  eNodeSocketInOut in_out = SOCK_IN;
  eNodeSocketDatatype socket_type = SOCK_FLOAT;
};

/** Compilation-local provenance; discard pointer keys after GPU bindings have been captured. */
struct ShaderUniformTracking {
  Map<const bNodeSocket *, ShaderUniformTarget> sources;
  Vector<ShaderUniformSource> unsafe_sources;
  bool unsupported_context = false;
};

struct InlineShaderNodeTreeParams {
  /**
   * Disable loop unrolling and keep Repeat Zone nodes in the tree.
   * (For engines with native support for Repeat Zones)
   *
   * Some Repeat Zones may still be unrolled (eg. if they have Closure or Bundle Zone Items).
   */
  bool allow_preserving_repeat_zones = false;

  /* Allow processing only the outputs relevant to specific engines. */
  NodeShaderOutputTarget target_engine_ = SHD_OUTPUT_ALL;

  /** Optional direct-uniform provenance. Does not alter folding or generated node graphs. */
  ShaderUniformTracking *r_uniform_tracking = nullptr;

  struct ErrorMessage {
    /* In theory, more contextual information could be added here like the entire context path to
     * that node. In practice, we can't report errors with that level of detail in shader nodes
     * yet. */
    const bNode *node;
    std::string message;
    bool fatal = false;
  };
  Vector<ErrorMessage> r_error_messages;
};

bool inline_shader_node_tree(const bNodeTree &src_tree,
                             bNodeTree &dst_tree,
                             InlineShaderNodeTreeParams &params);

}  // namespace nodes
}  // namespace blender
