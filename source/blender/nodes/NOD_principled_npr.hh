/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <algorithm>
#include <climits>

#include "DNA_node_types.h"

namespace blender::nodes::principled_npr {

inline NodePrincipledNPRRampPoint *find_point(NodeShaderPrincipledNPR &data, const int identifier)
{
  for (NodePrincipledNPRRampPoint &point : data.ramp_points) {
    if (point.initialized && point.identifier == identifier) {
      return &point;
    }
  }
  return nullptr;
}

inline int sorted_ramp_points(
    const NodeShaderPrincipledNPR &data,
    const NodePrincipledNPRRampPoint *(&points)[SHD_PRINCIPLED_NPR_MAX_RAMP_POINTS])
{
  int count = 0;
  for (const NodePrincipledNPRRampPoint &point : data.ramp_points) {
    if (point.initialized && point.enabled) {
      points[count++] = &point;
    }
  }
  std::sort(points, points + count, [](const auto *a, const auto *b) {
    const float a_position = std::clamp(a->position, 0.0f, 1.0f);
    const float b_position = std::clamp(b->position, 0.0f, 1.0f);
    return a_position < b_position || (a_position == b_position && a->identifier < b->identifier);
  });
  return count;
}

/** Analytic CPU counterpart of npr_v2_map_sorted, shared by UI preview and RNA evaluation.
 * Do not use the legacy ColorBand LUT: hard boundaries and narrow intervals must be preserved. */
inline void evaluate_ramp(const NodeShaderPrincipledNPR &data,
                          const float coordinate,
                          const float coordinate_scale,
                          const float coordinate_offset,
                          const float global_softness,
                          float color[4])
{
  const NodePrincipledNPRRampPoint *points[SHD_PRINCIPLED_NPR_MAX_RAMP_POINTS];
  const int count = sorted_ramp_points(data, points);
  if (count == 0) {
    std::fill_n(color, 4, 0.0f);
    return;
  }
  const float x = std::clamp(coordinate * coordinate_scale + coordinate_offset, 0.0f, 1.0f);
  int right = 0;
  while (right < count && x >= std::clamp(points[right]->position, 0.0f, 1.0f)) {
    right++;
  }
  if (right == 0 || right == count || x == std::clamp(points[right - 1]->position, 0.0f, 1.0f)) {
    std::copy_n(points[std::max(right - 1, 0)]->color, 4, color);
    return;
  }
  const float low = std::clamp(points[right - 1]->position, 0.0f, 1.0f);
  const float high = std::clamp(points[right]->position, 0.0f, 1.0f);
  const float interval = high - low;
  const float center = 0.5f * (low + high) -
                       std::clamp(points[right]->offset, -0.5f, 0.5f) * interval;
  float width = std::clamp(std::max(global_softness, 0.0f) *
                               std::max(points[right]->softness, 0.0f),
                           0.0f,
                           1.0f) *
                interval;
  width = std::min(width, 2.0f * std::min(center - low, high - center));
  float factor = x >= center ? 1.0f : 0.0f;
  if (width > 0.0f) {
    factor = std::clamp((x - center) / width + 0.5f, 0.0f, 1.0f);
    if (data.mapping_interpolation != SHD_PRINCIPLED_NPR_INTERP_LINEAR) {
      factor = factor * factor * (3.0f - 2.0f * factor);
    }
  }
  for (int channel = 0; channel < 4; channel++) {
    color[channel] = points[right - 1]->color[channel] * (1.0f - factor) +
                     points[right]->color[channel] * factor;
  }
}

inline float largest_gap_midpoint(const NodeShaderPrincipledNPR &data)
{
  float positions[SHD_PRINCIPLED_NPR_MAX_RAMP_POINTS + 2];
  int count = 0;
  positions[count++] = 0.0f;
  for (const NodePrincipledNPRRampPoint &point : data.ramp_points) {
    if (point.initialized && point.enabled) {
      positions[count++] = point.position;
    }
  }
  positions[count++] = 1.0f;
  std::sort(positions, positions + count);
  float width = -1.0f;
  float position = 0.5f;
  for (int i = 1; i < count; i++) {
    if (positions[i] - positions[i - 1] > width) {
      width = positions[i] - positions[i - 1];
      position = (positions[i] + positions[i - 1]) * 0.5f;
    }
  }
  return position;
}

/** A newly added point receives a new identity, including when it reuses a deleted slot. */
inline NodePrincipledNPRRampPoint *add_point(NodeShaderPrincipledNPR &data, const float position)
{
  if (data.next_point_identifier == INT_MAX) {
    return nullptr;
  }
  for (NodePrincipledNPRRampPoint &point : data.ramp_points) {
    if (!point.initialized) {
      const NodePrincipledNPRRampPoint *left = nullptr;
      const NodePrincipledNPRRampPoint *right = nullptr;
      for (const NodePrincipledNPRRampPoint &anchor : data.ramp_points) {
        if (!anchor.initialized || !anchor.enabled) {
          continue;
        }
        if (anchor.position <= position && (!left || anchor.position > left->position)) {
          left = &anchor;
        }
        if (anchor.position >= position && (!right || anchor.position < right->position)) {
          right = &anchor;
        }
      }
      point = dna::shallow_copy(NodePrincipledNPRRampPoint{});
      if (left || right) {
        left = left ? left : right;
        right = right ? right : left;
        const float width = right->position - left->position;
        const float factor = width > 0.0f ?
                                 std::clamp((position - left->position) / width, 0.0f, 1.0f) :
                                 0.0f;
        for (int channel = 0; channel < 4; channel++) {
          point.color[channel] = left->color[channel] * (1.0f - factor) +
                                 right->color[channel] * factor;
        }
      }
      point.identifier = data.next_point_identifier++;
      point.position = std::clamp(position, 0.0f, 1.0f);
      point.enabled = point.initialized = 1;
      data.ramp_point_count++;
      data.ramp_initialized_count++;
      data.active_point_identifier = point.identifier;
      return &point;
    }
  }
  return nullptr;
}

inline bool remove_point(NodeShaderPrincipledNPR &data, const int identifier)
{
  NodePrincipledNPRRampPoint *point = find_point(data, identifier);
  if (point == nullptr || (point->enabled && data.ramp_point_count <= 2)) {
    return false;
  }
  data.ramp_point_count -= point->enabled != 0;
  data.ramp_initialized_count--;
  *point = dna::shallow_copy(NodePrincipledNPRRampPoint{});
  if (data.active_point_identifier == identifier) {
    for (const NodePrincipledNPRRampPoint &candidate : data.ramp_points) {
      if (candidate.initialized && candidate.enabled) {
        data.active_point_identifier = candidate.identifier;
        break;
      }
    }
  }
  return true;
}

/** Count changes suspend and restore points, never transfer their animation identity. */
inline void set_point_count(NodeShaderPrincipledNPR &data, const int requested_count)
{
  const int count = std::clamp(requested_count, 2, SHD_PRINCIPLED_NPR_MAX_RAMP_POINTS);
  for (int i = SHD_PRINCIPLED_NPR_MAX_RAMP_POINTS - 1; i >= 0 && data.ramp_point_count > count;
       i--)
  {
    NodePrincipledNPRRampPoint &point = data.ramp_points[i];
    if (point.initialized && point.enabled) {
      point.enabled = 0;
      data.ramp_point_count--;
    }
  }
  for (NodePrincipledNPRRampPoint &point : data.ramp_points) {
    if (data.ramp_point_count == count) {
      break;
    }
    if (point.initialized && !point.enabled) {
      point.enabled = 1;
      data.ramp_point_count++;
    }
  }
  while (data.ramp_point_count < count) {
    if (add_point(data, largest_gap_midpoint(data)) == nullptr) {
      break;
    }
  }
  const NodePrincipledNPRRampPoint *active = find_point(data, data.active_point_identifier);
  if (active == nullptr || !active->enabled) {
    for (const NodePrincipledNPRRampPoint &point : data.ramp_points) {
      if (point.initialized && point.enabled) {
        data.active_point_identifier = point.identifier;
        break;
      }
    }
  }
}

inline void initialize_ramp(NodeShaderPrincipledNPR &data)
{
  for (NodePrincipledNPRRampPoint &point : data.ramp_points) {
    point = dna::shallow_copy(NodePrincipledNPRRampPoint{});
  }
  data.ramp_point_count = data.ramp_initialized_count = data.next_point_identifier = 0;
  NodePrincipledNPRRampPoint *dark = add_point(data, 0.0f);
  dark->color[0] = dark->color[1] = dark->color[2] = 0.45f;
  NodePrincipledNPRRampPoint *light = add_point(data, 1.0f);
  light->color[0] = light->color[1] = light->color[2] = 1.0f;
  data.active_point_identifier = dark->identifier;
}

}  // namespace blender::nodes::principled_npr
