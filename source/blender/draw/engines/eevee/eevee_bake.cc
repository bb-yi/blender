/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Eevee color baking entry point.
 *
 * This intentionally does not evaluate shader nodes on the CPU. The only supported path is:
 * generic bake UV/pixel setup -> Eevee bake callback -> DRW UV-space raster pass -> Combined.
 */

#include "eevee_bake.hh"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <string>

#include "BLI_array.hh"
#include "BLI_color.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_map.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "DNA_customdata_types.h"
#include "DNA_material_types.h"
#include "DNA_camera_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_tangent.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_mesh.hh"

#include "GPU_batch.hh"
#include "GPU_context.hh"
#include "GPU_material.hh"
#include "GPU_pass.hh"
#include "GPU_texture.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"

#include "NOD_shader.h"

#include "RE_bake.h"
#include "RE_engine.h"
#include "RE_pipeline.h"
#include "render_types.h"

#include "DRW_engine.hh"
#include "DRW_gpu_wrapper.hh"
#include "DRW_render.hh"
#include "draw_handle.hh"
#include "draw_manager.hh"
#include "draw_pass.hh"
#include "draw_view.hh"

#include "eevee_instance.hh"

#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"

namespace blender::eevee {

namespace {

constexpr int eevee_bake_supported_pass_type = SCE_PASS_EMIT;
constexpr int eevee_bake_max_accumulated_samples = 64;
constexpr int64_t eevee_bake_full_sample_pixel_limit = int64_t(1024) * 1024;
constexpr int64_t eevee_bake_medium_sample_pixel_limit = int64_t(4096) * 4096;

struct BakeBatchAttribute {
  const GPUMaterialAttribute *gpu_attr = nullptr;
  uint format_attr = uint(-1);
  int tangent_layer = -1;
};

struct BakeBatchTangentLayer {
  std::string uv_name;
  Array<float4> values;
};

struct BakeDrawGroup {
  blender::Material *material = nullptr;
  GPUMaterial *gpumat = nullptr;
  gpu::Batch *batch = nullptr;
};

struct BakeTriData {
  Array<int3> corner_tris;
  Array<int> tri_faces;
};

struct BakePrimitiveMask {
  Array<int> primitive_ids;
  bool has_valid_pixels = false;
};

struct ScopedBakeCamera {
  Object *object = nullptr;
  blender::Camera *data = nullptr;

  ~ScopedBakeCamera()
  {
    if (object != nullptr) {
      object->data = nullptr;
      BKE_id_free(nullptr, object);
    }
    if (data != nullptr) {
      BKE_id_free(nullptr, data);
    }
  }
};

struct BakeWorldBounds {
  float3 center = float3(0.0f);
  float3 extent = float3(1.0f);
  float radius = 1.0f;
};

struct BakeLocalBounds {
  float3 center = float3(0.0f);
  float3 extent = float3(1.0f);
  float radius = 1.0f;
};

struct ScopedBakeRenderBuffers {
  Instance &inst;
  bool acquired = false;

  ScopedBakeRenderBuffers(Instance &inst, const int2 extent) : inst(inst)
  {
    inst.render_buffers.acquire(extent);
    acquired = true;
  }

