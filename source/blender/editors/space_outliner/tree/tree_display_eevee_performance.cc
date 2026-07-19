/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BKE_scene_runtime.hh"

#include "BLI_hash.h"

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
  std::string key;
  bool default_open = false;
  bool has_time = false;
  double time_ms = 0.0;
  std::vector<std::unique_ptr<PerfNode>> children;

  PerfNode(std::string label, bool default_open = false, std::string key = "")
      : label(std::move(label)), key(std::move(key)), default_open(default_open)
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
  std::unique_ptr<PerfNode> shader_waits = std::make_unique<PerfNode>("Shader Waits", true);
  std::unique_ptr<PerfNode> pass_readback = std::make_unique<PerfNode>("Pass Readback", true);
  std::unique_ptr<PerfNode> material_sync = std::make_unique<PerfNode>("Material Sync", true);
  std::unique_ptr<PerfNode> shadow_contexts = std::make_unique<PerfNode>("Shadow Contexts", true);
  std::unique_ptr<PerfNode> shadow_lights = std::make_unique<PerfNode>("Shadow Lights", true);
  std::unique_ptr<PerfNode> probe_costs = std::make_unique<PerfNode>("Probe Costs", true);
};

enum class ReportSection {
  Main,
  ShaderWaits,
  PassReadback,
  MaterialSync,
  ShadowContexts,
  ShadowLights,
  ProbeCosts,
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

const std::string &node_key(const PerfNode &node)
{
  return node.key.empty() ? node.label : node.key;
}

std::string metadata_key_from_line(const std::string &line)
{
  const size_t colon_pos = line.find(':');
  return (colon_pos == std::string::npos) ? line : line.substr(0, colon_pos);
}

std::string detail_key_from_line(const std::string &line)
{
  if (startswith(line, "... ")) {
    const size_t more_pos = line.find(" more");
    if (more_pos != std::string::npos) {
      return "..." + line.substr(more_pos);
    }
    return "...";
  }

  static const char *metric_tokens[] = {
      " type=",
      " requests=",
      " passes=",
      " cpu=",
      " waits=",
      " updated=",
  };

  size_t token_pos = std::string::npos;
  for (const char *token : metric_tokens) {
    const size_t pos = line.find(token);
    if (pos != std::string::npos) {
      token_pos = (token_pos == std::string::npos) ? pos : std::min(token_pos, pos);
    }
  }
  if (token_pos != std::string::npos) {
    const std::string key = trim_copy(line.substr(0, token_pos));
    if (!key.empty()) {
      return key;
    }
  }

  const size_t equal_pos = line.find('=');
  if (equal_pos != std::string::npos) {
    return trim_copy(line.substr(0, equal_pos));
  }
  const size_t colon_pos = line.find(':');
  if (colon_pos != std::string::npos) {
    return trim_copy(line.substr(0, colon_pos));
  }
  return line;
}

bool parse_double_from_suffix(const std::string &value, double &r_value)
{
  const size_t ms_pos = value.find(" ms");
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

PerfNode &ensure_child(PerfNode &parent,
                       const std::string &label,
                       const bool default_open = false,
                       const std::string &key = "")
{
  const std::string child_key = key.empty() ? label : key;
  for (std::unique_ptr<PerfNode> &child : parent.children) {
    if (node_key(*child) == child_key) {
      child->label = label;
      if (default_open) {
        child->default_open = true;
      }
      return *child;
    }
  }
  parent.children.push_back(std::make_unique<PerfNode>(label, default_open, child_key));
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
    ensure_child(group, std::string(key.label) + ": " + value, false, key.label);
  }
}

void add_detail_line(PerfNode &root, const std::string &line)
{
  std::string key = detail_key_from_line(line);
  ensure_child(root, line, false, key);
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

  ReportSection section = ReportSection::Main;
  bool has_timing_domain = false;
  bool has_accounting = false;
  const char *line_start = report;
  while (*line_start != '\0') {
    const char *line_end = std::strchr(line_start, '\n');
    if (line_end == nullptr) {
      line_end = line_start + std::strlen(line_start);
    }
    const std::string raw_line(line_start, line_end - line_start);
    const std::string line = trim_copy(raw_line);

    if (!line.empty()) {
      if (startswith(line, "Viewport Draw CPU:") || startswith(line, "Viewport CPU:")) {
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
      else if (startswith(line, "Average Draw CPU:") || startswith(line, "Timing Domain:") ||
               startswith(line, "Timing Scope:") || startswith(line, "Accounting:") ||
               startswith(line, "Profiler Accounting CPU:") || startswith(line, "Render Run:") ||
               startswith(line, "Last Evaluation:") ||
               startswith(line, "Depsgraph Eval Serial:") || startswith(line, "Frame:") ||
               startswith(line, "View Layer:") || startswith(line, "Render View:") ||
               startswith(line, "Sample Index:") || startswith(line, "Sample Progress:") ||
               startswith(line, "Sample Count:") || startswith(line, "Sampling:"))
      {
        parsed.metadata.push_back(line);
        has_timing_domain |= startswith(line, "Timing Domain:");
        has_accounting |= startswith(line, "Accounting:");
      }
      else if (startswith(line, "Features:")) {
        section = ReportSection::Main;
        add_feature_items(*parsed.features, line.substr(std::strlen("Features:")));
      }
      else if (line == "Shadow Lights:") {
        section = ReportSection::ShadowLights;
      }
      else if (line == "Shader Waits:") {
        section = ReportSection::ShaderWaits;
      }
      else if (line == "Pass Readback:") {
        section = ReportSection::PassReadback;
      }
      else if (line == "Material Sync:") {
        section = ReportSection::MaterialSync;
      }
      else if (line == "Shadow Contexts:") {
        section = ReportSection::ShadowContexts;
      }
      else if (line == "Probe Costs:") {
        section = ReportSection::ProbeCosts;
      }
      else if (startswith(raw_line, "    - ") && section == ReportSection::ShadowLights) {
        add_detail_line(*parsed.shadow_lights, trim_copy(raw_line.substr(6)));
      }
      else if (startswith(raw_line, "    - ") && section == ReportSection::ShaderWaits) {
        add_detail_line(*parsed.shader_waits, trim_copy(raw_line.substr(6)));
      }
      else if (startswith(raw_line, "    - ") && section == ReportSection::PassReadback) {
        add_detail_line(*parsed.pass_readback, trim_copy(raw_line.substr(6)));
      }
      else if (startswith(raw_line, "    - ") && section == ReportSection::MaterialSync) {
        add_detail_line(*parsed.material_sync, trim_copy(raw_line.substr(6)));
      }
      else if (startswith(raw_line, "    - ") && section == ReportSection::ShadowContexts) {
        add_detail_line(*parsed.shadow_contexts, trim_copy(raw_line.substr(6)));
      }
      else if (startswith(raw_line, "    - ") && section == ReportSection::ProbeCosts) {
        add_detail_line(*parsed.probe_costs, trim_copy(raw_line.substr(6)));
      }
      else if (startswith(raw_line, "  - ")) {
        section = ReportSection::Main;
        add_stage_line(*parsed.stages, trim_copy(raw_line.substr(4)));
      }
    }

    if (*line_end == '\0') {
      break;
    }
    line_start = line_end + 1;
  }

  if (!has_timing_domain) {
    /* Legacy reports predate the explicit field, but all values here are CPU wall-time samples. */
    parsed.metadata.push_back("Timing Domain: CPU wall time");
  }
  if (!has_accounting) {
    parsed.metadata.push_back(
        "Accounting: Inclusive scopes (parent rows include child time; do not sum nested rows)");
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
  const std::string persistent_key = persistent_prefix + "/" + node_key(node);
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
      r_keys.push_back(meta_key + "/" + metadata_key_from_line(line));
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
  if (!report.shader_waits->children.empty()) {
    collect_node_keys(*report.shader_waits, root_key, r_keys);
  }
  if (!report.pass_readback->children.empty()) {
    collect_node_keys(*report.pass_readback, root_key, r_keys);
  }
  if (!report.material_sync->children.empty()) {
    collect_node_keys(*report.material_sync, root_key, r_keys);
  }
  if (!report.shadow_contexts->children.empty()) {
    collect_node_keys(*report.shadow_contexts, root_key, r_keys);
  }
  if (!report.shadow_lights->children.empty()) {
    collect_node_keys(*report.shadow_lights, root_key, r_keys);
  }
  if (!report.probe_costs->children.empty()) {
    collect_node_keys(*report.probe_costs, root_key, r_keys);
  }
}

std::array<const PerfNode *, 7> report_attribution_nodes(const ParsedReport &report)
{
  return {report.features.get(),
          report.shader_waits.get(),
          report.pass_readback.get(),
          report.material_sync.get(),
          report.shadow_contexts.get(),
          report.shadow_lights.get(),
          report.probe_costs.get()};
}

void collect_report_attribution_keys(const ParsedReport &report,
                                     const std::string &snapshot_key,
                                     std::vector<std::string> &r_keys)
{
  for (const PerfNode *node : report_attribution_nodes(report)) {
    if (!node->children.empty()) {
      collect_node_keys(*node, snapshot_key, r_keys);
    }
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
    std::unordered_set<short> used_indices;
    for (const std::string &key : persistent_keys) {
      short index = hash_index_for_key(key);
      while (used_indices.find(index) != used_indices.end()) {
        index = next_index(index);
      }
      used_indices.insert(index);
      key_indices.emplace(key, index);
    }
  }

  static short hash_index_for_key(const std::string &persistent_key)
  {
    return short((BLI_hash_string(persistent_key.c_str()) % 30000u) + 1u);
  }

  static short next_index(const short index)
  {
    return (index >= 30000) ? 1 : short(index + 1);
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
  const std::string persistent_key = persistent_prefix + "/" + node_key(node);
  TreeElement &te = builder.add_label(
      parent.subtree, &parent, label_with_time(node), persistent_key, node.default_open);
  for (const std::unique_ptr<PerfNode> &child : node.children) {
    append_node_tree(builder, te, *child, persistent_key);
  }
}

void append_report_attribution(PerformanceTreeBuilder &builder,
                               TreeElement &snapshot_root,
                               const ParsedReport &report,
                               const std::string &snapshot_key)
{
  /* Structured snapshots already contain the stage tree. Only retain report-only attribution. */
  for (const PerfNode *node : report_attribution_nodes(report)) {
    if (!node->children.empty()) {
      append_node_tree(builder, snapshot_root, *node, snapshot_key);
    }
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
      builder.add_label(
          meta.subtree, &meta, line, meta_key + "/" + metadata_key_from_line(line), false);
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
  if (!report.shader_waits->children.empty()) {
    append_node_tree(builder, root, *report.shader_waits, root_key);
  }
  if (!report.pass_readback->children.empty()) {
    append_node_tree(builder, root, *report.pass_readback, root_key);
  }
  if (!report.material_sync->children.empty()) {
    append_node_tree(builder, root, *report.material_sync, root_key);
  }
  if (!report.shadow_contexts->children.empty()) {
    append_node_tree(builder, root, *report.shadow_contexts, root_key);
  }
  if (!report.shadow_lights->children.empty()) {
    append_node_tree(builder, root, *report.shadow_lights, root_key);
  }
  if (!report.probe_costs->children.empty()) {
    append_node_tree(builder, root, *report.probe_costs, root_key);
  }
}

struct SnapshotNodeMetrics {
  double current_ms = 0.0;
  double self_ms = 0.0;
  bool ran_directly = false;
  bool ran = false;
};

using SnapshotNodeMetricsCache =
    std::unordered_map<const bke::SceneEeveePerformanceNode *, SnapshotNodeMetrics>;

SnapshotNodeMetrics snapshot_node_metrics(const bke::SceneEeveePerformanceNode &node,
                                          SnapshotNodeMetricsCache &cache)
{
  const auto cached = cache.find(&node);
  if (cached != cache.end()) {
    return cached->second;
  }
  double children_current_ms = 0.0;
  bool child_ran = false;
  for (const bke::SceneEeveePerformanceNode &child : node.children) {
    const SnapshotNodeMetrics child_metrics = snapshot_node_metrics(child, cache);
    children_current_ms += child_metrics.current_ms;
    child_ran |= child_metrics.ran;
  }

  SnapshotNodeMetrics metrics;
  metrics.ran_directly = node.active || node.calls > 0 || node.current_ms > 0.0;
  metrics.ran = metrics.ran_directly || child_ran;

  /* Some nodes exist only to preserve scope hierarchy. Aggregate children only for those
   * synthetic nodes; direct scopes already publish authoritative inclusive and self timings. */
  const double measured_current_ms = std::max(0.0, node.current_ms);
  metrics.current_ms = metrics.ran_directly ? measured_current_ms : children_current_ms;
  metrics.self_ms = metrics.ran_directly ? std::max(0.0, node.self_ms) : 0.0;
  cache.emplace(&node, metrics);
  return metrics;
}

std::string snapshot_node_label(const bke::SceneEeveePerformanceNode &node,
                                SnapshotNodeMetricsCache &cache)
{
  const SnapshotNodeMetrics metrics = snapshot_node_metrics(node, cache);
  std::string label = node.label;
  if (!metrics.ran) {
    label += " (Not Run";
    return label + ")";
  }

  label += " (" + format_ms(metrics.current_ms) + " ms";
  if (!metrics.ran_directly) {
    label += " aggregate";
  }
  if (metrics.ran_directly) {
    label += ", self " + format_ms(metrics.self_ms) + " ms, calls " +
             std::to_string(node.calls);
  }
  return label + ")";
}

std::vector<const bke::SceneEeveePerformanceNode *> snapshot_node_children(
    const bke::SceneEeveePerformanceNode &node,
    const bool sort_by_time,
    SnapshotNodeMetricsCache &cache)
{
  std::vector<const bke::SceneEeveePerformanceNode *> children;
  children.reserve(node.children.size());
  for (const bke::SceneEeveePerformanceNode &child : node.children) {
    children.push_back(&child);
  }
  if (sort_by_time) {
    std::stable_sort(children.begin(),
                     children.end(),
                     [&cache](const bke::SceneEeveePerformanceNode *a,
                              const bke::SceneEeveePerformanceNode *b) {
                       const SnapshotNodeMetrics a_metrics = snapshot_node_metrics(*a, cache);
                       const SnapshotNodeMetrics b_metrics = snapshot_node_metrics(*b, cache);
                       if (a_metrics.current_ms != b_metrics.current_ms) {
                         return a_metrics.current_ms > b_metrics.current_ms;
                       }
                       return a->id < b->id;
                     });
  }
  return children;
}

std::string snapshot_node_key(const std::string &snapshot_key,
                              const bke::SceneEeveePerformanceNode &node)
{
  return snapshot_key + "/node/" + node.id;
}

void collect_snapshot_node_keys(const bke::SceneEeveePerformanceNode &node,
                                const std::string &snapshot_key,
                                std::vector<std::string> &r_keys,
                                const bke::SceneEeveePerformanceNode *omitted_node = nullptr)
{
  if (&node == omitted_node) {
    return;
  }
  r_keys.push_back(snapshot_node_key(snapshot_key, node));
  for (const bke::SceneEeveePerformanceNode &child : node.children) {
    collect_snapshot_node_keys(child, snapshot_key, r_keys, omitted_node);
  }
}

struct SnapshotReportCache {
  bool sort_by_time;
  std::unordered_map<const bke::SceneEeveePerformanceSnapshot *, std::unique_ptr<ParsedReport>>
      reports;

  explicit SnapshotReportCache(const bool sort_by_time) : sort_by_time(sort_by_time) {}

  const ParsedReport *get(const bke::SceneEeveePerformanceSnapshot &snapshot)
  {
    if (snapshot.report.empty()) {
      return nullptr;
    }
    const auto cached = reports.find(&snapshot);
    if (cached != reports.end()) {
      return cached->second.get();
    }
    auto report = std::make_unique<ParsedReport>(
        parse_report(snapshot.kind, snapshot.report.c_str(), "", sort_by_time));
    const ParsedReport *result = report.get();
    reports.emplace(&snapshot, std::move(report));
    return result;
  }
};

const bke::SceneEeveePerformanceNode *snapshot_node_find(
    const bke::SceneEeveePerformanceNode &node, const char *label);

void collect_snapshot_content_keys(const bke::SceneEeveePerformanceSnapshot &snapshot,
                                   const std::string &snapshot_key,
                                   std::vector<std::string> &r_keys,
                                   SnapshotReportCache &report_cache)
{
  const std::string metadata_key = snapshot_key + "/metadata";
  r_keys.push_back(metadata_key);
  for (const char *key : {"identity",
                          "schema",
                          "mode-status",
                          "source",
                          "scene-view-layer",
                          "render-run-id",
                          "render-view",
                          "sequence",
                          "frame",
                          "sampling",
                          "resolution",
                          "timing-domain",
                          "timing-scope",
                          "accounting",
                          "last-evaluation",
                          "depsgraph-eval-serial",
                          "total-cpu",
                          "draw-sync",
                          "draw-submission",
                          "profiler-accounting"})
  {
    r_keys.push_back(metadata_key + "/" + key);
  }
  const bke::SceneEeveePerformanceNode *current_main_view = snapshot_node_find(
      snapshot.root, "MainView");
  const std::string main_view_key = snapshot_key + "/main-view";
  r_keys.push_back(main_view_key);
  if (current_main_view) {
    for (const bke::SceneEeveePerformanceNode &child : current_main_view->children) {
      collect_snapshot_node_keys(child, main_view_key, r_keys);
    }
  }
  for (const bke::SceneEeveePerformanceNode &child : snapshot.root.children) {
    collect_snapshot_node_keys(child, snapshot_key, r_keys, current_main_view);
  }
  if (snapshot.root.children.empty()) {
    r_keys.push_back(snapshot_key + "/not-run");
  }
  if (const ParsedReport *attribution = report_cache.get(snapshot)) {
    collect_report_attribution_keys(*attribution, snapshot_key, r_keys);
  }
}

void collect_snapshot_keys(const bke::SceneEeveePerformanceSnapshot &snapshot,
                           const std::string &snapshot_key,
                           std::vector<std::string> &r_keys,
                           SnapshotReportCache &report_cache)
{
  r_keys.push_back(snapshot_key);
  collect_snapshot_content_keys(snapshot, snapshot_key, r_keys, report_cache);
}

const bke::SceneEeveePerformanceNode *snapshot_node_find(
    const bke::SceneEeveePerformanceNode &node, const char *label)
{
  if (node.label == label) {
    return &node;
  }
  for (const bke::SceneEeveePerformanceNode &child : node.children) {
    if (const bke::SceneEeveePerformanceNode *result = snapshot_node_find(child, label)) {
      return result;
    }
  }
  return nullptr;
}

void append_snapshot_node(PerformanceTreeBuilder &builder,
                          TreeElement &parent,
                          const bke::SceneEeveePerformanceNode &node,
                          const std::string &snapshot_key,
                          const bool sort_by_time,
                          SnapshotNodeMetricsCache &metrics_cache,
                          const bke::SceneEeveePerformanceNode *omitted_node = nullptr,
                          const bool force_open = false,
                          const std::string *label_override = nullptr,
                          const std::string *key_override = nullptr)
{
  if (&node == omitted_node) {
    return;
  }
  TreeElement &element = builder.add_label(parent.subtree,
                                           &parent,
                                           label_override ? *label_override :
                                                            snapshot_node_label(node, metrics_cache),
                                           key_override ? *key_override :
                                                          snapshot_node_key(snapshot_key, node),
                                           force_open || node.kind != "stage");
  for (const bke::SceneEeveePerformanceNode *child :
       snapshot_node_children(node, sort_by_time, metrics_cache))
  {
    append_snapshot_node(
        builder, element, *child, snapshot_key, sort_by_time, metrics_cache, omitted_node);
  }
}

std::string snapshot_sample_progress(const bke::SceneEeveePerformanceSnapshot &snapshot)
{
  const uint64_t current = snapshot.sample_count >= 0xFFFFFFu ?
                               uint64_t(snapshot.sample_index) :
                               std::min(uint64_t(snapshot.sample_index), snapshot.sample_count);
  if (snapshot.sample_count >= 0xFFFFFFu) {
    return std::to_string(current) + "/Continuous";
  }
  return std::to_string(current) + "/" + std::to_string(snapshot.sample_count);
}

std::string snapshot_resolution_and_samples(const bke::SceneEeveePerformanceSnapshot &snapshot)
{
  return std::to_string(snapshot.resolution_x) + "x" + std::to_string(snapshot.resolution_y) +
         ", Samples " + snapshot_sample_progress(snapshot);
}

std::string snapshot_root_label(const bke::SceneEeveePerformanceSnapshot &snapshot,
                                const std::string &title,
                                SnapshotNodeMetricsCache &metrics_cache)
{
  const SnapshotNodeMetrics metrics = snapshot_node_metrics(snapshot.root, metrics_cache);
  std::string label = title + " (" + format_ms(metrics.current_ms) + " ms";
  label += ", self " + format_ms(metrics.self_ms) + " ms, " +
           snapshot_resolution_and_samples(snapshot) + ")";
  return label;
}

void append_snapshot_contents(PerformanceTreeBuilder &builder,
                              TreeElement &root,
                              const bke::SceneEeveePerformanceSnapshot &snapshot,
                              const std::string &snapshot_key,
                              const bool sort_by_time,
                              SnapshotNodeMetricsCache &metrics_cache,
                              SnapshotReportCache &report_cache)
{
  const std::string metadata_key = snapshot_key + "/metadata";
  TreeElement &metadata = builder.add_label(root.subtree, &root, "Metadata", metadata_key, false);
  builder.add_label(metadata.subtree,
                    &metadata,
                    "id=" + snapshot.id + " kind=" + snapshot.kind,
                    metadata_key + "/identity");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "schema=" + snapshot.schema,
                    metadata_key + "/schema");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "mode=" + snapshot.mode + " status=" + snapshot.status,
                    metadata_key + "/mode-status");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "source=" + snapshot.source_label +
                        " source_id=" + std::to_string(snapshot.source_id),
                    metadata_key + "/source");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "scene_uid=" + std::to_string(snapshot.scene_session_uid) +
                        " view_layer=" +
                        (snapshot.view_layer_name.empty() ? "<None>" : snapshot.view_layer_name),
                    metadata_key + "/scene-view-layer");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "render_run_id=" + std::to_string(snapshot.render_run_id),
                    metadata_key + "/render-run-id");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "render_view=" +
                        (snapshot.render_view_name.empty() ? "<default>" :
                                                              snapshot.render_view_name),
                    metadata_key + "/render-view");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "epoch=" + std::to_string(snapshot.epoch) +
                        " capture_seq=" + std::to_string(snapshot.capture_seq),
                    metadata_key + "/sequence");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "frame=" + std::to_string(snapshot.frame),
                    metadata_key + "/frame");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "sample=" + snapshot_sample_progress(snapshot) +
                        " playback=" + (snapshot.is_playback ? "true" : "false"),
                    metadata_key + "/sampling");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "resolution=" + snapshot_resolution_and_samples(snapshot),
                    metadata_key + "/resolution");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "Timing Domain: " + snapshot.timing_domain,
                    metadata_key + "/timing-domain");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "Timing Scope: " + snapshot.timing_scope,
                    metadata_key + "/timing-scope");
  builder.add_label(
      metadata.subtree,
      &metadata,
      "Accounting: Inclusive scopes (parent rows include child time; do not sum nested rows)",
      metadata_key + "/accounting");
  builder.add_label(metadata.subtree,
                    &metadata,
                    snapshot.has_last_evaluation ?
                        "Last Evaluation: " + format_ms(snapshot.last_evaluation_ms) +
                            " ms (not included in Draw CPU)" :
                        "Last Evaluation: n/a",
                    metadata_key + "/last-evaluation");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "depsgraph_eval_serial=" + std::to_string(snapshot.depsgraph_eval_serial),
                    metadata_key + "/depsgraph-eval-serial");
  builder.add_label(metadata.subtree,
                    &metadata,
                    (startswith(snapshot.kind, "viewport") ? "Viewport Draw CPU: " :
                                                            "Total CPU: ") +
                        format_ms(snapshot.total_cpu_ms) + " ms",
                    metadata_key + "/total-cpu");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "Draw Sync: " + format_ms(snapshot.draw_sync_ms) + " ms",
                    metadata_key + "/draw-sync");
  builder.add_label(metadata.subtree,
                    &metadata,
                    "Draw/Submission: " + format_ms(snapshot.draw_submission_ms) + " ms",
                    metadata_key + "/draw-submission");
  builder.add_label(
      metadata.subtree,
      &metadata,
      "Profiler Accounting CPU: " + format_ms(snapshot.profiler_accounting_ms) +
          " ms (excluded from Draw CPU; pre-publication accounting only)",
      metadata_key + "/profiler-accounting");

  const bke::SceneEeveePerformanceNode *current_main_view = snapshot_node_find(
      snapshot.root, "MainView");
  const std::string main_view_key = snapshot_key + "/main-view";
  if (current_main_view) {
    const std::string main_view_label = snapshot_node_label(*current_main_view, metrics_cache);
    append_snapshot_node(builder,
                         root,
                         *current_main_view,
                         main_view_key,
                         sort_by_time,
                         metrics_cache,
                         nullptr,
                         true,
                         &main_view_label,
                         &main_view_key);
  }
  else {
    builder.add_label(root.subtree, &root, "MainView (Not Run)", main_view_key, true);
  }

  if (snapshot.root.children.empty()) {
    builder.add_label(
        root.subtree, &root, "Stages (Not Run)", snapshot_key + "/not-run", false);
  }
  else {
    for (const bke::SceneEeveePerformanceNode *child :
         snapshot_node_children(snapshot.root, sort_by_time, metrics_cache))
    {
      append_snapshot_node(builder,
                           root,
                           *child,
                           snapshot_key,
                           sort_by_time,
                           metrics_cache,
                           current_main_view);
    }
  }

  if (const ParsedReport *attribution = report_cache.get(snapshot)) {
    append_report_attribution(builder, root, *attribution, snapshot_key);
  }
}

