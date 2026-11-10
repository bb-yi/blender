/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

namespace blender::eevee::geometry_lines {

enum EdgeType : uint32_t {
  Outline = 1 << 0,
  ObjectContour = 1 << 1,
  Intersection = 1 << 2,
  Smoothing = 1 << 3,
  MaterialBoundary = 1 << 4,
  Normal = 1 << 5,
  Wire = 1 << 6,
  Marked = 1 << 7,
};

struct UVMapKey {
  std::string name;
  int index = -1; /* Negative uses the named map, or the active map for an empty name. */
  bool operator<(const UVMapKey &other) const
  {
    return std::tie(name, index) < std::tie(other.name, other.index);
  }
};

/** Immutable image snapshot. Alpha is data, independent of the scene view transform. */
struct AlphaMask {
  int width = 0;
  int height = 0;
  float threshold = 0.5f;
  float minimum = 0;
  float maximum = 1;
  std::vector<float> alpha;
  struct Level {
    int2 extent;
    std::vector<float2> bounds;
  };
  std::vector<Level> hierarchy;
  void build_hierarchy();
  /** Conservative bilinear/repeat coverage of a UV interval: -1 mixed, 0 empty, 1 solid. */
  int coverage(double2 a, double2 b) const;
  bool sample(double2 uv) const;
};

struct Surface {
  uint64_t instance = 0;
  uint32_t resource_id = 0;
  uint32_t material_tag = 0;
  std::vector<int> rules;
  /** 0 = solid, 1 = ignore, 2 = alpha mask. */
  int occlusion = 0;
  std::shared_ptr<const AlphaMask> mask;
};

struct Face {
  int surface = 0;
  double3 normal;
  uint64_t identity = 0;
};

struct Triangle {
  int3 vertices;
  int face = 0;
  std::array<double2, 3> uv;
};

struct Edge {
  int2 vertices;
  int face_start = 0;
  int face_count = 0;
  uint32_t types = Wire;
  double normal_dot = 1;
  uint64_t identity = 0;
  bool loose = false;
  int seam_face = -1;
  uint32_t seam_types = 0;
  bool seam_secondary = false;
};

/** All evaluated data is owned by this snapshot; worker threads never dereference Blender IDs. */
struct Geometry {
  std::vector<double3> positions;
  std::vector<double3> previous_positions;
  std::vector<double3> next_positions;
  bool has_motion_steps = false;
  std::vector<uint64_t> vertex_identities;
  std::vector<uint64_t> seam_vertex_identities;
  std::vector<Surface> surfaces;
  std::vector<Face> faces;
  std::vector<Triangle> triangles;
  std::vector<Edge> edges;
  std::vector<int> edge_faces;
  /** Corner data indexed by triangle * 3; absent mesh layers use non-finite coordinates. */
  std::map<UVMapKey, std::vector<float2>> style_uvs;
  std::map<UVMapKey, std::vector<float4>> style_colors;
  std::map<UVMapKey, std::vector<float4>> point_colors;
  uint64_t attributes_revision = 0;
  uint64_t revision = 0;
  uint64_t intersection_revision = 0;
};

/** Connection and ordering can be shared through a brush while scope stays on the rule. */
struct StrokeConnection {
  bool clockwise = true;
  float intersection_join = 1;
  float gap_join = 2;
  float split_angle = 70;
  float minimum_length = 0;
  float direction = 0;
};

struct Rule : StrokeConnection {
  uint32_t visible = Outline | ObjectContour;
  uint32_t hidden = Outline | ObjectContour;
  bool hidden_enabled = false;
  bool open_edges = true;
  bool visible_open_outline = true, visible_open_object = true;
  bool hidden_open_outline = true, hidden_open_object = true;
  bool object_contours = false;
  bool contour_priority = false;
  bool self_intersection = true;
  bool connect_objects = false;
  bool connect_coincident = false;
  bool screen_space_join = true;
  bool repair_seams = false;
  float normal_min = 30;
  float normal_max = 180;
  float join_depth = 0.08f;
  std::vector<std::pair<int, StrokeConnection>> brush_connections;
  int width_reduction = -1;
  int opacity_reduction = -1;
  int hidden_width_reduction = -1;
  int hidden_opacity_reduction = -1;
  std::array<int, 8> visible_brush{};
  std::array<int, 8> hidden_brush{};
  std::array<float, 8> width_scale = {1, 1, 1, 1, 1, 1, 1, 1};
};

/** Priority nodes claim geometry before line-type selection, including disabled edge types. */
struct Plan {
  int rule = -1;
  bool overlay = false;
  std::vector<Plan> children;
};

struct Camera {
  double4x4 projection;
  double4x4 previous_projection;
  double4x4 next_projection;
  double3 position;
  double3 forward;
  int2 extent;
  bool perspective = true;
  double clip_margin = 64;
  bool use_previous_geometry = true;
  /** Interactive approximation. Visibility is tested against the current GPU depth buffer. */
  bool quick = false;
};

struct IntersectionEdge {
  double3 a, b;
  int2 faces;
  uint64_t identity;
  int2 triangles;
};

/** UV/color lookup survives clipping and reconnection without depending on the active map. */
struct AttributePoint {
  int triangle = -1; /* >=0 triangle, -1 invalid, -(edge+2) a loose source edge. */
  float2 barycentric = float2(0);
  float inverse_w = 1;
};

struct Segment {
  double3 a, b;
  double join_depth_a = 0, join_depth_b = 0;
  double3 previous_a, previous_b;
  double3 next_a, next_b;
  double source_start = 0;
  double source_end = 1;
  uint64_t identity = 0;
  uint64_t instance = 0;
  uint64_t secondary_instance = 0; /* The other surface participating in an intersection. */
  /** Topological endpoint identities, zero for an interior visibility cut. */
  uint64_t vertex_a = 0, vertex_b = 0;
  AttributePoint attribute_a, attribute_b;
  int rule = 0;
  int brush = 0;
  int type = 0;
  float width_scale = 1;
  bool hidden = false;
  bool intersection = false;
};

struct StrokePart {
  Segment segment;
  double arc = 0;
  double length = 0;
  float2 offset_normal_a = float2(0), offset_normal_b = float2(0);
};

struct Stroke {
  std::vector<StrokePart> parts;
  double length = 0;
  uint64_t identity = 0;
  int brush = 0;
  bool closed = false;
};

struct Statistics {
  uint64_t candidates = 0;
  uint64_t triangle_tests = 0;
  uint64_t mask_tests = 0;
  uint64_t endpoint_tests = 0;
  uint64_t intersection_tests = 0;
  double intersection_ms = 0;
  double visibility_ms = 0;
  double connection_ms = 0;
};

struct Result {
  int2 extent = int2(1);
  std::vector<Segment> segments;
  std::vector<Stroke> strokes;
  Statistics statistics;
  bool cancelled = false;
};

std::vector<IntersectionEdge> find_intersections(const Geometry &geometry,
                                                Statistics &statistics,
                                                const std::atomic<bool> &cancel);
Result evaluate(const Geometry &geometry,
                const Camera &camera,
                const Plan &plan,
                const std::vector<Rule> &rules,
                const std::vector<IntersectionEdge> &intersections,
                const std::atomic<bool> &cancel,
                bool connect = true);
void connect_strokes(Result &result,
                     const std::vector<Rule> &rules,
                     const std::atomic<bool> &cancel);

}  // namespace blender::eevee::geometry_lines