  ~ScopedBakeRenderBuffers()
  {
    if (acquired) {
      inst.render_buffers.release();
    }
  }
};

static void eevee_bake_report_error(RenderEngine *engine, const std::string &message)
{
  RE_engine_report(engine, RPT_ERROR, message.c_str());
  RE_engine_set_error_message(engine, message.c_str());
}

static const char *material_name(const blender::Material *material)
{
  return material ? material->id.name + 2 : "<None>";
}

static BakeWorldBounds compute_bake_world_bounds(const Object &object, const Mesh &mesh)
{
  BakeWorldBounds bounds;
  const Span<float3> positions = mesh.vert_positions();
  if (positions.is_empty()) {
    bounds.center = object.object_to_world().location();
    return bounds;
  }

  float3 min = float3(FLT_MAX);
  float3 max = float3(-FLT_MAX);
  for (const float3 &position : positions) {
    const float3 world_position = math::transform_point(object.object_to_world(), position);
    math::min_max(world_position, min, max);
  }

  bounds.center = (min + max) * 0.5f;
  bounds.extent = math::max(max - min, float3(0.001f));
  bounds.radius = std::max(math::length(bounds.extent) * 0.5f, 0.5f);
  return bounds;
}

static BakeLocalBounds compute_bake_local_bounds(const Mesh &mesh)
{
  BakeLocalBounds bounds;
  const Span<float3> positions = mesh.vert_positions();
  if (positions.is_empty()) {
    return bounds;
  }

  float3 min = float3(FLT_MAX);
  float3 max = float3(-FLT_MAX);
  for (const float3 &position : positions) {
    math::min_max(position, min, max);
  }

  bounds.center = (min + max) * 0.5f;
  bounds.extent = math::max(max - min, float3(0.001f));
  bounds.radius = std::max(math::length(bounds.extent) * 0.5f, 0.5f);
  return bounds;
}

static void configure_bake_camera(ScopedBakeCamera &bake_camera,
                                  const Object &object,
                                  const Mesh &mesh)
{
  const BakeWorldBounds bounds = compute_bake_world_bounds(object, mesh);
  const float ortho_scale = std::max(bounds.radius * 2.5f, 1.0f);
  const float clip_end = std::max(bounds.radius * 6.0f + 4.0f, 10.0f);
  const float3 camera_backward = math::normalize(float3(0.45f, -0.65f, 1.0f));
  const float3 camera_location = bounds.center + camera_backward * (bounds.radius * 2.5f + 2.0f);
  const float3 camera_right = math::normalize(math::cross(float3(0.0f, 1.0f, 0.0f),
                                                         camera_backward));
  const float3 camera_up = math::cross(camera_backward, camera_right);

  float4x4 camera_to_world = float4x4::identity();
  camera_to_world.x_axis() = camera_right;
  camera_to_world.y_axis() = camera_up;
  camera_to_world.z_axis() = camera_backward;
  camera_to_world.location() = camera_location;

  bake_camera.object->loc[0] = camera_location.x;
  bake_camera.object->loc[1] = camera_location.y;
  bake_camera.object->loc[2] = camera_location.z;
  bake_camera.object->runtime->object_to_world = camera_to_world;
  bake_camera.object->runtime->world_to_object = math::invert(camera_to_world);

  bake_camera.data->type = CAM_ORTHO;
  bake_camera.data->ortho_scale = ortho_scale;
  bake_camera.data->clip_start = 0.01f;
  bake_camera.data->clip_end = clip_end;
}

static Object *scene_render_camera_get(RenderEngine *engine)
{
  if (engine == nullptr || engine->re == nullptr) {
    return nullptr;
  }

  Object *camera = RE_GetCamera(engine->re);
  if (camera != nullptr && camera->type == OB_CAMERA) {
    return camera;
  }
  return nullptr;
}

static std::string node_display_name(const bNode &node)
{
  if (node.label[0] != '\0') {
    return node.label;
  }
  if (node.name[0] != '\0') {
    return node.name;
  }
  return node.idname;
}

static bool image_dimensions_match(const RenderEngine *engine, const int width, const int height)
{
  if (engine->bake.targets == nullptr) {
    return false;
  }
  const int image_id = engine->bake.image_id;
  if (image_id < 0 || image_id >= engine->bake.targets->images_num) {
    return false;
  }
  const BakeImage &image = engine->bake.targets->images[image_id];
  return image.width == width && image.height == height;
}

static std::string bake_uv_layer_name(const RenderEngine *engine, const Mesh &mesh)
{
  if (engine->bake.uv_layer[0] != '\0') {
    return engine->bake.uv_layer;
  }
  return mesh.active_or_default_uv_map_name();
}

static bool bake_target_is_color_attribute(const BakeImage &image)
{
  return image.image == nullptr;
}

static bool validate_bake_request(RenderEngine *engine,
                                  Object *object,
                                  const int pass_type,
                                  const int width,
                                  const int height)
{
  if (pass_type != eevee_bake_supported_pass_type) {
    eevee_bake_report_error(engine, "Eevee Color Bake only supports the Emit bake type");
    return false;
  }
  if (object == nullptr || object->type != OB_MESH) {
    eevee_bake_report_error(engine, "Eevee Color Bake only supports mesh objects");
    return false;
  }
  if (width <= 0 || height <= 0) {
    eevee_bake_report_error(engine, "Eevee Color Bake requires a non-empty image target");
    return false;
  }
  if (engine->bake.targets == nullptr || engine->bake.pixels == nullptr ||
      engine->bake.result == nullptr)
  {
    eevee_bake_report_error(engine, "Eevee Color Bake requires image bake targets");
    return false;
  }
  if (engine->bake.targets->channels_num != 4) {
    eevee_bake_report_error(engine, "Eevee Color Bake requires RGBA bake targets");
    return false;
  }
  if (!image_dimensions_match(engine, width, height)) {
    eevee_bake_report_error(engine, "Eevee Color Bake image dimensions do not match bake target");
    return false;
  }
  if (engine->re != nullptr && engine->re->scene != nullptr) {
    const BakeData &bake = engine->re->scene->r.bake;
    if (!ELEM(bake.target, R_BAKE_TARGET_IMAGE_TEXTURES, R_BAKE_TARGET_VERTEX_COLORS)) {
      eevee_bake_report_error(
          engine,
          "Eevee Color Bake only supports Image Textures or Active Color Attribute targets");
      return false;
    }
    if (bake.flag & R_BAKE_TO_ACTIVE) {
      eevee_bake_report_error(engine, "Eevee Color Bake does not support Selected to Active");
      return false;
    }
    if (bake.flag & R_BAKE_CAGE) {
      eevee_bake_report_error(engine, "Eevee Color Bake does not support cage baking");
      return false;
    }
  }
  return true;
}

static const char *unsupported_bake_feature_for_node(const bNode &node)
{
  switch (node.type_legacy) {
    case SH_NODE_NPR_INPUT:
      return "NPR Input screen/GBuffer reads";
    case SH_NODE_NPR_IMAGE_SAMPLE:
      return "NPR Image Sample";
    case SH_NODE_NPR_REFRACTION:
      return "NPR Refraction";
    case SH_NODE_INPUT_AOV:
      return "Input AOV";
    case SH_NODE_OUTPUT_AOV:
      return "Output AOV";
    case SH_NODE_OUTPUT_FILTER:
      return "Filter-domain output";
    case SH_NODE_RENDER_TEXTURE:
      return "Render Texture feedback";
    case SH_NODE_SCENE_COLOR:
      return "Scene Color";
    case SH_NODE_SCREENSPACE_INFO:
      return "Screen Space Info";
    case SH_NODE_AMBIENT_OCCLUSION:
      return "Ambient Occlusion screen-space sampling";
    case SH_NODE_BEVEL:
      return "Bevel screen-space raycast";
    case SH_NODE_CURVATURE:
      return "Curvature screen-space depth sampling";
    case SH_NODE_RAYCAST:
      return "Raycast screen-space tracing";
    case SH_NODE_BSDF_TRANSPARENT:
      return "Transparent BSDF layering";
    case SH_NODE_BSDF_RAY_PORTAL:
      return "Ray Portal BSDF";
    case SH_NODE_SHADER_INFO:
      if (node.custom1 == SHD_SHADER_INFO_SHADOW_SOFT_FILTERED) {
        return "Shader Info Soft Filtered shadows";
      }
      break;
    default:
      break;
  }
  return nullptr;
}

static const bNode *find_active_output_node(const bNodeTree &ntree, const int output_type)
{
  for (const bNode *node : ntree.all_nodes()) {
    if (node->type_legacy == output_type && (node->flag & NODE_DO_OUTPUT) &&
        !node->is_muted())
    {
      return node;
    }
  }
  for (const bNode *node : ntree.all_nodes()) {
    if (node->type_legacy == output_type && !node->is_muted()) {
      return node;
    }
  }
  return nullptr;
}

static bool node_tree_contains_unsupported_bake_dependency(
    const bNodeTree &ntree,
    const bNodeSocket *root_socket,
    Set<const bNodeTree *> &visited_trees,
    Map<const bNodeTree *, const bNode *> &group_node_by_tree,
    const bNode *&r_node,
    const char *&r_feature);

static bool node_tree_contains_direct_unsupported_bake_output_node(const bNodeTree &ntree,
                                                                   const int output_type,
                                                                   const char *feature,
                                                                   const bNode *&r_node,
                                                                   const char *&r_feature)
{
  for (const bNode *node : ntree.all_nodes()) {
    if (node->type_legacy == output_type && !node->is_muted()) {
      r_node = node;
      r_feature = feature;
      return true;
    }
  }
  return false;
}

static bool node_group_contains_unsupported_bake_side_effect(const bNodeTree &group_tree,
                                                             const bNode *&r_node,
                                                             const char *&r_feature)
{
  if (node_tree_contains_direct_unsupported_bake_output_node(
          group_tree, SH_NODE_OUTPUT_AOV, "Output AOV", r_node, r_feature))
  {
    return true;
  }
  if (node_tree_contains_direct_unsupported_bake_output_node(
          group_tree, SH_NODE_OUTPUT_FILTER, "Filter-domain output", r_node, r_feature))
  {
    return true;
  }
  return false;
}

static bool node_output_contains_unsupported_bake_dependency(
    const bNodeTree &ntree,
    const bNode *node,
    const bNodeSocket *output_socket,
    Set<const bNodeTree *> &visited_trees,
    Map<const bNodeTree *, const bNode *> &group_node_by_tree,
    const bNode *&r_node,
    const char *&r_feature)
{
  if (node == nullptr || node->is_muted()) {
    return false;
  }

  if (const char *feature = unsupported_bake_feature_for_node(*node)) {
    r_node = node;
    r_feature = feature;
    return true;
  }

  if (node->type_legacy == NODE_GROUP) {
    const bNodeTree *group_tree = reinterpret_cast<const bNodeTree *>(node->id);
    if (group_tree == nullptr || output_socket == nullptr) {
      return false;
    }
    group_tree->ensure_topology_cache();
    if (node_group_contains_unsupported_bake_side_effect(*group_tree, r_node, r_feature)) {
      return true;
    }
    const bNode *group_output_node = find_active_output_node(*group_tree, NODE_GROUP_OUTPUT);
    if (group_output_node == nullptr) {
      return false;
    }
    const bNodeSocket *group_output_input = group_output_node->input_by_identifier(
        output_socket->identifier);
    const bNode *previous_group_node = group_node_by_tree.lookup_default(group_tree, nullptr);
    group_node_by_tree.add_overwrite(group_tree, node);
    const bool found = node_tree_contains_unsupported_bake_dependency(
        *group_tree, group_output_input, visited_trees, group_node_by_tree, r_node, r_feature);
    if (previous_group_node != nullptr) {
      group_node_by_tree.add_overwrite(group_tree, previous_group_node);
    }
    else {
      group_node_by_tree.remove(group_tree);
    }
    return found;
  }

  if (node->type_legacy == NODE_GROUP_INPUT) {
    const bNode *group_node = group_node_by_tree.lookup_default(&ntree, nullptr);
    if (output_socket != nullptr && group_node != nullptr) {
      if (const bNodeSocket *group_input = group_node->input_by_identifier(
              output_socket->identifier))
      {
        const bNodeTree &owner_tree = group_node->owner_tree();
        owner_tree.ensure_topology_cache();
        for (const bNodeLink *link : group_input->directly_linked_links()) {
          if (!link->is_used()) {
            continue;
          }
          if (node_output_contains_unsupported_bake_dependency(
                  owner_tree,
                  link->fromnode,
                  link->fromsock,
                  visited_trees,
                  group_node_by_tree,
                  r_node,
                  r_feature))
          {
            return true;
          }
        }
      }
    }
    return false;
  }

  for (const bNodeSocket *socket : node->input_sockets()) {
    for (const bNodeLink *link : socket->directly_linked_links()) {
      if (!link->is_used()) {
        continue;
      }
      if (node_output_contains_unsupported_bake_dependency(
              ntree,
              link->fromnode,
              link->fromsock,
              visited_trees,
              group_node_by_tree,
              r_node,
              r_feature))
      {
        return true;
      }
    }
  }

  return false;
}

static bool node_tree_contains_unsupported_bake_dependency(const bNodeTree &ntree,
                                                           const bNodeSocket *root_socket,
                                                           Set<const bNodeTree *> &visited_trees,
                                                           Map<const bNodeTree *, const bNode *>
                                                               &group_node_by_tree,
                                                           const bNode *&r_node,
                                                           const char *&r_feature)
{
  if (root_socket == nullptr || visited_trees.contains(&ntree)) {
    return false;
  }

  ntree.ensure_topology_cache();
  visited_trees.add(&ntree);
  bool found = false;
  for (const bNodeLink *link : root_socket->directly_linked_links()) {
    if (!link->is_used()) {
      continue;
    }
    if (node_output_contains_unsupported_bake_dependency(
            ntree,
            link->fromnode,
            link->fromsock,
            visited_trees,
            group_node_by_tree,
            r_node,
            r_feature))
    {
      found = true;
      break;
    }
  }
  visited_trees.remove(&ntree);
  return found;
}

static bool output_node_inputs_contain_unsupported_bake_dependency(
    const bNodeTree &ntree,
    const bNode &output_node,
    Set<const bNodeTree *> &visited_trees,
    Map<const bNodeTree *, const bNode *> &group_node_by_tree,
    const bNode *&r_node,
    const char *&r_feature)
{
  for (const bNodeSocket *socket : output_node.input_sockets()) {
    if (node_tree_contains_unsupported_bake_dependency(
            ntree, socket, visited_trees, group_node_by_tree, r_node, r_feature))
    {
      return true;
    }
  }
  return false;
}

static bool material_tree_contains_unsupported_bake_node(const bNodeTree &ntree,
                                                         Set<const bNodeTree *> &visited_trees,
                                                         const bNode *&r_node,
                                                         const char *&r_feature)
{
  const bNode *output_node = find_active_output_node(ntree, SH_NODE_OUTPUT_MATERIAL);
  if (output_node == nullptr || output_node->input_sockets().is_empty()) {
    return false;
  }
  Map<const bNodeTree *, const bNode *> group_node_by_tree;
  return output_node_inputs_contain_unsupported_bake_dependency(
      ntree, *output_node, visited_trees, group_node_by_tree, r_node, r_feature);
}

static bool npr_tree_contains_unsupported_bake_node(const bNodeTree &ntree,
                                                    Set<const bNodeTree *> &visited_trees,
                                                    const bNode *&r_node,
                                                    const char *&r_feature)
{
  const bNode *output_node = find_active_output_node(ntree, SH_NODE_NPR_OUTPUT);
  if (output_node == nullptr || output_node->input_sockets().is_empty()) {
    return false;
  }
  Map<const bNodeTree *, const bNode *> group_node_by_tree;
  return output_node_inputs_contain_unsupported_bake_dependency(
      ntree, *output_node, visited_trees, group_node_by_tree, r_node, r_feature);
}

static bool validate_material_node_trees_for_bake(RenderEngine *engine, blender::Material *material)
{
  Set<const bNodeTree *> visited;
  const bNode *unsupported_node = nullptr;
  const char *unsupported_feature = nullptr;

  auto report_unsupported = [&]() {
    std::string message = "Eevee Color Bake does not support ";
    message += unsupported_feature;
    message += " in material \"";
    message += material_name(material);
    message += "\"";
    if (unsupported_node != nullptr) {
      message += " (node \"";
      message += node_display_name(*unsupported_node);
      message += "\")";
    }
    eevee_bake_report_error(engine, message);
  };

  if (material->eevee_domain == MA_EEVEE_DOMAIN_FILTER) {
    unsupported_node = (material->nodetree != nullptr) ?
                           find_active_output_node(*material->nodetree, SH_NODE_OUTPUT_FILTER) :
                           nullptr;
    unsupported_feature = "Filter-domain output";
    report_unsupported();
    return false;
  }

  if (material->nodetree != nullptr) {
    if (node_tree_contains_direct_unsupported_bake_output_node(*material->nodetree,
                                                               SH_NODE_OUTPUT_AOV,
                                                               "Output AOV",
                                                               unsupported_node,
                                                               unsupported_feature))
    {
      report_unsupported();
      return false;
    }

    if (node_tree_contains_direct_unsupported_bake_output_node(*material->nodetree,
                                                               SH_NODE_OUTPUT_FILTER,
                                                               "Filter-domain output",
                                                               unsupported_node,
                                                               unsupported_feature))
    {
      report_unsupported();
      return false;
    }

    if (material_tree_contains_unsupported_bake_node(
            *material->nodetree, visited, unsupported_node, unsupported_feature))
    {
      report_unsupported();
      return false;
    }
  }

  visited.clear();
  if (bNodeTree *npr_tree = npr_tree_get_from_mat(material)) {
    if (node_tree_contains_direct_unsupported_bake_output_node(*npr_tree,
                                                               SH_NODE_OUTPUT_AOV,
                                                               "Output AOV",
                                                               unsupported_node,
                                                               unsupported_feature))
    {
      report_unsupported();
      return false;
    }

    if (npr_tree_contains_unsupported_bake_node(
            *npr_tree, visited, unsupported_node, unsupported_feature))
    {
      report_unsupported();
      return false;
    }
  }
  return true;
}

static const char *unsupported_gpu_material_flag(const GPUMaterial *gpumat)
{
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SCENE_COLOR)) {
    return "Scene Color";
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SCREENSPACE_INFO)) {
    return "Screen Space Info";
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_RENDER_TEXTURE)) {
    return "Render Texture feedback";
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR_REFRACTION)) {
    return "NPR Refraction";
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_AOV)) {
    return "AOV input/output";
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_FILTER_MATERIAL)) {
    return "Filter-domain material";
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_RAYCAST)) {
    return "screen-space raycast/depth sampling";
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_AO)) {
    return "Ambient Occlusion screen-space sampling";
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT)) {
    return "transparent material layering";
  }
  return nullptr;
}

