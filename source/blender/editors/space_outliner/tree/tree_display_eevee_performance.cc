/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "BKE_scene_runtime.hh"

#include "DNA_outliner_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "eevee_telemetry.hh"

#include "../outliner_intern.hh"
#include "tree_display.hh"

namespace blender::ed::outliner {

namespace {

struct PerfNode {
  std::string label;
  bool default_open = false;
  bool has_time = false;
  double time_ms = 0.0;
  std::vector<std::unique_ptr<PerfNode>> children;

  PerfNode(std::string label, bool default_open = false)
      : label(std::move(label)), default_open(default_open)
  {
  }
};

struct ParsedReport {
  std::string title;
  bool has_total = false;
  double total_ms = 0.0;
  std::vector<std::string> metadata;
  std::unique_ptr<PerfNode> features = std::make_unique<PerfNode>("Features", true);
  std::unique_ptr<PerfNode> stages = std::make_unique<PerfNode>("Stages", true);
  std::unique_ptr<PerfNode> hints = std::make_unique<PerfNode>("Hints", true);
};

std::string trim_copy(const std::string &value)
{
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
    start++;
  }
  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }
  return value.substr(start, end - start);
}

std::string format_ms(const double value)
{
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3) << value;
  return stream.str();
}

bool startswith(const std::string &value, const char *prefix)
{
  return value.rfind(prefix, 0) == 0;
}

bool parse_double_from_suffix(const std::string &value, double &r_value)
{
  size_t ms_pos = value.find(" ms");
  if (ms_pos == std::string::npos) {
    return false;
  }
  size_t begin = ms_pos;
  while (begin > 0) {
    const char c = value[begin - 1];
    if ((c >= '0' && c <= '9') || c == '.') {
      begin--;
      continue;
    }
    break;
  }
  if (begin == ms_pos) {
    return false;
  }
  r_value = std::atof(value.substr(begin, ms_pos - begin).c_str());
  return true;
}

PerfNode &ensure_child(PerfNode &parent, const std::string &label, const bool default_open = false)
{
  for (std::unique_ptr<PerfNode> &child : parent.children) {
    if (child->label == label) {
      if (default_open) {
        child->default_open = true;
      }
      return *child;
    }
  }
  parent.children.push_back(std::make_unique<PerfNode>(label, default_open));
  return *parent.children.back();
}

PerfNode &ensure_stage_path(PerfNode &root, const char *path)
{
  PerfNode *node = &root;
  const char *segment_begin = path;
  for (const char *ch = path;; ch++) {
    if (*ch != '/' && *ch != '\0') {
      continue;
    }
    const std::string label(segment_begin, ch - segment_begin);
    node = &ensure_child(*node, label, false);
    if (*ch == '\0') {
      break;
    }
    segment_begin = ch + 1;
  }
  return *node;
}

void populate_stage_skeleton(PerfNode &stages_root)
{
  for (const eevee::TelemetryStageInfo &stage_info : eevee::TelemetryModule::stage_infos()) {
    ensure_stage_path(stages_root, stage_info.tree_path);
  }
}

double finalize_stage_times(PerfNode &node)
{
  double child_total = 0.0;
  for (std::unique_ptr<PerfNode> &child : node.children) {
    child_total += finalize_stage_times(*child);
  }

  if (!node.has_time) {
    node.has_time = true;
    node.time_ms = child_total;
  }
  return node.time_ms;
}

void sort_nodes_by_time(PerfNode &node)
{
  std::stable_sort(node.children.begin(),
                   node.children.end(),
                   [](const std::unique_ptr<PerfNode> &a, const std::unique_ptr<PerfNode> &b) {
                     return a->time_ms > b->time_ms;
                   });
  for (std::unique_ptr<PerfNode> &child : node.children) {
    sort_nodes_by_time(*child);
  }
}

