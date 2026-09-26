/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_principled_npr_v2.hh"

#include "BKE_colorband.hh"
#include "DNA_color_types.h"
#include "DNA_material_types.h"
#include "GPU_material.hh"
#include "MEM_guardedalloc.h"
#include "node_shader_util.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>

namespace blender::nodes::node_shader_principled_npr_cc {

static GPUNodeLink *input_link(bNode &node, GPUNodeStack *inputs, const char *identifier)
{
  return GPU_node_get_input_link(node, inputs, identifier);
}

static GPUNodeLink *pack4(
    GPUMaterial *mat, GPUNodeLink *x, GPUNodeLink *y, GPUNodeLink *z, GPUNodeLink *w)
{
  GPUNodeLink *result = nullptr;
  GPU_link(mat, "npr_v2_pack4", x, y, z, w, &result);
  return result;
}

static GPUNodeLink *pack_matrix(GPUMaterial *mat, const std::array<GPUNodeLink *, 4> &columns)
{
  GPUNodeLink *result = nullptr;
  GPU_link(mat, "npr_v2_pack_matrix", columns[0], columns[1], columns[2], columns[3], &result);
  return result;
}

static double response_curve(double value, double c)
{
  if (c <= 1e-8) {
    return value < 0.5 ? 0.0 : value > 0.5 ? 1.0 : 0.5;
  }
  const double t = std::clamp(
      std::sin(std::asin(std::clamp(2.0 * value - 1.0, -1.0, 1.0)) / 3.0) / c + 0.5, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

static double calibration_beta(double softness)
{
  const double c = softness * softness * softness;
  if (c < 1e-8) {
    return 0.5;
  }
  if (softness >= 1.0) {
    return 0.71339675;
  }
  const auto decode = [c](double y) { return y / ((1.0 - y) + (1.0 - c) * y); };
  double lo = 0.4;
  double hi = 1.0;
  for (int iteration = 0; iteration < 64; iteration++) {
    const double beta = (lo + hi) * 0.5;
    const double ratio = decode(response_curve(0.5 / (0.5 + beta), c)) /
                         decode(response_curve(1.0 / (1.0 + beta), c));
    if (ratio > 0.5) {
      lo = beta;
    }
    else {
      hi = beta;
    }
  }
  return (lo + hi) * 0.5;
}

static GPUNodeLink *calibration_table(GPUMaterial *mat, float &layer)
{
  /* The shared ramp texture is RGBA16F. A half-grid high part and scaled residual
   * preserve the calibration precision needed by sharp transitions. */
  static const std::array<std::array<float, 4>, CM_TABLE + 1> table = [] {
    std::array<std::array<float, 4>, CM_TABLE + 1> data{};
    for (int i = 0; i <= CM_TABLE; i++) {
      const double beta = calibration_beta(double(i) / CM_TABLE);
      const double grid = beta < 0.5 ? 4096.0 : 2048.0;
      const double high = std::round(beta * grid) / grid;
      data[i] = {float(high), float((beta - high) * 65536.0), 0.0f, 1.0f};
    }
    return data;
  }();
  float *pixels = MEM_new_array_uninitialized<float>((CM_TABLE + 1) * 4, __func__);
  std::memcpy(pixels, table.data(), sizeof(table));
  return GPU_color_band(mat, CM_TABLE + 1, pixels, &layer);
}

int node_gpu_v2(GPUMaterial *mat, bNode *node, GPUNodeStack *in, GPUNodeStack *out)
{
  const auto &storage = *static_cast<const NodeShaderPrincipledNPR *>(node->storage);
  const bool has_shader = out[0].hasoutput;
  const bool has_color = out[1].hasoutput;
  if (!has_shader && !has_color) {
    return GPU_link(mat, "npr_v2_alpha", input_link(*node, in, "alpha"), &out[2].link);
  }

  /* GPU_constant/uniform keep a pointer until GPU_link consumes it. Stable local storage
   * is essential: returning a link to a temporary scalar corrupts packed mode fields. */
  std::deque<float> constant_storage;
  auto constant = [&](float value) {
    constant_storage.push_back(value);
    return GPU_constant(&constant_storage.back());
  };
  auto socket = [&](const char *id) {
    const GPUNodeStack &input = GPU_node_get_input(*node, in, id);
    if (!input.link && input.type == GPU_FLOAT) {
      /* Match native Principled's disabled-lobe pruning. Keep ordinary values
       * uniform, so dragging within an enabled range reuses the same shader. */
      if (input.vec[0] == 0.0f) {
        for (const char *disabled : {"metallic", "transmission_weight", "subsurface_weight",
                                    "coat_weight", "sheen_weight", "thin_wall",
                                    "metallic_body_preservation", "emission_strength",
                                    "rim_strength", "highlight_strength", "profile_offset",
                                    "flatten_strength"})
        {
          if (std::strcmp(id, disabled) == 0) {
            return constant(0.0f);
          }
        }
      }
      if (input.vec[0] == 1.0f &&
          (std::strcmp(id, "profile_softness") == 0 || std::strcmp(id, "alpha") == 0 ||
           std::strcmp(id, "metallic") == 0))
      {
        return constant(1.0f);
      }
    }
    return input_link(*node, in, id);
  };
  auto control = [&](const char *a, const char *b, const char *c, const char *d) {
    return pack4(mat, socket(a), socket(b), socket(c), socket(d));
  };
  auto nonzero = [&](const char *id) {
    const GPUNodeStack &s = GPU_node_get_input(*node, in, id);
    return s.link || s.socket_not_zero();
  };
  eGPUMaterialNPRFeature features = GPU_MAT_NPR_FEATURE_NONE;
  const GPUNodeStack &highlight_strength = GPU_node_get_input(*node, in, "highlight_strength");
  /* The shader uses an exact positive test, not socket_not_zero's near-zero threshold. */
  if (highlight_strength.link || highlight_strength.vec[0] > 0.0f) {
    if (storage.highlight_light_shape == SHD_PRINCIPLED_NPR_HIGHLIGHT_INTEGRATED) {
      features |= GPU_MAT_NPR_FINITE_HIGHLIGHT;
    }
    const GPUNodeStack &softness = GPU_node_get_input(*node, in, "profile_softness");
    const GPUNodeStack &offset = GPU_node_get_input(*node, in, "profile_offset");
    if (softness.link || softness.vec[0] != 1.0f || offset.link || offset.vec[0] != 0.0f ||
        storage.specular_mapping == SHD_PRINCIPLED_NPR_SPECULAR_RAMP)
    {
      features |= GPU_MAT_NPR_REFERENCE_HIGHLIGHT;
    }
  }
  const Material *blender_material = GPU_material_get_material(mat);
  const bool depth_rim_supported = blender_material == nullptr ||
      blender_material->surface_render_method != MA_SURFACE_METHOD_FORWARD;
  /* Resource lifetime follows the mode, not a changing uniform threshold.
   * Width/Mask 0 -> nonzero must not reuse a pass compiled without HiZ. */
  if (ELEM(storage.rim_mode, SHD_PRINCIPLED_NPR_RIM_SCREEN_DEPTH,
           SHD_PRINCIPLED_NPR_RIM_GOO_DEPTH) && depth_rim_supported)
  {
    GPU_material_hiz_data_set(mat);
    if (storage.rim_mode == SHD_PRINCIPLED_NPR_RIM_SCREEN_DEPTH) {
      /* The legacy contour search needs object IDs. Goo only reads depth. */
      GPU_material_flag_set(mat, GPU_MATFLAG_RAYCAST);
    }
  }

  if (storage.shadow_mode != SHD_PRINCIPLED_NPR_SHADOW_NONE) {
    switch (storage.shadow_quality) {
      case SHD_SHADER_INFO_SHADOW_STABLE:
        features |= GPU_MAT_NPR_SHADOW_STABLE;
        break;
      case SHD_SHADER_INFO_SHADOW_TEMPORAL:
        features |= GPU_MAT_NPR_SHADOW_TEMPORAL;
        break;
      case SHD_SHADER_INFO_SHADOW_SOFT_FILTERED:
        features |= GPU_MAT_NPR_SHADOW_SOFT;
        break;
    }
  }
  /* Version >= 2 feeds constant(0) into shading[1].y, so it can never share energy. */
  if (storage.energy_response_version == 1 &&
      storage.mapping_stage == SHD_PRINCIPLED_NPR_MAPPING_COMBINED &&
      storage.light_combine != SHD_PRINCIPLED_NPR_LIGHT_STRONGEST)
  {
    features |= GPU_MAT_NPR_SHARED_ENERGY;
  }
  if (depth_rim_supported) {
    if (storage.rim_mode == SHD_PRINCIPLED_NPR_RIM_SCREEN_DEPTH) {
      features |= GPU_MAT_NPR_RIM_SCREEN_DEPTH;
    }
    else if (storage.rim_mode == SHD_PRINCIPLED_NPR_RIM_GOO_DEPTH) {
      features |= GPU_MAT_NPR_RIM_GOO_DEPTH;
    }
  }
  switch (storage.mapping_stage) {
    case SHD_PRINCIPLED_NPR_MAPPING_PER_LIGHT:
      features |= GPU_MAT_NPR_MAP_PER_LIGHT;
      break;
    case SHD_PRINCIPLED_NPR_MAPPING_COMBINED:
      features |= GPU_MAT_NPR_MAP_COMBINED;
      break;
    default:
      features |= GPU_MAT_NPR_MAP_TOTAL;
      break;
  }
  if (storage.diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_DRIVEN_RAMP) {
    features |= GPU_MAT_NPR_DRIVEN_RAMP;
  }
  /* OR across nodes; Local Color needs this code too, but Alpha-only returned above. */
  GPU_material_npr_features_add(mat, features);

  GPUNodeStack &normal = GPU_node_get_input(*node, in, "normal");
  if (!normal.link) {
    GPU_link(mat, "world_normals_get", &normal.link);
  }
  GPUNodeStack &coat_normal = GPU_node_get_input(*node, in, "coat_normal");
  if (!coat_normal.link) {
    GPU_link(mat, "world_normals_get", &coat_normal.link);
  }
  GPUNodeStack &tangent = GPU_node_get_input(*node, in, "tangent");
  if (!tangent.link) {
    GPU_link(mat,
             "npr_v2_default_tangent",
             GPU_attribute(mat, CD_TANGENT, ""),
             GPU_attribute(mat, CD_ORCO, ""),
             normal.link,
             &tangent.link);
  }

  GPU_material_flag_set(mat, GPU_MATFLAG_GLSL_LIGHT_ACCESS);
  if (storage.shadow_mode == SHD_PRINCIPLED_NPR_SHADOW_CAST_ONLY ||
      storage.shadow_mode == SHD_PRINCIPLED_NPR_SHADOW_SELF_ONLY)
  {
    GPU_material_shader_info_shadow_classification_set(mat);
  }
  if (storage.shadow_mode != SHD_PRINCIPLED_NPR_SHADOW_NONE &&
      storage.shadow_quality == SHD_SHADER_INFO_SHADOW_SOFT_FILTERED)
  {
    GPU_material_flag_set(mat, GPU_MATFLAG_RAYCAST);
    GPU_material_hiz_data_set(mat);
  }
  if (has_shader) {
    GPU_material_principled_npr_v2_set(mat);
    eGPUMaterialFlag flags = GPU_MATFLAG_DIFFUSE | GPU_MATFLAG_GLOSSY;
    if (nonzero("transmission_weight")) {
      flags |= GPU_MATFLAG_REFRACT | GPU_MATFLAG_REFRACTION_MAYBE_COLORED;
    }
    if (nonzero("subsurface_weight")) {
      flags |= GPU_MATFLAG_SUBSURFACE | GPU_MATFLAG_TRANSLUCENT;
    }
    if (nonzero("coat_weight")) {
      flags |= GPU_MATFLAG_COAT;
    }
    const GPUNodeStack &alpha = GPU_node_get_input(*node, in, "alpha");
    if (alpha.link || alpha.socket_not_one()) {
      flags |= GPU_MATFLAG_TRANSPARENT;
    }
    if (nonzero("emission_strength")) {
      flags |= GPU_MATFLAG_EMISSION;
    }
    flags |= GPU_MATFLAG_REFLECTION_MAYBE_COLORED;
    GPU_material_flag_set(mat, flags);
  }

  std::array<GPUNodeLink *, 32> colors{};
  std::array<GPUNodeLink *, 32> points{};
  std::array<std::array<float, 4>, 32> point_data{};
  const float white[4] = {1, 1, 1, 1};
  const float inactive[4] = {1, 0, 1, 0};
  int count = 2;
  if (storage.diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_SIMPLE) {
    colors[0] = socket("shadow_color");
    colors[1] = socket("lit_color");
    point_data[0] = {0, 0, 1, 0};
    point_data[1] = {1, 0, 1, 1};
    points[0] = GPU_constant(point_data[0].data());
    points[1] = GPU_constant(point_data[1].data());
  }
  else if (storage.diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_DRIVEN_RAMP) {
    count = std::clamp(int(storage.driven_stop_count), 2, 8);
    for (int i = 0; i < 8; i++) {
      colors[i] = socket(("stop_color_" + std::to_string(i)).c_str());
      points[i] = pack4(mat,
                        socket(("stop_position_" + std::to_string(i)).c_str()),
                        socket(("stop_offset_" + std::to_string(i)).c_str()),
                        socket(("stop_softness_" + std::to_string(i)).c_str()),
                        constant(float(i)));
    }
  }
  else {
    std::array<const NodePrincipledNPRRampPoint *, 32> active{};
    count = 0;
    for (const NodePrincipledNPRRampPoint &point : storage.ramp_points) {
      if (point.enabled && point.initialized) {
        active[count++] = &point;
      }
    }
    std::sort(active.begin(), active.begin() + count, [](const auto *a, const auto *b) {
      const float pa = std::clamp(a->position, 0.0f, 1.0f);
      const float pb = std::clamp(b->position, 0.0f, 1.0f);
      return pa != pb ? pa < pb : a->identifier < b->identifier;
    });
    for (int i = 0; i < count; i++) {
      const auto &point = *active[i];
      point_data[i] = {point.position, point.offset, point.softness, float(i)};
      colors[i] = GPU_uniform(point.color);
      points[i] = GPU_uniform(point_data[i].data());
    }
    count = std::max(count, 2);
  }
  for (int i = 0; i < 32; i++) {
    if (colors[i] == nullptr) {
      colors[i] = GPU_constant(white);
    }
    if (points[i] == nullptr) {
      points[i] = GPU_constant(inactive);
    }
  }
  std::array<GPUNodeLink *, 8> color_matrices;
  std::array<GPUNodeLink *, 8> point_matrices;
  for (int i = 0; i < 8; i++) {
    color_matrices[i] = pack_matrix(
        mat, {colors[4 * i], colors[4 * i + 1], colors[4 * i + 2], colors[4 * i + 3]});
    point_matrices[i] = pack_matrix(
        mat, {points[4 * i], points[4 * i + 1], points[4 * i + 2], points[4 * i + 3]});
  }

  GPUNodeLink *radius = nullptr;
  GPU_link(mat,
           "npr_v2_pack_vector",
           socket("subsurface_radius"),
           socket("metallic_body_preservation"),
           &radius);
  GPUNodeLink *material = pack_matrix(
      mat,
      {control("metallic", "roughness", "alpha", "ior"),
       control("transmission_weight", "subsurface_weight", "subsurface_scale", "thin_wall"),
       radius,
       pack4(mat,
             socket("ambient_strength"),
             socket("reflection_strength"),
             constant(0),
             constant(0))});
  GPUNodeLink *surface = pack_matrix(
      mat,
      {control("coat_weight", "coat_roughness", "coat_ior", "sheen_weight"),
       pack4(mat, socket("sheen_roughness"), socket("emission_strength"),
             storage.energy_response_version >= 2 ? socket("flatten_strength") : constant(0),
             constant(0)),
       socket("emission_color"),
       GPU_uniform(inactive)});
  GPUNodeLink *shading = pack_matrix(
      mat,
      {pack4(mat,
             socket("coordinate_scale"),
             socket("coordinate_offset"),
             socket("mapping_softness"),
             constant(storage.energy_response_version)),
       pack4(mat, socket("direct_strength"),
             storage.energy_response_version >= 2 ? constant(0) :
                 socket(storage.energy_response_version >= 1 ? "energy_influence" :
                                                              "intensity_influence"),
             socket("light_color_influence"), socket("shadow_strength")),
       pack4(mat,
             constant(storage.coordinate_range),
             constant(storage.color_application),
             constant(storage.mapping_stage),
             constant(storage.light_combine)),
       pack4(mat,
             constant(storage.shadow_mode),
             constant(storage.shadow_quality),
             constant(storage.shadow_samples),
             constant(storage.use_all_lights ? -1.0f : float(storage.lightgroup_id)))});
  GPUNodeLink *highlight = pack_matrix(mat,
                                       {socket("highlight_color"),
                                        control("highlight_strength",
                                                "profile_softness",
                                                "profile_offset",
                                                "highlight_light_color_influence"),
                                        pack4(mat,
                                              socket("anisotropy"),
                                              socket("anisotropy_rotation"),
                                              constant(storage.specular_mapping),
                                              constant(storage.highlight_light_shape)),
                                        GPU_uniform(inactive)});
  GPUNodeLink *rim_color = nullptr;
  GPU_link(mat, "npr_v2_pack_color", socket("rim_color"), socket("rim_strength"), &rim_color);
  GPUNodeLink *rim_shape = nullptr;
  GPUNodeLink *rim_depth = nullptr;
  if (storage.rim_mode == SHD_PRINCIPLED_NPR_RIM_GOO_DEPTH) {
    GPU_link(mat, "npr_v2_pack_vector", socket("goo_rim_scale"),
             socket("goo_rim_samples"), &rim_shape);
    rim_depth = pack4(mat, socket("goo_rim_radius"), socket("goo_rim_thickness"),
                      constant(0), constant(depth_rim_supported ? 1 : 0));
  }
  else {
    rim_shape = control("rim_angle", "rim_length", "rim_length_falloff", "rim_thickness");
    rim_depth = pack4(mat, socket("rim_pixel_width"), socket("rim_depth_threshold"),
                      socket("rim_depth_softness"), constant(depth_rim_supported ? 1 : 0));
  }
  GPUNodeLink *rim = pack_matrix(
      mat,
      {rim_color,
       rim_shape,
       pack4(mat,
             socket("rim_thickness_falloff"),
             socket("rim_light_bias"),
             socket("rim_mask"),
             constant(storage.rim_mode)),
       rim_depth});

  float beta_layer;
  GPUNodeLink *table = calibration_table(mat, beta_layer);
  float *specular_data;
  int specular_size;
  float specular_layer;
  BKE_colorband_evaluate_table_rgba(&storage.specular_ramp, &specular_data, &specular_size);
  GPUNodeLink *specular_table = GPU_color_band(mat, specular_size, specular_data, &specular_layer);
  const float table_data[4] = {
      beta_layer,
      specular_layer,
      float(count),
      storage.mapping_interpolation == SHD_PRINCIPLED_NPR_INTERP_LINEAR ? 1.0f : 0.0f};
  GPUNodeLink *execution = pack4(
      mat, socket("Weight"), constant(has_shader ? 1 : 0),
      constant(storage.diffuse_mapping != SHD_PRINCIPLED_NPR_DIFFUSE_DRIVEN_RAMP ? 1 : 0),
      /* Keep the feature defines in the GPUPass graph hash, including Local Color-only graphs. */
      constant(float(features)));
  return GPU_link(mat,
                  "node_principled_npr_v2",
                  socket("base_color"),
                  normal.link,
                  tangent.link,
                  coat_normal.link,
                  material,
                  surface,
                  shading,
                  highlight,
                  rim,
                  socket("coat_tint"),
                  socket("sheen_tint"),
                  color_matrices[0],
                  color_matrices[1],
                  color_matrices[2],
                  color_matrices[3],
                  color_matrices[4],
                  color_matrices[5],
                  color_matrices[6],
                  color_matrices[7],
                  point_matrices[0],
                  point_matrices[1],
                  point_matrices[2],
                  point_matrices[3],
                  point_matrices[4],
                  point_matrices[5],
                  point_matrices[6],
                  point_matrices[7],
                  table,
                  specular_table,
                  GPU_constant(table_data),
                  execution,
                  &out[0].link,
                  &out[1].link,
                  &out[2].link);
}

}  // namespace blender::nodes::node_shader_principled_npr_cc