static bool validate_gpu_material_for_bake(RenderEngine *engine,
                                           const blender::Material *material,
                                           const GPUMaterial *gpumat)
{
  if (const char *unsupported_flag = unsupported_gpu_material_flag(gpumat)) {
    std::string message = "Eevee Color Bake does not support ";
    message += unsupported_flag;
    message += " in material \"";
    message += material_name(material);
    message += "\"";
    eevee_bake_report_error(engine, message);
    return false;
  }

  return true;
}

static Mesh *mesh_for_bake(Depsgraph *depsgraph, Object *object)
{
  Mesh *mesh = BKE_mesh_new_from_object(depsgraph, object, false, false, true);
  if (mesh == nullptr) {
    return nullptr;
  }
  if (mesh->normals_domain() == bke::MeshNormalDomain::Corner) {
    ED_mesh_split_faces(mesh);
  }
  mesh->corner_tris();
  mesh->corner_tri_faces();
  mesh->corner_normals();
  if (mesh->corner_edges().size() != mesh->corners_num) {
    bke::mesh_calc_edges(*mesh, true, false);
  }
  return mesh;
}

static BakeTriData bake_tri_data_from_mesh(const Mesh &mesh)
{
  BakeTriData data;
  const int tris_num = poly_to_tri_count(mesh.faces_num, mesh.corners_num);
  data.corner_tris = Array<int3>(tris_num);
  data.tri_faces = Array<int>(tris_num);
  bke::mesh::corner_tris_calc(
      mesh.vert_positions(), mesh.faces(), mesh.corner_verts(), data.corner_tris);
  bke::mesh::corner_tris_calc_face_indices(mesh.faces(), data.tri_faces);
  return data;
}

static int material_index_for_primitive(const Object &object,
                                        const int primitive_id,
                                        const Span<int> tri_faces,
                                        const VArraySpan<int> &material_indices)
{
  if (primitive_id < 0 || primitive_id >= tri_faces.size() || material_indices.is_empty()) {
    return 0;
  }
  const int face_i = tri_faces[primitive_id];
  if (face_i < 0 || face_i >= material_indices.size()) {
    return 0;
  }
  return clamp_i(material_indices[face_i], 0, max_ii(object.totcol - 1, 0));
}

static blender::Material *material_from_index(Object *object, const int material_index)
{
  blender::Material *material = BKE_object_material_get_eval(object, material_index + 1);
  return material ? material : BKE_material_default_surface();
}