const eevee::TelemetryStageInfo *find_stage_info_by_label(const std::string &stage_name)
{
  for (const eevee::TelemetryStageInfo &stage_info : eevee::TelemetryModule::stage_infos()) {
    if (stage_name == stage_info.label) {
      return &stage_info;
    }
  }
  return nullptr;
}

void add_stage_line(PerfNode &stages_root, const std::string &line)
{
  const size_t ms_pos = line.find(" ms");
  if (ms_pos == std::string::npos) {
    return;
  }

  size_t value_begin = ms_pos;
  while (value_begin > 0) {
    const char c = line[value_begin - 1];
    if ((c >= '0' && c <= '9') || c == '.') {
      value_begin--;
      continue;
    }
    break;
  }
  const std::string stage_name = trim_copy(line.substr(0, value_begin));
  if (stage_name.empty()) {
    return;
  }
  const double time_ms = std::atof(trim_copy(line.substr(value_begin, ms_pos - value_begin)).c_str());

  const eevee::TelemetryStageInfo *stage_info = find_stage_info_by_label(stage_name);
  PerfNode *node = &stages_root;
  if (stage_info != nullptr) {
    node = &ensure_stage_path(stages_root, stage_info->tree_path);
  }
  else {
    PerfNode &other = ensure_child(stages_root, "Other", true);
    node = &ensure_child(other, stage_name, false);
  }
  node->has_time = true;
  node->time_ms = time_ms;
}

void add_feature_items(PerfNode &features_root, const std::string &line)
{
  struct FeatureKey {
    const char *source;
    const char *group;
    const char *label;
  };

  static const FeatureKey keys[] = {
      {"AO", "Pipeline Features", "Ambient Occlusion"},
      {"DOF", "Pipeline Features", "Depth of Field"},
      {"MB", "Pipeline Features", "Motion Blur"},
      {"Volume", "Pipeline Features", "Volume"},
      {"Raytrace", "Pipeline Features", "Raytrace"},
      {"Filters", "Scene Counts", "Filter Materials"},
      {"RenderTextures", "Scene Counts", "Render Textures"},
      {"Lights", "Scene Counts", "Lights"},
      {"Probes", "Scene Counts", "Probes"},
      {"NPR Mats", "Scene Counts", "NPR Materials"},
      {"Raycast Mats", "Scene Counts", "Raycast Materials"},
      {"GLSL Mats", "Scene Counts", "GLSL Function Materials"},
  };

  for (const FeatureKey &key : keys) {
    const std::string needle = std::string(key.source) + "=";
    const size_t start = line.find(needle);
    if (start == std::string::npos) {
      continue;
    }

    size_t end = line.size();
    for (const FeatureKey &other : keys) {
      if (&other == &key) {
        continue;
      }
      const std::string other_needle = std::string(other.source) + "=";
      const size_t other_pos = line.find(other_needle, start + needle.size());
      if (other_pos != std::string::npos) {
        end = std::min(end, other_pos);
      }
    }

    const std::string value = trim_copy(line.substr(start + needle.size(), end - (start + needle.size())));
    PerfNode &group = ensure_child(features_root, key.group, true);
    ensure_child(group, std::string(key.label) + ": " + value, false);
  }
}