void append_snapshot(PerformanceTreeBuilder &builder,
                     ListBaseT<TreeElement> &tree,
                     TreeElement *parent,
                     const bke::SceneEeveePerformanceSnapshot &snapshot,
                     const std::string &title,
                     const std::string &snapshot_key,
                     const bool sort_by_time,
                     SnapshotNodeMetricsCache &metrics_cache,
                     SnapshotReportCache &report_cache)
{
  ListBaseT<TreeElement> &target = parent ? parent->subtree : tree;
  TreeElement &root = builder.add_label(
      target, parent, snapshot_root_label(snapshot, title, metrics_cache), snapshot_key, true);
  append_snapshot_contents(
      builder,
      root,
      snapshot,
      snapshot_key,
      sort_by_time,
      metrics_cache,
      report_cache);
}

std::string final_render_name_key(const std::string &name)
{
  /* Length-prefix names so separators and the empty default view cannot collide. */
  return std::to_string(name.size()) + ":" + name;
}

std::string final_render_snapshot_key(const bke::SceneEeveePerformanceSnapshot &snapshot)
{
  return "Final Render/scene-" + std::to_string(snapshot.scene_session_uid) + "/layer-" +
         final_render_name_key(snapshot.view_layer_name) + "/view-" +
         final_render_name_key(snapshot.render_view_name);
}

