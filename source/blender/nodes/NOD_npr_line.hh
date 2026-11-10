/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>

namespace blender {
struct Main;
struct bNodeTree;
namespace nodes {
inline constexpr const char *npr_line_tree_idname = "NPRLineNodeTree";
inline constexpr const char *npr_line_objects_socket = "NodeSocketNPRLineObjects";
inline constexpr const char *npr_line_brush_socket = "NodeSocketNPRLineBrush";
inline constexpr const char *npr_line_plan_socket = "NodeSocketNPRLinePlan";
inline constexpr const char *npr_line_occlusion_socket = "NodeSocketNPRLineOcclusion";
inline constexpr const char *npr_line_reduction_socket = "NodeSocketNPRLineReduction";
inline constexpr const char *npr_line_map_socket = "NodeSocketNPRLineMap";
void register_npr_line_nodes();
void npr_line_tag_tree_changed(Main &bmain, bNodeTree &tree);
/** Temporary viewport isolation. This state is never stored in a blend file. */
int npr_line_solo_node(const bNodeTree &tree);
void npr_line_set_solo_node(Main &bmain, bNodeTree &tree, int node_identifier);
bool npr_line_timing_enabled(const bNodeTree &tree);
void npr_line_set_timing_enabled(Main &bmain, bNodeTree &tree, bool enabled);
void npr_line_set_timing(const bNodeTree &tree, const std::string &text);
std::string npr_line_timing(const bNodeTree &tree);
}  // namespace nodes
}  // namespace blender