ParsedReport parse_report(const std::string &root_title,
                         const char *report,
                         const char *empty_message,
                         const bool sort_by_time)
{
  ParsedReport parsed;
  parsed.title = root_title;

  if (report == nullptr || report[0] == '\0') {
    parsed.metadata.push_back(empty_message);
    return parsed;
  }

  populate_stage_skeleton(*parsed.stages);

  const char *line_start = report;
  while (*line_start != '\0') {
    const char *line_end = std::strchr(line_start, '\n');
    if (line_end == nullptr) {
      line_end = line_start + std::strlen(line_start);
    }
    const std::string raw_line(line_start, line_end - line_start);
    const std::string line = trim_copy(raw_line);

    if (!line.empty()) {
      if (startswith(line, "Viewport CPU:")) {
        double total = 0.0;
        if (parse_double_from_suffix(line, total)) {
          parsed.has_total = true;
          parsed.total_ms = total;
        }
      }
      else if (startswith(line, "Total CPU:")) {
        double total = 0.0;
        if (parse_double_from_suffix(line, total)) {
          parsed.has_total = true;
          parsed.total_ms = total;
        }
      }
      else if (startswith(line, "Frame:") || startswith(line, "Sample Index:") ||
               startswith(line, "Sample Progress:") || startswith(line, "Sampling:"))
      {
        parsed.metadata.push_back(line);
      }
      else if (startswith(line, "Features:")) {
        add_feature_items(*parsed.features, line.substr(std::strlen("Features:")));
      }
      else if (startswith(raw_line, "  - ")) {
        add_stage_line(*parsed.stages, trim_copy(raw_line.substr(4)));
      }
      else if (line == "Hints:") {
        /* handled by next lines */
      }
      else if (startswith(line, "* ")) {
        ensure_child(*parsed.hints, line.substr(2), false);
      }
    }

    if (*line_end == '\0') {
      break;
    }
    line_start = line_end + 1;
  }

  finalize_stage_times(*parsed.stages);

  if (sort_by_time) {
    sort_nodes_by_time(*parsed.stages);
  }

  return parsed;
}

void collect_node_keys(const PerfNode &node,
                       const std::string &persistent_prefix,
                       std::vector<std::string> &r_keys)
{
  const std::string persistent_key = persistent_prefix + "/" + node.label;
  r_keys.push_back(persistent_key);
  for (const std::unique_ptr<PerfNode> &child : node.children) {
    collect_node_keys(*child, persistent_key, r_keys);
  }
}

void collect_report_keys(const ParsedReport &report, std::vector<std::string> &r_keys)
{
  const std::string root_key = report.title;
  r_keys.push_back(root_key);

  if (!report.metadata.empty()) {
    const std::string meta_key = root_key + "/Metadata";
    r_keys.push_back(meta_key);
    for (const std::string &line : report.metadata) {
      r_keys.push_back(meta_key + "/" + line);
    }
  }

  if (!report.features->children.empty()) {
    collect_node_keys(*report.features, root_key, r_keys);
  }
  if (!report.stages->children.empty()) {
    const std::string stages_key = root_key + "/Stages";
    r_keys.push_back(stages_key);
    for (const std::unique_ptr<PerfNode> &child : report.stages->children) {
      collect_node_keys(*child, stages_key, r_keys);
    }
  }
  if (!report.hints->children.empty()) {
    collect_node_keys(*report.hints, root_key, r_keys);
  }
}

struct PerformanceTreeBuilder {
  SpaceOutliner &space_outliner;
  ID &owner_id;
  std::unordered_map<std::string, short> key_indices;

  PerformanceTreeBuilder(SpaceOutliner &space_outliner,
                         ID &owner_id,
                         std::vector<std::string> persistent_keys)
      : space_outliner(space_outliner), owner_id(owner_id)
  {
    std::sort(persistent_keys.begin(), persistent_keys.end());
    persistent_keys.erase(
        std::unique(persistent_keys.begin(), persistent_keys.end()), persistent_keys.end());
    for (size_t index = 0; index < persistent_keys.size(); index++) {
      key_indices.emplace(persistent_keys[index], short(index + 1));
    }
  }

  short stable_index_for_key(const std::string &persistent_key) const
  {
    const auto it = key_indices.find(persistent_key);
    return (it != key_indices.end()) ? it->second : 0;
  }

  TreeElement &add_label(ListBaseT<TreeElement> &tree,
                         TreeElement *parent,
                         const std::string &label,
                         const std::string &persistent_key,
                         const bool default_open = false)
  {
    TreeElement *te = AbstractTreeDisplay::add_element(&space_outliner,
                                                       &tree,
                                                       &owner_id,
                                                       (void *)label.c_str(),
                                                       parent,
                                                       TSE_GENERIC_LABEL,
                                                       stable_index_for_key(persistent_key));
    if (default_open) {
      TreeStoreElem *tselem = TREESTORE(te);
      if (tselem != nullptr && !tselem->used) {
        tselem->flag &= ~TSE_CLOSED;
      }
    }
    return *te;
  }
};