static bool collect_image_primitives_by_material(RenderEngine *engine,
                                                 const BakeImage &image,
                                                 const Object &object,
                                                 const BakeTriData &tri_data,
                                                 const Mesh &mesh,
                                                 BakePrimitiveMask &r_primitive_mask,
                                                 Map<int, VectorSet<int>> &r_primitives_by_material)
{
  const Span<int> tri_faces = tri_data.tri_faces;
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<int> material_indices = *attributes.lookup<int>("material_index",
                                                                   bke::AttrDomain::Face);
  const BakePixel *pixels = engine->bake.pixels + image.offset;
  r_primitive_mask.primitive_ids = Array<int>(int64_t(image.width) * int64_t(image.height), -1);
  r_primitive_mask.has_valid_pixels = false;

  for (int y = 0; y < image.height; y++) {
    for (int x = 0; x < image.width; x++) {
      const BakePixel &pixel = pixels[y * image.width + x];
      if (pixel.object_id != engine->bake.object_id || pixel.primitive_id < 0) {
        continue;
      }
      if (pixel.primitive_id >= tri_faces.size()) {
        eevee_bake_report_error(
            engine, "Eevee Color Bake primitive index is outside the evaluated mesh triangle range");
        return false;
      }

      r_primitive_mask.primitive_ids[y * image.width + x] = pixel.primitive_id;
      r_primitive_mask.has_valid_pixels = true;

      const int material_index = material_index_for_primitive(
          object, pixel.primitive_id, tri_faces, material_indices);
      r_primitives_by_material.lookup_or_add_default(material_index).add(pixel.primitive_id);
    }
  }

  return true;
}

static bool collect_color_attribute_primitives_by_material(
    RenderEngine *engine,
    const BakeImage &image,
    const Object &object,
    const BakeTriData &tri_data,
    const Mesh &mesh,
    BakePrimitiveMask &r_primitive_mask,
    Map<int, VectorSet<int>> &r_primitives_by_material)
{
  const Span<int> tri_faces = tri_data.tri_faces;
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<int> material_indices = *attributes.lookup<int>("material_index",
                                                                   bke::AttrDomain::Face);
  const BakePixel *pixels = engine->bake.pixels + image.offset;
  r_primitive_mask.primitive_ids = Array<int>(int64_t(image.width) * int64_t(image.height), -1);
  r_primitive_mask.has_valid_pixels = false;

  for (int y = 0; y < image.height; y++) {
    for (int x = 0; x < image.width; x++) {
      const int pixel_i = y * image.width + x;
      const BakePixel &pixel = pixels[pixel_i];
      if (pixel.object_id != engine->bake.object_id || pixel.primitive_id < 0) {
        continue;
      }
      if (pixel.primitive_id >= tri_faces.size()) {
        eevee_bake_report_error(
            engine, "Eevee Color Bake primitive index is outside the evaluated mesh triangle range");
        return false;
      }

      r_primitive_mask.primitive_ids[pixel_i] = pixel.primitive_id;
      r_primitive_mask.has_valid_pixels = true;

      const int material_index = material_index_for_primitive(
          object, pixel.primitive_id, tri_faces, material_indices);
      r_primitives_by_material.lookup_or_add_default(material_index).add(pixel_i);
    }
  }

  return true;
}

static StringRefNull default_uv_name(const Mesh &mesh)
{
  const StringRefNull default_name = mesh.default_uv_map_name();
  if (!default_name.is_empty()) {
    return default_name;
  }
  return mesh.active_uv_map_name();
}

static bool lookup_uv_attribute(const Mesh &mesh,
                                const StringRef name,
                                VArraySpan<float2> &r_uvs)
{
  if (name.is_empty()) {
    return false;
  }
  const bke::AttributeAccessor attributes = mesh.attributes();
  const bke::AttributeReader<float2> uv_attr = attributes.lookup<float2>(
      name, bke::AttrDomain::Corner);
  if (!uv_attr) {
    return false;
  }
  r_uvs = VArraySpan<float2>(*uv_attr);
  return !r_uvs.is_empty();
}

static int find_tangent_layer_index(const Span<BakeBatchTangentLayer> tangent_layers,
                                    const StringRef uv_name)
{
  for (const int i : tangent_layers.index_range()) {
    if (tangent_layers[i].uv_name == uv_name) {
      return i;
    }
  }
  return -1;
}

static bool resolve_tangent_uv_name(RenderEngine *engine,
                                    const Mesh &mesh,
                                    const blender::Material *material,
                                    const GPUMaterialAttribute &attr,
                                    std::string &r_uv_name)
{
  StringRef uv_name = attr.name;
  if (uv_name.is_empty()) {
    uv_name = default_uv_name(mesh);
  }

  if (uv_name.is_empty()) {
    std::string message =
        "Eevee Color Bake requires a UV map for tangent-space material attributes in material \"";
    message += material_name(material);
    message += "\"";
    eevee_bake_report_error(engine, message);
    return false;
  }

  VArraySpan<float2> uvs;
  if (!lookup_uv_attribute(mesh, uv_name, uvs)) {
    std::string message = "Eevee Color Bake requires corner-domain UV map \"";
    message += uv_name;
    message += "\" for tangent-space material attributes in material \"";
    message += material_name(material);
    message += "\"";
    eevee_bake_report_error(engine, message);
    return false;
  }

  r_uv_name = uv_name;
  return true;
}

static bool ensure_tangent_layer(RenderEngine *engine,
                                 const Mesh &mesh,
                                 const blender::Material *material,
                                 const GPUMaterialAttribute &attr,
                                 Vector<BakeBatchTangentLayer> &r_tangent_layers,
                                 int &r_layer_index)
{
  std::string uv_name;
  if (!resolve_tangent_uv_name(engine, mesh, material, attr, uv_name)) {
    return false;
  }

  r_layer_index = find_tangent_layer_index(r_tangent_layers.as_span(), uv_name);
  if (r_layer_index != -1) {
    return true;
  }

  VArraySpan<float2> uv_map;
  lookup_uv_attribute(mesh, uv_name, uv_map);
  Array<Span<float2>> uv_map_spans(1);
  uv_map_spans[0] = uv_map;

  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> sharp_faces = *attributes.lookup<bool>("sharp_face",
                                                               bke::AttrDomain::Face);

  Array<Array<float4>> tangents = bke::mesh::calc_uv_tangents(mesh.vert_positions(),
                                                             mesh.faces(),
                                                             mesh.corner_verts(),
                                                             mesh.corner_tris(),
                                                             mesh.corner_tri_faces(),
                                                             sharp_faces,
                                                             mesh.vert_normals(),
                                                             mesh.face_normals(),
                                                             mesh.corner_normals(),
                                                             uv_map_spans);
  if (tangents.is_empty() || tangents[0].size() != mesh.corners_num) {
    std::string message = "Eevee Color Bake failed to calculate tangent space for UV map \"";
    message += uv_name;
    message += "\" in material \"";
    message += material_name(material);
    message += "\"";
    eevee_bake_report_error(engine, message);
    return false;
  }

  BakeBatchTangentLayer layer;
  layer.uv_name = uv_name;
  layer.values = std::move(tangents[0]);
  r_tangent_layers.append(std::move(layer));
  r_layer_index = int(r_tangent_layers.size()) - 1;
  return true;
}

