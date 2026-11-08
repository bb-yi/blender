/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "eevee_geometry_lines_geometry.hh"

namespace blender {
struct bNodeTree;
struct Collection;
struct Image;
struct Material;
struct Object;
namespace eevee::geometry_lines {

/** ID references are resolved on the render thread before making an immutable worker snapshot. */
struct ObjectScope {
  int mode = 0;
  Collection *collection = nullptr;
  Collection *exclude_collection = nullptr;
  std::vector<Object *> objects;
  std::vector<Object *> excluded;
  bool children = true;
  bool instances = true;
  bool repair_seams = false;
};

struct RuleScope {
  std::shared_ptr<ObjectScope> objects;
  std::vector<blender::Material *> materials;
  bool exclude_materials = false;
};

struct Brush {
  float4 color = float4(0, 0, 0, 1);
  float width = 2, opacity = 1;
  float spacing = 0, stretch = 1, angle = 0, softness = 0;
  float dot_size = 16, dot_size_random = 0.8f, dot_antialiasing = 0.5f;
  float dot_space_x = 0.1f, dot_space_y = 0.1f, dot_space_random_x = 1, dot_space_random_y = 1;
  float tip_reduction_start = 1, tip_reduction_end = 1, groove = 0;
  int groove_count = 5;
  float taper_start = 0, taper_end = 0, alpha_start = 0, alpha_end = 0;
  float dash_length = 8, dash_space = 5, dash_phase = 0;
  float width_random = 0, angle_random = 0, grain = 0;
  float extend = 0, offset = 0, shift_x = 0, shift_y = 0, wave = 0, cycles = 3, noise = 0;
  float wave_length = 100, wave_phase = 0, wave_random = 0, wave_period_random = 0, wave_phase_random = 0;
  float offset_random = 0;
  bool wave_pixels = false;
  int mode = 0, copies = 1, seed = 0;
  int stroke_mode = 0;
  bool dashed = false;
  bool width_curve_enabled = false, opacity_curve_enabled = false;
  bool raster_size_response = false;
  bool override_connection = false;
  StrokeConnection connection;
  int width_map = -1;
  float width_map_strength = 1;
  int color_map = -1;
  float color_map_strength = 1;
  int brush_map = -1, distortion_map = -1;
  float brush_map_opacity = 1, distortion_map_amount = 5;
  std::array<float2, 256> curve;
  blender::Image *image = nullptr;
  uint64_t identity = 0;

  bool procedural_tip() const
  {
    return mode != 0 && (mode == 2 || groove > 0 || tip_reduction_start < 1 || tip_reduction_end < 1);
  }
};

struct OcclusionRule {
  RuleScope scope;
  int mode = 0;
  blender::Image *image = nullptr;
  std::string uv_map;
  float threshold = 0.5f;
};

struct Reduction {
  float start = 1, end = 10;
  bool use_object = false;
  blender::Object *object = nullptr;
  std::array<float, 256> curve;
};

struct TextureMap {
  blender::Image *image = nullptr;
  UVMapKey uv;
  int coordinates = 0; /* Surface UV / top-left screen UV / mesh color attribute. */
  int wrap_u = 0, wrap_v = 0, filter = 1;
  float2 tiling = float2(1), offset = float2(0);
  float rotation = 0;
};

struct Graph {
  Plan plan;
  std::vector<Rule> rules;
  std::vector<RuleScope> scopes;
  std::vector<Brush> brushes;
  std::vector<OcclusionRule> occlusion;
  std::vector<Reduction> reductions;
  std::vector<TextureMap> maps;
  bool composite = true;
  bool relative_width = false;
  bool depth_of_field = false;
  bool motion_blur = false;
  float width_scale = 1;
  float reference_width = 1920;
  float antialiasing = 1;
  float offscreen_margin = 0;
  int samples = 1;
  int preview = 0;
  uint64_t visibility_signature = 0;
  uint64_t connection_signature = 0;
  uint64_t paint_signature = 0;
  std::string error;
};

Graph compile_graph(const bNodeTree &tree, int solo_node = 0);

}  // namespace eevee::geometry_lines
}  // namespace blender