std::string label_with_time(const PerfNode &node)
{
  if (node.has_time) {
    return node.label + " (" + format_ms(node.time_ms) + " ms)";
  }
  return node.label;
}

void append_node_tree(PerformanceTreeBuilder &builder,
                      TreeElement &parent,
                      const PerfNode &node,
                      const std::string &persistent_prefix)
{
  const std::string persistent_key = persistent_prefix + "/" + node.label;
  TreeElement &te = builder.add_label(
      parent.subtree, &parent, label_with_time(node), persistent_key, node.default_open);
  for (const std::unique_ptr<PerfNode> &child : node.children) {
    append_node_tree(builder, te, *child, persistent_key);
  }
}

void append_parsed_report(PerformanceTreeBuilder &builder, ListBaseT<TreeElement> &tree, const ParsedReport &report)
{
  std::string root_label = report.title;
  const std::string root_key = report.title;
  if (report.has_total) {
    root_label += " (" + format_ms(report.total_ms) + " ms)";
  }
  TreeElement &root = builder.add_label(tree, nullptr, root_label, root_key, true);

  if (!report.metadata.empty()) {
    const std::string meta_key = root_key + "/Metadata";
    TreeElement &meta = builder.add_label(root.subtree, &root, "Metadata", meta_key, true);
    for (const std::string &line : report.metadata) {
      builder.add_label(meta.subtree, &meta, line, meta_key + "/" + line, false);
    }
  }

  if (!report.features->children.empty()) {
    append_node_tree(builder, root, *report.features, root_key);
  }
  if (!report.stages->children.empty()) {
    const std::string stages_key = root_key + "/Stages";
    TreeElement &stages = builder.add_label(root.subtree, &root, "Stages", stages_key, true);
    for (const std::unique_ptr<PerfNode> &child : report.stages->children) {
      append_node_tree(builder, stages, *child, stages_key);
    }
  }
  if (!report.hints->children.empty()) {
    append_node_tree(builder, root, *report.hints, root_key);
  }
}

}  // namespace

TreeDisplayEeveePerformance::TreeDisplayEeveePerformance(SpaceOutliner &space_outliner)
    : AbstractTreeDisplay(space_outliner)
{
}

ListBaseT<TreeElement> TreeDisplayEeveePerformance::build_tree(const TreeSourceData &source_data)
{
  ListBaseT<TreeElement> tree = {nullptr};
  Scene &scene = *source_data.scene;
  const bke::SceneEeveePerformanceRuntime *runtime =
      (scene.runtime != nullptr) ? &scene.runtime->eevee_performance : nullptr;

  const bool sort_by_time = (scene.eevee.flag & SCE_EEVEE_PERFORMANCE_PROFILER_SORT_BY_TIME) != 0;

  const ParsedReport viewport_report = parse_report(
      "Viewport",
      (runtime != nullptr) ? runtime->viewport_report.c_str() : nullptr,
      "No viewport timing captured yet.",
      sort_by_time);
  const ParsedReport render_report = parse_report(
      "Final Render",
      (runtime != nullptr) ? runtime->render_report.c_str() : nullptr,
      "No final render timing captured yet.",
      sort_by_time);

  std::vector<std::string> persistent_keys;
  collect_report_keys(viewport_report, persistent_keys);
  collect_report_keys(render_report, persistent_keys);
  PerformanceTreeBuilder builder{space_outliner_, scene.id, persistent_keys};

  append_parsed_report(builder, tree, viewport_report);
  append_parsed_report(builder, tree, render_report);

  return tree;
}

}  // namespace blender::ed::outliner