static float4 bake_attribute_fallback_value(const GPUMaterialAttribute &attr)
{
  return attr.is_default_color ? float4(1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
}

static float4 orco_value_for_corner(const Mesh &mesh, const int vert)
{
  if (const float3 *orco = static_cast<const float3 *>(
          CustomData_get_layer(&mesh.vert_data, CD_ORCO)))
  {
    return float4(orco[vert], 0.0f);
  }

  float3 normalized = mesh.vert_positions()[vert];
  BKE_mesh_orco_verts_transform(
      const_cast<Mesh *>(&mesh), MutableSpan<float3>(&normalized, 1), false);
  return float4(normalized, 0.0f);
}

static float4 value_to_attribute_float4(const float value)
{
  return float4(value, value, value, 1.0f);
}

static float4 value_to_attribute_float4(const float2 value)
{
  return float4(value.x, value.y, 0.0f, 1.0f);
}

static float4 value_to_attribute_float4(const float3 value)
{
  return float4(value, 1.0f);
}

static float4 value_to_attribute_float4(const int value)
{
  const float v = float(value);
  return float4(v, v, v, 1.0f);
}

static float4 value_to_attribute_float4(const int2 value)
{
  return float4(float(value.x), float(value.y), 0.0f, 1.0f);
}

static float4 value_to_attribute_float4(const int8_t value)
{
  const float v = float(value);
  return float4(v, v, v, 1.0f);
}

static float4 value_to_attribute_float4(const short2 value)
{
  return float4(float(value.x), float(value.y), 0.0f, 1.0f);
}

static float4 value_to_attribute_float4(const bool value)
{
  const float v = value ? 1.0f : 0.0f;
  return float4(v, v, v, 1.0f);
}

static float4 value_to_attribute_float4(const ColorGeometry4f value)
{
  return float4(value.r, value.g, value.b, value.a);
}

static float4 value_to_attribute_float4(const ColorGeometry4b value)
{
  const ColorGeometry4f decoded = color::decode(value);
  return float4(decoded.r, decoded.g, decoded.b, decoded.a);
}

static float4 value_to_attribute_float4(const math::Quaternion value)
{
  return float4(value.w, value.x, value.y, value.z);
}

static float4 value_to_attribute_float4(const float4x4 value)
{
  const float *matrix = value.base_ptr();
  return float4(matrix[0], matrix[1], matrix[2], matrix[3]);
}

static int attribute_element_index_for_corner(const Mesh &mesh,
                                              const bke::AttrDomain domain,
                                              const int corner,
                                              const int face)
{
  switch (domain) {
    case bke::AttrDomain::Point:
      return mesh.corner_verts()[corner];
    case bke::AttrDomain::Edge:
      return mesh.corner_edges()[corner];
    case bke::AttrDomain::Face:
      return face;
    case bke::AttrDomain::Corner:
      return corner;
    default:
      return -1;
  }
}

static float4 attribute_value_for_corner(const Mesh &mesh,
                                         const GPUMaterialAttribute &attr,
                                         const int corner,
                                         const int vert,
                                         const int face)
{
  if (attr.type == CD_ORCO) {
    return orco_value_for_corner(mesh, vert);
  }

  StringRef name = attr.name;
  if (attr.is_default_color) {
    name = mesh.default_color_attribute ? StringRef(mesh.default_color_attribute) : StringRef();
  }
  else if (name.is_empty()) {
    name = default_uv_name(mesh);
  }

  const bke::AttributeAccessor attributes = mesh.attributes();
  const std::optional<bke::AttributeMetaData> meta = attributes.lookup_meta_data(name);
  if (!meta) {
    return bake_attribute_fallback_value(attr);
  }
  if (meta->data_type == bke::AttrType::String) {
    return bake_attribute_fallback_value(attr);
  }

  const bke::GAttributeReader reader = attributes.lookup(name);
  if (!reader) {
    return bake_attribute_fallback_value(attr);
  }

  const int element_index = attribute_element_index_for_corner(mesh, reader.domain, corner, face);
  if (element_index < 0) {
    return bake_attribute_fallback_value(attr);
  }

  const GVArraySpan span(*reader);
  if (element_index >= span.size()) {
    return bake_attribute_fallback_value(attr);
  }

  float4 value = bake_attribute_fallback_value(attr);
  bke::attribute_math::to_static_type(reader.varray.type(), [&]<typename T>() {
    value = value_to_attribute_float4(span.typed<T>()[element_index]);
  });
  return value;
}

static float3 barycentric_point(const float3 &a,
                                const float3 &b,
                                const float3 &c,
                                const float2 barycentric_uv)
{
  const float w0 = barycentric_uv.x;
  const float w1 = barycentric_uv.y;
  const float w2 = 1.0f - w0 - w1;
  return a * w0 + b * w1 + c * w2;
}

static float4 barycentric_point(const float4 &a,
                                const float4 &b,
                                const float4 &c,
                                const float2 barycentric_uv)
{
  const float w0 = barycentric_uv.x;
  const float w1 = barycentric_uv.y;
  const float w2 = 1.0f - w0 - w1;
  return a * w0 + b * w1 + c * w2;
}

static float4 barycentric_attribute_value_for_primitive(const Mesh &mesh,
                                                        const BakeTriData &tri_data,
                                                        const GPUMaterialAttribute &attr,
                                                        const int primitive_id,
                                                        const float2 barycentric_uv)
{
  const int3 tri = tri_data.corner_tris[primitive_id];
  const Span<int> tri_faces = tri_data.tri_faces;
  const Span<int> corner_verts = mesh.corner_verts();
  const int face = tri_faces[primitive_id];

  const float4 v0 = attribute_value_for_corner(mesh, attr, tri[0], corner_verts[tri[0]], face);
  const float4 v1 = attribute_value_for_corner(mesh, attr, tri[1], corner_verts[tri[1]], face);
  const float4 v2 = attribute_value_for_corner(mesh, attr, tri[2], corner_verts[tri[2]], face);
  return barycentric_point(v0, v1, v2, barycentric_uv);
}

static gpu::Batch *build_bake_batch_for_material(RenderEngine *engine,
                                                 const BakeImage &image,
                                                 const BakeTriData &tri_data,
                                                 const Mesh &mesh,
                                                 const StringRef bake_uv_name,
                                                 const Span<int> primitive_ids,
                                                 const blender::Material *material,
                                                 const GPUMaterial *gpumat)
{
  if (primitive_ids.is_empty()) {
    return nullptr;
  }

  VArraySpan<float2> bake_uvs;
  if (!lookup_uv_attribute(mesh, bake_uv_name, bake_uvs)) {
    std::string message = "Eevee Color Bake requires bake UV map \"";
    message += bake_uv_name;
    message += "\"";
    eevee_bake_report_error(engine, message);
    return nullptr;
  }

  GPUVertFormat format = {};
  const uint pos_attr = GPU_vertformat_attr_add(
      &format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint nor_attr = GPU_vertformat_attr_add(
      &format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint bake_uv_attr = GPU_vertformat_attr_add(
      &format, "bake_uv", gpu::VertAttrType::SFLOAT_32_32);
  const uint geom_nor_attr = GPU_vertformat_attr_add(
      &format, "geom_nor", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint primitive_id_attr = GPU_vertformat_attr_add(
      &format, "primitive_id", gpu::VertAttrType::SINT_32);

  Set<std::string> added_attribute_names;
  added_attribute_names.add("pos");
  added_attribute_names.add("nor");
  added_attribute_names.add("bake_uv");
  added_attribute_names.add("geom_nor");
  added_attribute_names.add("primitive_id");

  Vector<BakeBatchAttribute> batch_attributes;
  Vector<BakeBatchTangentLayer> tangent_layers;
  for (const GPUMaterialAttribute &gpu_attr : GPU_material_attributes(gpumat)) {
    if (added_attribute_names.add(gpu_attr.input_name)) {
      BakeBatchAttribute batch_attr;
      batch_attr.gpu_attr = &gpu_attr;
      batch_attr.format_attr = GPU_vertformat_attr_add(
          &format, gpu_attr.input_name, gpu::VertAttrType::SFLOAT_32_32_32_32);
      if (gpu_attr.type == CD_TANGENT) {
        if (!ensure_tangent_layer(
                engine, mesh, material, gpu_attr, tangent_layers, batch_attr.tangent_layer))
        {
          return nullptr;
        }
      }
      batch_attributes.append(batch_attr);
    }
  }

  const Span<int3> corner_tris = tri_data.corner_tris;
  const Span<int> tri_faces = tri_data.tri_faces;
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> face_normals = mesh.face_normals();
  const Span<float3> corner_normals = mesh.corner_normals();

  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, primitive_ids.size() * 3);

  int vertex_i = 0;
  for (const int primitive_id : primitive_ids) {
    const int3 tri = corner_tris[primitive_id];
    const int face = tri_faces[primitive_id];
    const float3 &geometry_normal = face_normals[face];
    for (int tri_corner = 0; tri_corner < 3; tri_corner++, vertex_i++) {
      const int corner = tri[tri_corner];
      const int vert = corner_verts[corner];
      const float3 position = positions[vert];
      const float3 normal = corner_normals[corner];
      const float2 uv = bake_uvs[corner];
      const float2 bake_uv = uv - float2(image.uv_offset);
      const float2 raster_uv = bake_uv -
                               float2(0.001f / float(image.width), 0.002f / float(image.height));

      GPU_vertbuf_attr_set(vbo, pos_attr, vertex_i, &position);
      GPU_vertbuf_attr_set(vbo, nor_attr, vertex_i, &normal);
      GPU_vertbuf_attr_set(vbo, bake_uv_attr, vertex_i, &raster_uv);
      GPU_vertbuf_attr_set(vbo, geom_nor_attr, vertex_i, &geometry_normal);
      GPU_vertbuf_attr_set(vbo, primitive_id_attr, vertex_i, &primitive_id);

      for (const BakeBatchAttribute &batch_attr : batch_attributes) {
        const float4 value = (batch_attr.tangent_layer == -1) ?
                                 attribute_value_for_corner(
                                     mesh, *batch_attr.gpu_attr, corner, vert, face) :
                                 tangent_layers[batch_attr.tangent_layer].values[corner];
        GPU_vertbuf_attr_set(vbo, batch_attr.format_attr, vertex_i, &value);
      }
    }
  }

  return GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
}

static gpu::Batch *build_color_attribute_batch_for_material(RenderEngine *engine,
                                                            const BakeImage &image,
                                                            const BakeTriData &tri_data,
                                                            const Mesh &mesh,
                                                            const Span<int> pixel_ids,
                                                            const blender::Material *material,
                                                            const GPUMaterial *gpumat)
{
  if (pixel_ids.is_empty()) {
    return nullptr;
  }

  GPUVertFormat format = {};
  const uint pos_attr = GPU_vertformat_attr_add(
      &format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint nor_attr = GPU_vertformat_attr_add(
      &format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint bake_uv_attr = GPU_vertformat_attr_add(
      &format, "bake_uv", gpu::VertAttrType::SFLOAT_32_32);
  const uint geom_nor_attr = GPU_vertformat_attr_add(
      &format, "geom_nor", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint primitive_id_attr = GPU_vertformat_attr_add(
      &format, "primitive_id", gpu::VertAttrType::SINT_32);

  Set<std::string> added_attribute_names;
  added_attribute_names.add("pos");
  added_attribute_names.add("nor");
  added_attribute_names.add("bake_uv");
  added_attribute_names.add("geom_nor");
  added_attribute_names.add("primitive_id");

  Vector<BakeBatchAttribute> batch_attributes;
  Vector<BakeBatchTangentLayer> tangent_layers;
  for (const GPUMaterialAttribute &gpu_attr : GPU_material_attributes(gpumat)) {
    if (added_attribute_names.add(gpu_attr.input_name)) {
      BakeBatchAttribute batch_attr;
      batch_attr.gpu_attr = &gpu_attr;
      batch_attr.format_attr = GPU_vertformat_attr_add(
          &format, gpu_attr.input_name, gpu::VertAttrType::SFLOAT_32_32_32_32);
      if (gpu_attr.type == CD_TANGENT) {
        if (!ensure_tangent_layer(
                engine, mesh, material, gpu_attr, tangent_layers, batch_attr.tangent_layer))
        {
          return nullptr;
        }
      }
      batch_attributes.append(batch_attr);
    }
  }

  const BakePixel *pixels = engine->bake.pixels + image.offset;
  const Span<int3> corner_tris = tri_data.corner_tris;
  const Span<int> tri_faces = tri_data.tri_faces;
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> face_normals = mesh.face_normals();
  const Span<float3> corner_normals = mesh.corner_normals();

  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, pixel_ids.size() * 6);

  int vertex_i = 0;
  for (const int pixel_i : pixel_ids) {
    const BakePixel &pixel = pixels[pixel_i];
    const int primitive_id = pixel.primitive_id;
    const int3 tri = corner_tris[primitive_id];
    const int face = tri_faces[primitive_id];
    const float2 barycentric_uv = float2(pixel.uv[0], pixel.uv[1]);
    const float3 &geometry_normal = face_normals[face];
    const float3 position = barycentric_point(positions[corner_verts[tri[0]]],
                                              positions[corner_verts[tri[1]]],
                                              positions[corner_verts[tri[2]]],
                                              barycentric_uv);
    const float3 normal = math::normalize(barycentric_point(corner_normals[tri[0]],
                                                            corner_normals[tri[1]],
                                                            corner_normals[tri[2]],
                                                            barycentric_uv));

    const int x = pixel_i % image.width;
    const int y = pixel_i / image.width;
    const float pixel_x0 = float(x) / float(image.width);
    const float pixel_x1 = float(x + 1) / float(image.width);
    const float pixel_y0 = float(y) / float(image.height);
    const float pixel_y1 = float(y + 1) / float(image.height);
    const float2 raster_uvs[6] = {
        float2(pixel_x0, pixel_y0),
        float2(pixel_x1, pixel_y0),
        float2(pixel_x1, pixel_y1),
        float2(pixel_x0, pixel_y0),
        float2(pixel_x1, pixel_y1),
        float2(pixel_x0, pixel_y1),
    };

    for (int tri_corner = 0; tri_corner < 6; tri_corner++, vertex_i++) {
      GPU_vertbuf_attr_set(vbo, pos_attr, vertex_i, &position);
      GPU_vertbuf_attr_set(vbo, nor_attr, vertex_i, &normal);
      GPU_vertbuf_attr_set(vbo, bake_uv_attr, vertex_i, &raster_uvs[tri_corner]);
      GPU_vertbuf_attr_set(vbo, geom_nor_attr, vertex_i, &geometry_normal);
      GPU_vertbuf_attr_set(vbo, primitive_id_attr, vertex_i, &primitive_id);

      for (const BakeBatchAttribute &batch_attr : batch_attributes) {
        const float4 value = (batch_attr.tangent_layer == -1) ?
                                 barycentric_attribute_value_for_primitive(
                                     mesh, tri_data, *batch_attr.gpu_attr, primitive_id, barycentric_uv) :
                                 barycentric_point(
                                     tangent_layers[batch_attr.tangent_layer].values[tri[0]],
                                     tangent_layers[batch_attr.tangent_layer].values[tri[1]],
                                     tangent_layers[batch_attr.tangent_layer].values[tri[2]],
                                     barycentric_uv);
        GPU_vertbuf_attr_set(vbo, batch_attr.format_attr, vertex_i, &value);
      }
    }
  }

  return GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
}

static GPUMaterial *compile_bake_material(RenderEngine *engine,
                                          Instance &inst,
                                          blender::Material *material)
{
  if (!validate_material_node_trees_for_bake(engine, material)) {
    return nullptr;
  }

  bNodeTree *ntree = (material->nodetree != nullptr) ? material->nodetree :
                                                   inst.materials.default_surface->nodetree;
  blender::Material *default_material = inst.materials.default_surface;
  GPUMaterial *gpumat = nullptr;

  for (int attempt = 0; attempt < 2; attempt++) {
    gpumat = inst.shaders.material_shader_get(material,
                                              ntree,
                                              MAT_PIPE_BAKE_COLOR,
                                              MAT_GEOM_MESH,
                                              MAT_PROBE_NONE,
                                              false,
                                              default_material,
                                              false);
    if (gpumat == nullptr) {
      break;
    }
    if (GPU_material_status(gpumat) != GPU_MAT_QUEUED) {
      break;
    }
    GPU_pass_cache_wait_for_all();
  }

  if (gpumat == nullptr || GPU_material_status(gpumat) != GPU_MAT_SUCCESS) {
    std::string message = "Eevee Color Bake failed to compile material \"";
    message += material_name(material);
    message += "\"";
    eevee_bake_report_error(engine, message);
    return nullptr;
  }

  if (!validate_gpu_material_for_bake(engine, material, gpumat)) {
    return nullptr;
  }

  return gpumat;
}

static bool sync_scene_for_bake(RenderEngine *engine, Depsgraph *depsgraph, Instance &inst)
{
  DRW_render_object_iter(
      engine, depsgraph, [&](draw::ObjectRef &ob_ref, RenderEngine *, Depsgraph *) {
        const Object *ob = ob_ref.object;
        if (!ELEM(ob->type, OB_LAMP, OB_LIGHTPROBE, OB_MESH, OB_CURVES, OB_POINTCLOUD)) {
          return;
        }
        inst.object_sync(ob_ref, *inst.manager);
      });
  return true;
}

static void bind_bake_resources(PassSimple::Sub &sub, Instance &inst)
{
  sub.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst.pipelines.utility_tx);
  sub.bind_resources(inst.uniform_data);
  inst.lights.bind_resources(sub);
  inst.lights.bind_front_light_shader_resources(sub);
  sub.bind_resources(inst.shadows);
  sub.bind_resources(inst.sampling);
  sub.bind_resources(inst.volume_probes);
  sub.bind_resources(inst.sphere_probes);
}

static void draw_bake_light_shader_surface_context(
    Instance &inst,
    const Vector<BakeDrawGroup> &draw_groups,
    const draw::ResourceHandleRange resource_handle,
    gpu::Texture *primitive_tx,
    draw::Texture &position_tx,
    draw::Texture &normal_tx,
    draw::View &view)
{
  draw::Framebuffer framebuffer("EeveeColorBake.LightShaderSurfaceFramebuffer");
  framebuffer.ensure(GPU_ATTACHMENT_NONE,
                     GPU_ATTACHMENT_TEXTURE(position_tx),
                     GPU_ATTACHMENT_TEXTURE(normal_tx));

  PassSimple pass("Eevee.ColorBake.LightShaderSurface");
  pass.shader_set(inst.shaders.static_shader_get(BAKE_LIGHT_SHADER_SURFACE));
  pass.framebuffer_set(&framebuffer);
  pass.clear_color(float4(0.0f));
  pass.state_set(DRW_STATE_WRITE_COLOR);

  for (const BakeDrawGroup &group : draw_groups) {
    if (group.batch == nullptr) {
      continue;
    }
    PassSimple::Sub &sub = pass.sub(material_name(group.material));
    sub.bind_texture("bake_primitive_tx", primitive_tx);
    sub.draw(group.batch, resource_handle);
  }

  inst.manager->submit(pass, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
}

static bool draw_bake_groups(RenderEngine *engine,
                             Instance &inst,
                             const Vector<BakeDrawGroup> &draw_groups,
                             const draw::ResourceHandleRange resource_handle,
                             const BakePrimitiveMask &primitive_mask,
                             RenderPass *combined_pass,
                             const int width,
                             const int height)
{
  if (!primitive_mask.has_valid_pixels) {
    std::fill_n(combined_pass->ibuf->float_buffer.data,
                int64_t(width) * int64_t(height) * 4,
                0.0f);
    return true;
  }

  gpu::Texture *primitive_tx = GPU_texture_create_2d("EeveeColorBake.PrimitiveMask",
                                                     width,
                                                     height,
                                                     1,
                                                     gpu::TextureFormat::SINT_32,
                                                     GPU_TEXTURE_USAGE_SHADER_READ,
                                                     nullptr);
  if (primitive_tx == nullptr) {
    eevee_bake_report_error(engine, "Eevee Color Bake failed to create the primitive mask");
    return false;
  }
  GPU_texture_update(primitive_tx, GPU_DATA_INT, primitive_mask.primitive_ids.data());
  GPU_texture_filter_mode(primitive_tx, false);
  GPU_texture_extend_mode(primitive_tx, GPU_SAMPLER_EXTEND_MODE_EXTEND);

  draw::Texture color_tx("EeveeColorBake.Color");
  color_tx.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32,
                     int2(width, height),
                     GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ |
                         GPU_TEXTURE_USAGE_HOST_READ);

  draw::Framebuffer framebuffer("EeveeColorBake.Framebuffer");
  framebuffer.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_tx));

  draw::Texture light_shader_position_tx("EeveeColorBake.LightShaderPosition");
  draw::Texture light_shader_normal_tx("EeveeColorBake.LightShaderNormal");
  if (inst.lights.needs_bake_light_shader()) {
    constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT |
                                       GPU_TEXTURE_USAGE_SHADER_READ;
    light_shader_position_tx.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32,
                                       int2(width, height),
                                       usage);
    light_shader_normal_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16,
                                     int2(width, height),
                                     usage);
  }

  ScopedBakeRenderBuffers render_buffers_scope(inst, int2(width, height));
  draw::Framebuffer depth_framebuffer("EeveeColorBake.Depth");
  depth_framebuffer.ensure(GPU_ATTACHMENT_TEXTURE(inst.render_buffers.depth_tx));
  depth_framebuffer.bind();
  depth_framebuffer.clear_depth(1.0f);
  inst.hiz_buffer.set_source(&inst.render_buffers.depth_tx);

  draw::View view("EeveeColorBake.View");
  const CameraData &camera_data = inst.camera.data_get();
  view.sync(camera_data.viewmat, camera_data.winmat);
  view.visibility_test(false);

  const int64_t pixel_count = int64_t(width) * int64_t(height);
  uint64_t max_sample_count = eevee_bake_max_accumulated_samples;
  /* Every bake sample requires a full float texture readback for CPU accumulation. Keep high
   * resolution bakes bounded without changing normal viewport/render sampling. */
  if (pixel_count > eevee_bake_medium_sample_pixel_limit) {
    max_sample_count = 4;
  }
  else if (pixel_count > eevee_bake_full_sample_pixel_limit) {
    max_sample_count = 16;
  }
  const int sample_count = int(
      std::max<uint64_t>(1, std::min<uint64_t>(inst.sampling.sample_count(), max_sample_count)));
  Array<float4> accumulated_color(pixel_count, float4(0.0f));

  for (int sample : IndexRange(sample_count)) {
    if (RE_engine_test_break(engine)) {
      GPU_TEXTURE_FREE_SAFE(primitive_tx);
      return false;
    }

    DRW_submission_start();

    inst.sampling.step();
    inst.capture_view.render_world();

    inst.volume_probes.set_view(view);
    inst.sphere_probes.set_view(view);
    inst.lights.set_view(view, int2(width, height));
    inst.shadows.set_view(view, int2(width, height));
    inst.lights.eval_uniform_light_shaders(view);
    if (inst.lights.needs_bake_light_shader()) {
      draw_bake_light_shader_surface_context(inst,
                                             draw_groups,
                                             resource_handle,
                                             primitive_tx,
                                             light_shader_position_tx,
                                             light_shader_normal_tx,
                                             view);
      inst.lights.eval_bake_light_shaders(
          view, int2(width, height), light_shader_position_tx, light_shader_normal_tx);
    }

    PassSimple pass("Eevee.ColorBake");
    pass.framebuffer_set(&framebuffer);
    pass.clear_color(float4(0.0f));
    pass.state_set(DRW_STATE_WRITE_COLOR);

    for (const BakeDrawGroup &group : draw_groups) {
      if (group.batch == nullptr || group.gpumat == nullptr) {
        continue;
      }
      PassSimple::Sub &sub = pass.sub(material_name(group.material));
      sub.material_set(*inst.manager, group.gpumat, false);
      bind_bake_resources(sub, inst);
      sub.bind_texture("bake_primitive_tx", primitive_tx);
      sub.push_constant("surface_cull_mode",
                        int(BKE_material_surface_cull_method_get(group.material)));
      sub.draw(group.batch, resource_handle);
    }

    inst.manager->submit(pass, view);

    DRW_submission_end();

    GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
    float4 *readback = color_tx.read<float4>(GPU_DATA_FLOAT);
    if (readback == nullptr) {
      eevee_bake_report_error(engine, "Eevee Color Bake failed to read back the GPU result");
      GPU_TEXTURE_FREE_SAFE(primitive_tx);
      return false;
    }

    for (int64_t pixel_i : IndexRange(pixel_count)) {
      accumulated_color[pixel_i] += readback[pixel_i];
    }
    MEM_delete(readback);

    if ((sample == 0) || ((sample + 1) % 16 == 0) || (sample + 1 == sample_count)) {
      std::string re_info = "Eevee Color Baking " + std::to_string(sample + 1) + " / " +
                            std::to_string(sample_count) + " samples";
      RE_engine_update_stats(engine, nullptr, re_info.c_str());
    }
    RE_engine_update_progress(engine, float(sample + 1) / float(sample_count));
    GPU_render_step();
  }

  const float sample_weight = 1.0f / float(sample_count);
  for (int64_t pixel_i : IndexRange(pixel_count)) {
    accumulated_color[pixel_i] *= sample_weight;
  }
  std::memcpy(
      combined_pass->ibuf->float_buffer.data, accumulated_color.data(), sizeof(float4) * pixel_count);
  GPU_TEXTURE_FREE_SAFE(primitive_tx);
  return true;
}