bool viewport_source_closed(const bke::SceneEeveePerformanceViewportSource &source)
{
  return source.closed || source.lifetime.expired();
}

std::vector<std::shared_ptr<const bke::SceneEeveePerformanceSnapshot>> final_render_snapshots(
    const bke::SceneEeveePerformanceSnapshotSet &snapshots)
{
  std::vector<std::shared_ptr<const bke::SceneEeveePerformanceSnapshot>> result;
  for (const std::shared_ptr<const bke::SceneEeveePerformanceSnapshot> &snapshot :
       snapshots.final_renders)
  {
    if (snapshot) {
      result.push_back(snapshot);
    }
  }
  if (result.empty() && snapshots.final_render) {
    result.push_back(snapshots.final_render);
  }
  return result;
}

void collect_structured_snapshot_keys(const bke::SceneEeveePerformanceSnapshotSet &snapshots,
                                      std::vector<std::string> &r_keys,
                                      SnapshotReportCache &report_cache)
{
  r_keys.push_back("Viewport Sources");
  for (const bke::SceneEeveePerformanceViewportSource &source : snapshots.viewport_sources) {
    const std::string source_key = "Viewport Sources/source-" + std::to_string(source.source_id);
    r_keys.push_back(source_key);
    if (source.latest) {
      collect_snapshot_content_keys(*source.latest, source_key, r_keys, report_cache);
    }
    if (!source.latest) {
      r_keys.push_back(source_key + "/not-run");
    }
  }
  r_keys.push_back("Final Render");
  const auto final_snapshots = final_render_snapshots(snapshots);
  if (final_snapshots.size() == 1) {
    collect_snapshot_content_keys(*final_snapshots.front(), "Final Render", r_keys, report_cache);
  }
  else {
    for (const std::shared_ptr<const bke::SceneEeveePerformanceSnapshot> &snapshot :
         final_snapshots)
    {
      collect_snapshot_keys(*snapshot, final_render_snapshot_key(*snapshot), r_keys, report_cache);
    }
  }
  r_keys.push_back("Color Bake");
  if (snapshots.color_bake) {
    collect_snapshot_keys(*snapshots.color_bake, "Color Bake", r_keys, report_cache);
  }
  r_keys.push_back("Light Probe Bake");
  if (snapshots.light_probe_bake) {
    collect_snapshot_keys(*snapshots.light_probe_bake, "Light Probe Bake", r_keys, report_cache);
  }
}

