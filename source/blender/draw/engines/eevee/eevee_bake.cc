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
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "DNA_material_types.h"
#include "DNA_camera_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
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
    if (bake.target != R_BAKE_TARGET_IMAGE_TEXTURES) {
      eevee_bake_report_error(engine, "Eevee Color Bake only supports Image Textures targets");
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

static bool node_tree_contains_unsupported_bake_node(const bNodeTree &ntree,
                                                     Set<const bNodeTree *> &visited,
                                                     const bNode *&r_node,
                                                     const char *&r_feature)
{
  if (visited.contains(&ntree)) {
    return false;
  }
  visited.add(&ntree);

  for (const bNode *node : ntree.all_nodes()) {
    if ((node->flag & NODE_MUTED) != 0) {
      continue;
    }

    switch (node->type_legacy) {
      case SH_NODE_NPR_INPUT:
        r_node = node;
        r_feature = "NPR Input screen/GBuffer reads";
        return true;
      case SH_NODE_NPR_IMAGE_SAMPLE:
        r_node = node;
        r_feature = "NPR Image Sample";
        return true;
      case SH_NODE_NPR_REFRACTION:
        r_node = node;
        r_feature = "NPR Refraction";
        return true;
      case SH_NODE_INPUT_AOV:
        r_node = node;
        r_feature = "Input AOV";
        return true;
      case SH_NODE_OUTPUT_AOV:
        r_node = node;
        r_feature = "Output AOV";
        return true;
      case SH_NODE_OUTPUT_FILTER:
        r_node = node;
        r_feature = "Filter-domain output";
        return true;
      case SH_NODE_RENDER_TEXTURE:
        r_node = node;
        r_feature = "Render Texture feedback";
        return true;
      case SH_NODE_SCENE_COLOR:
        r_node = node;
        r_feature = "Scene Color";
        return true;
      case SH_NODE_SCREENSPACE_INFO:
        r_node = node;
        r_feature = "Screen Space Info";
        return true;
      case SH_NODE_SHADER_INFO:
        if (node->custom1 == SHD_SHADER_INFO_SHADOW_SOFT_FILTERED) {
          r_node = node;
          r_feature = "Shader Info Soft Filtered shadows";
          return true;
        }
        break;
      case NODE_GROUP:
        if (const bNodeTree *group_tree = reinterpret_cast<const bNodeTree *>(node->id)) {
          if (node_tree_contains_unsupported_bake_node(
                  *group_tree, visited, r_node, r_feature))
          {
            return true;
          }
        }
        break;
      default:
        break;
    }
  }

  return false;
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
    message += "\" (node \"";
    message += node_display_name(*unsupported_node);
    message += "\")";
    eevee_bake_report_error(engine, message);
  };

  if (material->nodetree != nullptr &&
      node_tree_contains_unsupported_bake_node(
          *material->nodetree, visited, unsupported_node, unsupported_feature))
  {
    report_unsupported();
    return false;
  }

  visited.clear();
  if (bNodeTree *npr_tree = npr_tree_get_from_mat(material)) {
    if (node_tree_contains_unsupported_bake_node(
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
  return mesh;
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
                                                 const Mesh &mesh,
                                                 Map<int, VectorSet<int>> &r_primitives_by_material)
{
  const Span<int> tri_faces = mesh.corner_tri_faces();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<int> material_indices = *attributes.lookup<int>("material_index",
                                                                   bke::AttrDomain::Face);
  const BakePixel *pixels = engine->bake.pixels + image.offset;

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

      const int material_index = material_index_for_primitive(
          object, pixel.primitive_id, tri_faces, material_indices);
      r_primitives_by_material.lookup_or_add_default(material_index).add(pixel.primitive_id);
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

static float4 attribute_value_for_corner(const Mesh &mesh,
                                         const GPUMaterialAttribute &attr,
                                         const int corner)
{
  if (attr.type == CD_ORCO) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
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
    return attr.is_default_color ? float4(1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
  }

  switch (meta->data_type) {
    case bke::AttrType::Float2: {
      const bke::AttributeReader<float2> reader = attributes.lookup<float2>(
          name, bke::AttrDomain::Corner);
      if (reader) {
        const float2 value = VArraySpan<float2>(*reader)[corner];
        return float4(value.x, value.y, 0.0f, 1.0f);
      }
      break;
    }
    case bke::AttrType::Float3: {
      const bke::AttributeReader<float3> reader = attributes.lookup<float3>(
          name, bke::AttrDomain::Corner);
      if (reader) {
        const float3 value = VArraySpan<float3>(*reader)[corner];
        return float4(value, 1.0f);
      }
      break;
    }
    case bke::AttrType::Float: {
      const bke::AttributeReader<float> reader = attributes.lookup<float>(
          name, bke::AttrDomain::Corner);
      if (reader) {
        const float value = VArraySpan<float>(*reader)[corner];
        return float4(value, 0.0f, 0.0f, 1.0f);
      }
      break;
    }
    case bke::AttrType::Int32: {
      const bke::AttributeReader<int> reader = attributes.lookup<int>(name,
                                                                      bke::AttrDomain::Corner);
      if (reader) {
        const float value = float(VArraySpan<int>(*reader)[corner]);
        return float4(value, 0.0f, 0.0f, 1.0f);
      }
      break;
    }
    case bke::AttrType::Bool: {
      const bke::AttributeReader<bool> reader = attributes.lookup<bool>(name,
                                                                        bke::AttrDomain::Corner);
      if (reader) {
        const float value = VArraySpan<bool>(*reader)[corner] ? 1.0f : 0.0f;
        return float4(value, 0.0f, 0.0f, 1.0f);
      }
      break;
    }
    default:
      break;
  }

  return attr.is_default_color ? float4(1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
}

static gpu::Batch *build_bake_batch_for_material(RenderEngine *engine,
                                                 const BakeImage &image,
                                                 const Mesh &mesh,
                                                 const Span<int> primitive_ids,
                                                 const blender::Material *material,
                                                 const GPUMaterial *gpumat)
{
  if (primitive_ids.is_empty()) {
    return nullptr;
  }

  VArraySpan<float2> bake_uvs;
  if (!lookup_uv_attribute(mesh, mesh.active_or_default_uv_map_name(), bake_uvs)) {
    eevee_bake_report_error(engine, "Eevee Color Bake requires an active UV map");
    return nullptr;
  }

  GPUVertFormat format = {};
  const uint pos_attr = GPU_vertformat_attr_add(
      &format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint nor_attr = GPU_vertformat_attr_add(
      &format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint bake_uv_attr = GPU_vertformat_attr_add(
      &format, "bake_uv", gpu::VertAttrType::SFLOAT_32_32);

  Set<std::string> added_attribute_names;
  added_attribute_names.add("pos");
  added_attribute_names.add("nor");
  added_attribute_names.add("bake_uv");

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

  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> corner_normals = mesh.corner_normals();

  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, primitive_ids.size() * 3);

  int vertex_i = 0;
  for (const int primitive_id : primitive_ids) {
    const int3 tri = corner_tris[primitive_id];
    for (int tri_corner = 0; tri_corner < 3; tri_corner++, vertex_i++) {
      const int corner = tri[tri_corner];
      const int vert = corner_verts[corner];
      const float3 position = positions[vert];
      const float3 normal = corner_normals[corner];
      const float2 uv = bake_uvs[corner];
      const float2 bake_uv = uv - float2(image.uv_offset);

      GPU_vertbuf_attr_set(vbo, pos_attr, vertex_i, &position);
      GPU_vertbuf_attr_set(vbo, nor_attr, vertex_i, &normal);
      GPU_vertbuf_attr_set(vbo, bake_uv_attr, vertex_i, &bake_uv);

      for (const BakeBatchAttribute &batch_attr : batch_attributes) {
        const float4 value = (batch_attr.tangent_layer == -1) ?
                                 attribute_value_for_corner(mesh, *batch_attr.gpu_attr, corner) :
                                 tangent_layers[batch_attr.tangent_layer].values[corner];
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
    pass.sub(material_name(group.material)).draw(group.batch, resource_handle);
  }

  inst.manager->submit(pass, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
}

static bool draw_bake_groups(RenderEngine *engine,
                             Instance &inst,
                             const Vector<BakeDrawGroup> &draw_groups,
                             const draw::ResourceHandleRange resource_handle,
                             RenderPass *combined_pass,
                             const int width,
                             const int height)
{
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
  const int sample_count = int(std::max<uint64_t>(
      1,
      std::min<uint64_t>(inst.sampling.sample_count(), eevee_bake_max_accumulated_samples)));
  Array<float4> accumulated_color(pixel_count, float4(0.0f));

  for (int sample : IndexRange(sample_count)) {
    if (RE_engine_test_break(engine)) {
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
  Map<int, VectorSet<int>> primitives_by_material;
  if (!collect_image_primitives_by_material(engine, image, *object, mesh, primitives_by_material)) {
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
    Scene *input_scene = DEG_get_input_scene(depsgraph);
    Object *camera_object = (input_scene != nullptr) ? input_scene->camera : nullptr;
    if (camera_object == nullptr) {
      bake_camera.object = BKE_object_add_only_object(nullptr,
                                                      OB_CAMERA,
                                                      "Eevee Color Bake Camera");
      bake_camera.data = BKE_id_new_nomain<blender::Camera>("Eevee Color Bake Camera");
      if (bake_camera.object == nullptr || bake_camera.data == nullptr) {
        ok = false;
      }
      else {
        bake_camera.object->data = &bake_camera.data->id;
        configure_bake_camera(bake_camera, *object, mesh);
        camera_object = bake_camera.object;
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
      if (bake_camera.object != nullptr) {
        inst.camera_orig_object = bake_camera.object;
        inst.camera_eval_object = bake_camera.object;
      }

      draw::Manager &manager = *inst.manager;
      manager.begin_sync();
      inst.begin_sync();
      sync_scene_for_bake(engine, depsgraph, inst);

      draw::ObjectRef object_ref(object);
      const float receiver_bounds_inflate = std::max(
          compute_bake_world_bounds(*object, mesh).radius * 0.02f, 0.01f);
      draw::ResourceHandleRange resource_handle = manager.resource_handle(object_ref,
                                                                          receiver_bounds_inflate);

      Vector<BakeDrawGroup> draw_groups;
      Vector<GPUMaterial *> gpu_materials;
      for (const auto &item : primitives_by_material.items()) {
        blender::Material *material = material_from_index(object, item.key);
        GPUMaterial *gpumat = compile_bake_material(engine, inst, material);
        if (gpumat == nullptr) {
          ok = false;
          break;
        }

        gpu::Batch *batch = build_bake_batch_for_material(
            engine, image, mesh, item.value.as_span(), material, gpumat);
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
        ok = draw_bake_groups(engine, inst, draw_groups, resource_handle, combined_pass, width, height);
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