static bool run_gpu_bake(RenderEngine *engine,
                         Depsgraph *depsgraph,
                         Object *object,
                         Mesh &mesh,
                         const BakeImage &image,
                         const RenderLayer *render_layer,
                         RenderPass *combined_pass,
                         const int width,
                         const int height)
{
  BakeTriData tri_data = bake_tri_data_from_mesh(mesh);
  BakePrimitiveMask primitive_mask;
  Map<int, VectorSet<int>> primitives_by_material;
  const bool is_color_attribute_target = bake_target_is_color_attribute(image);
  if (is_color_attribute_target ?
          !collect_color_attribute_primitives_by_material(
              engine, image, *object, tri_data, mesh, primitive_mask, primitives_by_material) :
          !collect_image_primitives_by_material(
              engine, image, *object, tri_data, mesh, primitive_mask, primitives_by_material))
  {
    return false;
  }

  const bool use_render_context = engine->re != nullptr;
  if (use_render_context) {
    DRW_render_context_enable(engine->re);
  }
  else {
    if (!RE_engine_gpu_context_create(engine)) {
      eevee_bake_report_error(engine, "Eevee Color Bake failed to create a GPU context");
      return false;
    }
    if (!RE_engine_gpu_context_enable(engine)) {
      RE_engine_gpu_context_destroy(engine);
      eevee_bake_report_error(engine, "Eevee Color Bake failed to enable the GPU context");
      return false;
    }
  }
  if (!GPU_context_active_get()) {
    if (use_render_context) {
      DRW_render_context_disable(engine->re);
    }
    else {
      RE_engine_gpu_context_disable(engine);
      RE_engine_gpu_context_destroy(engine);
    }
    eevee_bake_report_error(engine, "Eevee Color Bake failed to enable the GPU context");
    return false;
  }

  bool ok = true;

  DRWContext draw_ctx(DRWContext::CUSTOM, depsgraph, int2(width, height));
  DRW_custom_pipeline_begin(draw_ctx, depsgraph);

  {
    Instance *inst_ptr = MEM_new<Instance>("Eevee Color Bake Instance");
    Instance &inst = *inst_ptr;
    inst.is_color_bake = true;
    ScopedBakeCamera bake_camera;
    Object *camera_object = scene_render_camera_get(engine);
    const bool use_fallback_camera = (camera_object == nullptr);
    if (use_fallback_camera) {
      bake_camera.object = BKE_object_add_only_object(nullptr, OB_CAMERA, "Eevee Color Bake Camera");
      bake_camera.data = BKE_id_new_nomain<blender::Camera>("Eevee Color Bake Camera");
      camera_object = bake_camera.object;
      if (bake_camera.object == nullptr || bake_camera.data == nullptr) {
        ok = false;
      }
      else {
        bake_camera.object->data = &bake_camera.data->id;
        configure_bake_camera(bake_camera, *object, mesh);
      }
    }
    rcti rect;
    rect.xmin = 0;
    rect.ymin = 0;
    rect.xmax = width;
    rect.ymax = height;
    if (ok) {
      inst.init(int2(width, height),
                &rect,
                &rect,
                engine,
                depsgraph,
                camera_object,
                render_layer);
      if (use_fallback_camera && bake_camera.object != nullptr) {
        inst.camera_orig_object = bake_camera.object;
        inst.camera_eval_object = bake_camera.object;
        CameraData bake_camera_data;
        if (camera_data_from_object(
                inst.scene, bake_camera.object, int2(width, height), bake_camera_data))
        {
          inst.camera.override(bake_camera_data, true);
        }
      }

      draw::Manager &manager = *inst.manager;
      manager.begin_sync();
      inst.begin_sync();
      sync_scene_for_bake(engine, depsgraph, inst);

      draw::ObjectRef object_ref(object);
      const BakeLocalBounds receiver_bounds = compute_bake_local_bounds(mesh);
      const float3 receiver_bounds_half_extent = receiver_bounds.extent * 0.5f +
                                                 float3(std::max(receiver_bounds.radius * 0.02f,
                                                                 0.01f));
      const float4x4 object_matrix = object->object_to_world();
      draw::ResourceHandleRange resource_handle = manager.resource_handle(
          object_ref, &object_matrix, &receiver_bounds.center, &receiver_bounds_half_extent);

      Vector<BakeDrawGroup> draw_groups;
      Vector<GPUMaterial *> gpu_materials;
      const std::string bake_uv_name = bake_uv_layer_name(engine, mesh);
      for (const auto &item : primitives_by_material.items()) {
        blender::Material *material = material_from_index(object, item.key);
        GPUMaterial *gpumat = compile_bake_material(engine, inst, material);
        if (gpumat == nullptr) {
          ok = false;
          break;
        }

        gpu::Batch *batch = is_color_attribute_target ?
                                 build_color_attribute_batch_for_material(engine,
                                                                          image,
                                                                          tri_data,
                                                                          mesh,
                                                                          item.value.as_span(),
                                                                          material,
                                                                          gpumat) :
                                 build_bake_batch_for_material(engine,
                                                               image,
                                                               tri_data,
                                                               mesh,
                                                               bake_uv_name,
                                                               item.value.as_span(),
                                                               material,
                                                               gpumat);
        if (batch == nullptr) {
          ok = false;
          break;
        }

        inst.manager->register_layer_attributes(gpumat);
        gpu_materials.append(gpumat);

        BakeDrawGroup group;
        group.material = material;
        group.gpumat = gpumat;
        group.batch = batch;
        draw_groups.append(group);
      }

      if (ok) {
        manager.extract_object_attributes(resource_handle, object_ref, gpu_materials);
        inst.shadows.sync_bake_receiver_bounds(resource_handle);
      }

      inst.end_sync();
      manager.end_sync();

      if (ok && !draw_groups.is_empty()) {
        ok = draw_bake_groups(
            engine, inst, draw_groups, resource_handle, primitive_mask, combined_pass, width, height);
      }
      else if (ok) {
        std::fill_n(combined_pass->ibuf->float_buffer.data,
                    int64_t(width) * int64_t(height) * 4,
                    0.0f);
      }

      for (BakeDrawGroup &group : draw_groups) {
        if (group.batch != nullptr) {
          GPU_batch_discard(group.batch);
        }
      }
    }

    MEM_delete(inst_ptr);
  }

  DRW_custom_pipeline_end(draw_ctx);
  if (use_render_context) {
    DRW_render_context_disable(engine->re);
  }
  else {
    RE_engine_gpu_context_disable(engine);
    RE_engine_gpu_context_destroy(engine);
  }

  return ok;
}

}  // namespace