void append_structured_snapshots(PerformanceTreeBuilder &builder,
                                 ListBaseT<TreeElement> &tree,
                                 const bke::SceneEeveePerformanceSnapshotSet &snapshots,
                                 const bool sort_by_time,
                                 SnapshotReportCache &report_cache)
{
  SnapshotNodeMetricsCache metrics_cache;
  const bool has_viewport_source = !snapshots.viewport_sources.empty();
  TreeElement &viewports = builder.add_label(
      tree,
      nullptr,
      has_viewport_source ? "Viewport Sources" : "Viewport Sources (Not Run)",
      "Viewport Sources",
      true);
  std::vector<const bke::SceneEeveePerformanceViewportSource *> ordered_sources;
  ordered_sources.reserve(snapshots.viewport_sources.size());
  for (const bke::SceneEeveePerformanceViewportSource &source : snapshots.viewport_sources) {
    ordered_sources.push_back(&source);
  }
  std::sort(ordered_sources.begin(), ordered_sources.end(), [](const auto *a, const auto *b) {
    const bool a_closed = viewport_source_closed(*a);
    const bool b_closed = viewport_source_closed(*b);
    return (a_closed != b_closed) ? !a_closed : a->source_id > b->source_id;
  });
  for (const bke::SceneEeveePerformanceViewportSource *source_ptr : ordered_sources) {
    const bke::SceneEeveePerformanceViewportSource &source = *source_ptr;
    const std::string source_key = "Viewport Sources/source-" + std::to_string(source.source_id);
    const bool source_closed = viewport_source_closed(source);
    TreeElement &source_root = builder.add_label(viewports.subtree,
                                                 &viewports,
                                                 source.label + (source_closed ? " (Closed)" : ""),
                                                 source_key,
                                                 !source_closed);
    if (source.latest) {
      append_snapshot_contents(builder,
                               source_root,
                               *source.latest,
                               source_key,
                               sort_by_time,
                               metrics_cache,
                               report_cache);
    }
    if (!source.latest) {
      builder.add_label(source_root.subtree,
                        &source_root,
                        "No timing captured yet (Not Run)",
                        source_key + "/not-run");
    }
  }
  const auto final_snapshots = final_render_snapshots(snapshots);
  if (final_snapshots.empty()) {
    builder.add_label(tree, nullptr, "Final Render (Not Run)", "Final Render", true);
  }
  else if (final_snapshots.size() == 1) {
    const bke::SceneEeveePerformanceSnapshot &snapshot = *final_snapshots.front();
    TreeElement &final_root = builder.add_label(
        tree,
        nullptr,
        snapshot_root_label(snapshot, "Final Render", metrics_cache),
        "Final Render",
        true);
    append_snapshot_contents(
        builder, final_root, snapshot, "Final Render", sort_by_time, metrics_cache, report_cache);
  }
  else {
    TreeElement &final_root = builder.add_label(
        tree,
        nullptr,
        "Final Render (" + std::to_string(final_snapshots.size()) + " results)",
        "Final Render",
        true);
    for (const std::shared_ptr<const bke::SceneEeveePerformanceSnapshot> &snapshot :
         final_snapshots)
    {
      const std::string layer_name = snapshot->view_layer_name.empty() ? std::string("<None>") :
                                                                         snapshot->view_layer_name;
      const std::string view_name = snapshot->render_view_name.empty() ?
                                        std::string("<default>") :
                                        snapshot->render_view_name;
      append_snapshot(builder,
                      tree,
                      &final_root,
                      *snapshot,
                      "Layer " + layer_name + " / View " + view_name,
                      final_render_snapshot_key(*snapshot),
                      sort_by_time,
                      metrics_cache,
                      report_cache);
    }
  }
  if (snapshots.color_bake) {
    append_snapshot(builder,
                    tree,
                    nullptr,
                    *snapshots.color_bake,
                    "Color Bake",
                    "Color Bake",
                    sort_by_time,
                    metrics_cache,
                    report_cache);
  }
  else {
    builder.add_label(tree, nullptr, "Color Bake (Not Implemented)", "Color Bake", true);
  }
  if (snapshots.light_probe_bake) {
    append_snapshot(builder,
                    tree,
                    nullptr,
                    *snapshots.light_probe_bake,
                    "Light Probe Bake",
                    "Light Probe Bake",
                    sort_by_time,
                    metrics_cache,
                    report_cache);
  }
  else {
    builder.add_label(
        tree, nullptr, "Light Probe Bake (Not Implemented)", "Light Probe Bake", true);
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

  const std::shared_ptr<const bke::SceneEeveePerformanceSnapshotSet> snapshots =
      runtime ? runtime->snapshot_set_get() : nullptr;
  if (snapshots) {
    SnapshotReportCache report_cache(sort_by_time);
    std::vector<std::string> persistent_keys;
    collect_structured_snapshot_keys(*snapshots, persistent_keys, report_cache);
    PerformanceTreeBuilder builder{space_outliner_, scene.id, std::move(persistent_keys)};
    append_structured_snapshots(builder, tree, *snapshots, sort_by_time, report_cache);
    return tree;
  }

  const std::string legacy_viewport_report =
      runtime ? runtime->viewport_report_get() : std::string();
  const std::string legacy_render_report = runtime ? runtime->render_report_get() : std::string();

  if (legacy_viewport_report.empty() && legacy_render_report.empty()) {
    const bke::SceneEeveePerformanceSnapshotSet empty_snapshots;
    SnapshotReportCache report_cache(sort_by_time);
    std::vector<std::string> persistent_keys;
    collect_structured_snapshot_keys(empty_snapshots, persistent_keys, report_cache);
    PerformanceTreeBuilder builder{space_outliner_, scene.id, std::move(persistent_keys)};
    append_structured_snapshots(builder, tree, empty_snapshots, sort_by_time, report_cache);
    return tree;
  }

  const ParsedReport viewport_report = parse_report(
      "Viewport",
      legacy_viewport_report.c_str(),
      "No viewport timing captured yet.",
      sort_by_time);
  const ParsedReport render_report = parse_report(
      "Final Render",
      legacy_render_report.c_str(),
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