void eevee_bake(RenderEngine *engine,
                Depsgraph *depsgraph,
                Object *object,
                const int pass_type,
                const int /*pass_filter*/,
                const int width,
                const int height)
{
  if (!validate_bake_request(engine, object, pass_type, width, height)) {
    return;
  }

  const BakeImage &image = engine->bake.targets->images[engine->bake.image_id];
  char layer_name[RE_MAXNAME];
  SNPRINTF(layer_name, "Eevee Color Bake %d", engine->bake.image_id);

  RenderResult *result = RE_engine_begin_result(engine, 0, 0, width, height, layer_name, nullptr);
  if (result == nullptr || result->layers.first == nullptr) {
    eevee_bake_report_error(engine, "Eevee Color Bake failed to allocate render result");
    return;
  }

  RenderLayer *layer = static_cast<RenderLayer *>(result->layers.first);
  RenderPass *combined_pass = RE_pass_find_by_name(layer, RE_PASSNAME_COMBINED, "");
  if (combined_pass == nullptr || combined_pass->ibuf == nullptr ||
      combined_pass->ibuf->float_buffer.data == nullptr)
  {
    RE_engine_end_result(engine, result, true, false, false);
    eevee_bake_report_error(engine, "Eevee Color Bake failed to allocate Combined pass");
    return;
  }

  Mesh *mesh = mesh_for_bake(depsgraph, object);
  if (mesh == nullptr) {
    RE_engine_end_result(engine, result, true, false, false);
    eevee_bake_report_error(engine, "Eevee Color Bake failed to access evaluated mesh");
    return;
  }

  const bool ok = run_gpu_bake(
      engine, depsgraph, object, *mesh, image, layer, combined_pass, width, height);

  BKE_id_free(nullptr, mesh);

  if (!ok) {
    RE_engine_end_result(engine, result, true, false, false);
    return;
  }

  RE_engine_end_result(engine, result, false, false, false);
}

}  // namespace blender::eevee
