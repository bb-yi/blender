/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_geometry_lines_mesh.hh"

#include <algorithm>
#include <cstring>
#include <map>
#include <limits>
#include <numeric>
#include <set>
#include <unordered_map>

#include "DNA_collection_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_color.hh"
#include "BLI_hash_c.hh"
#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_customdata.hh"
#include "BKE_duplilist.hh"
#include "BKE_image.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "DEG_depsgraph_query.hh"
#include "IMB_imbuf_types.hh"
#include "draw_handle.hh"
#include "eevee_velocity.hh"

namespace blender::eevee::geometry_lines {
namespace {

uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (size_t i = 0; i < size; i++) {
    hash = (hash ^ bytes[i]) * 1099511628211ULL;
  }
  return hash;
}
template<class T> uint64_t hash_value(uint64_t hash, const T &value)
{
  return hash_bytes(hash, &value, sizeof(value));
}
uint64_t id_hash(const ID &id)
{
  uint64_t hash = hash_bytes(14695981039346656037ULL, id.name, std::strlen(id.name));
  if (id.lib) {
    hash = hash_bytes(hash, id.lib->filepath, std::strlen(id.lib->filepath));
  }
  return hash;
}
Object *original(const Object *object)
{
  return object ? reinterpret_cast<Object *>(DEG_get_original_id(const_cast<ID *>(&object->id))) : nullptr;
}

bool collection_contains(Collection *collection, Object *object, bool children, bool instances)
{
  if (!collection || !object) { return false; }
  if (!children) { return BKE_collection_has_object(collection, object); }
  return instances ? BKE_collection_has_object_recursive_instanced(collection, object) :
                     BKE_collection_has_object_recursive(collection, object);
}

bool scope_matches(const ObjectScope &scope, const draw::ObjectRef &ref)
{
  Object *object = original(ref.object);
  Object *parent = original(ref.dupli_parent());
  if (!scope.instances && ref.is_dupli()) { return false; }
  auto in_list = [&](const std::vector<Object *> &list) {
    return std::find(list.begin(), list.end(), object) != list.end() ||
           (scope.instances && parent && std::find(list.begin(), list.end(), parent) != list.end());
  };
  bool include = scope.mode == 0;
  if (scope.mode == 1) {
    include = collection_contains(scope.collection, object, scope.children, scope.instances) ||
              (scope.instances && collection_contains(scope.collection, parent, scope.children, true));
  }
  else if (scope.mode == 2) {
    include = in_list(scope.objects);
  }
  return include && !in_list(scope.excluded) &&
         !collection_contains(scope.exclude_collection, object, true, scope.instances) &&
         !(scope.instances && collection_contains(scope.exclude_collection, parent, true, true));
}

bool material_matches(const RuleScope &scope, blender::Material *material)
{
  if (scope.materials.empty()) { return true; }
  const bool found = std::find(scope.materials.begin(), scope.materials.end(), material) != scope.materials.end();
  return scope.exclude_materials ? !found : found;
}

struct LocalMesh {
  std::vector<float3> positions;
  std::vector<float3> normals;
  std::vector<int> face_materials;
  std::vector<int3> triangles;
  std::vector<int> triangle_faces;
  std::map<std::string, std::vector<float2>> triangle_uvs;
  std::map<UVMapKey, std::vector<float2>> style_uvs;
  std::map<UVMapKey, std::vector<float4>> style_colors;
  std::map<UVMapKey, std::vector<float4>> point_colors;
  std::vector<Edge> edges;
  std::vector<int> edge_faces;
  std::vector<int> seam_representatives;
  uint64_t generation = 0;
  uint64_t revision = 0;
  uint64_t uv_signature = 0;
  uint64_t topology = 0;
  uint64_t mask_uv_revision = 0;
  uint64_t attributes_revision = 0;
};

std::shared_ptr<LocalMesh> read_mesh(const Mesh &mesh,
                                    uint64_t generation,
                                    uint64_t uv_signature,
                                    const std::set<std::string> &uv_names,
                                    const std::set<UVMapKey> &style_uv_names,
                                    const std::set<UVMapKey> &color_names,
                                    bool repair_seams)
{
  auto local = std::make_shared<LocalMesh>();
  local->generation = generation;
  local->uv_signature = uv_signature;
  const auto positions = mesh.vert_positions();
  local->positions.assign(positions.begin(), positions.end());
  /* Contours and dihedral angles describe the surface geometry. face_normals() can
   * average custom shading normals; keep those only for smoothing-boundary tests. */
  const auto normals = mesh.face_normals_true();
  local->normals.assign(normals.begin(), normals.end());
  const auto faces = mesh.faces();
  const auto vertices = mesh.corner_verts();
  const auto corner_edges = mesh.corner_edges();
  const auto edges = mesh.edges();
  const auto corners = mesh.corner_tris();
  const auto triangle_faces = mesh.corner_tri_faces();
  const auto corner_normals = mesh.corner_normals();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArray<int> materials = *attributes.lookup_or_default<int>("material_index", bke::AttrDomain::Face, 0);
  const VArray<bool> sharp_edges = *attributes.lookup_or_default<bool>("sharp_edge", bke::AttrDomain::Edge, false);
  const VArray<bool> sharp_faces = *attributes.lookup_or_default<bool>("sharp_face", bke::AttrDomain::Face, false);
  const VArray<bool> marks = *attributes.lookup_or_default<bool>("freestyle_edge", bke::AttrDomain::Edge, false);
  const auto wire_metadata = attributes.lookup_meta_data("pencil_wire_edge");
  const VArray<bool> wire_edges = wire_metadata && wire_metadata->domain == bke::AttrDomain::Edge &&
                                      wire_metadata->data_type == bke::AttrType::Bool ?
                                    *attributes.lookup<bool>("pencil_wire_edge", bke::AttrDomain::Edge) :
                                    VArray<bool>::from_single(true, edges.size());
  local->face_materials.resize(faces.size());
  local->edges.resize(edges.size());
  for (int face = 0; face < faces.size(); face++) {
    local->face_materials[face] = materials[face];
    for (int corner : faces[face]) {
      local->edges[corner_edges[corner]].face_count++;
    }
  }
  int offset = 0;
  for (int i = 0; i < edges.size(); i++) {
    Edge &edge = local->edges[i];
    edge.vertices = edges[i];
    edge.face_start = offset;
    offset += edge.face_count;
    edge.types = (wire_edges[i] ? Wire : 0) | (marks[i] ? Marked : 0) | (sharp_edges[i] ? Smoothing : 0);
    edge.identity = uint64_t(i) + 1;
  }
  local->edge_faces.resize(offset);
  std::vector<int> cursor(edges.size(), 0);
  std::vector<float3> normal_a(edges.size()), normal_b(edges.size());
  std::vector<bool> face_reverse(repair_seams ? edges.size() : 0);
  for (int face = 0; face < faces.size(); face++) {
    const auto face_corners = faces[face];
    for (int corner : face_corners) {
      const int next = corner + 1 == face_corners.one_after_last() ? face_corners.start() : corner + 1;
      const int edge_index = corner_edges[corner];
      Edge &edge = local->edges[edge_index];
      const bool reverse = vertices[corner] != edge.vertices.x;
      if (repair_seams && edge.face_count == 1) { face_reverse[edge_index] = reverse; }
      const float3 a = corner_normals[reverse ? next : corner];
      const float3 b = corner_normals[reverse ? corner : next];
      if (cursor[edge_index] == 0) {
        normal_a[edge_index] = a;
        normal_b[edge_index] = b;
      }
      else if (math::dot(normal_a[edge_index], a) < 0.9999f ||
               math::dot(normal_b[edge_index], b) < 0.9999f) {
        edge.types |= Smoothing;
      }
      if (sharp_faces[face]) { edge.types |= Smoothing; }
      local->edge_faces[edge.face_start + cursor[edge_index]++] = face;
    }
  }
  if (repair_seams) {
    /* Virtual adjacency only. Do not weld the evaluated mesh or merge different objects,
     * back-to-back sheets, or ambiguous stacks of more than two boundary edges. */
    std::map<std::array<float, 3>, int> coordinates;
    std::vector<int> coordinate_ids(positions.size());
    local->seam_representatives.resize(positions.size());
    std::iota(local->seam_representatives.begin(), local->seam_representatives.end(), 0);
    for (int i = 0; i < positions.size(); i++) {
      const float3 p = positions[i];
      coordinate_ids[i] = coordinates.emplace(std::array<float, 3>{p.x, p.y, p.z}, i).first->second;
    }
    std::map<std::pair<int, int>, std::vector<int>> boundaries;
    for (int i = 0; i < edges.size(); i++) {
      if (local->edges[i].face_count != 1) { continue; }
      const int a = coordinate_ids[edges[i].x], b = coordinate_ids[edges[i].y];
      if (a != b) { boundaries[{std::min(a,b), std::max(a,b)}].push_back(i); }
    }
    auto root = [&](int vertex) {
      int current = vertex;
      while (local->seam_representatives[current] != current) { current = local->seam_representatives[current]; }
      while (local->seam_representatives[vertex] != vertex) {
        const int next = local->seam_representatives[vertex];
        local->seam_representatives[vertex] = current;
        vertex = next;
      }
      return current;
    };
    auto unite = [&](int a, int b) {
      a = root(a); b = root(b);
      local->seam_representatives[std::max(a,b)] = std::min(a,b);
    };
    for (const auto &[key, matches] : boundaries) {
      if (matches.size() != 2) { continue; }
      const int a = matches[0], b = matches[1];
      Edge &ea = local->edges[a], &eb = local->edges[b];
      const int fa = local->edge_faces[ea.face_start], fb = local->edge_faces[eb.face_start];
      const int from_a = coordinate_ids[edges[a][face_reverse[a] ? 1 : 0]];
      const int from_b = coordinate_ids[edges[b][face_reverse[b] ? 1 : 0]];
      if (fa == fb || from_a == from_b || math::dot(normals[fa], normals[fb]) < -0.9999f) { continue; }
      const bool same = coordinate_ids[edges[a].x] == coordinate_ids[edges[b].x];
      uint32_t types = ea.types | eb.types;
      if (math::dot(normal_a[a], same ? normal_a[b] : normal_b[b]) < .9999f ||
          math::dot(normal_b[a], same ? normal_b[b] : normal_a[b]) < .9999f) { types |= Smoothing; }
      ea.seam_face = fb; eb.seam_face = fa;
      ea.seam_types = eb.seam_types = types;
      eb.seam_secondary = true;
      unite(edges[a].x, edges[b][same ? 0 : 1]);
      unite(edges[a].y, edges[b][same ? 1 : 0]);
    }
    for (int i = 0; i < positions.size(); i++) { local->seam_representatives[i] = root(i); }
  }
  local->triangles.reserve(corners.size());
  local->triangle_faces.assign(triangle_faces.begin(), triangle_faces.end());
  for (const int3 corner : corners) {
    local->triangles.push_back(int3(vertices[corner.x], vertices[corner.y], vertices[corner.z]));
  }
  const std::string active_uv = mesh.active_or_default_uv_map_name();
  for (const std::string &requested : uv_names) {
    const std::string name = requested.empty() ? active_uv : requested;
    const auto uv_attribute = attributes.lookup<float2>(name, bke::AttrDomain::Corner);
    if (!uv_attribute) { continue; }
    const VArray<float2> uv = *uv_attribute;
    auto &data = local->triangle_uvs[requested];
    data.reserve(corners.size() * 3);
    for (const int3 corner : corners) {
      data.push_back(uv[corner.x]);
      data.push_back(uv[corner.y]);
      data.push_back(uv[corner.z]);
    }
  }
  const auto uv_map_names = mesh.uv_map_names();
  for (const UVMapKey &requested : style_uv_names) {
    std::string name = requested.name.empty() ? active_uv : requested.name;
    if (requested.index >= 0) {
      if (requested.index >= uv_map_names.size()) { continue; }
      name = uv_map_names[requested.index];
    }
    const auto attribute = attributes.lookup<float2>(name, bke::AttrDomain::Corner);
    if (!attribute) { continue; }
    const VArray<float2> uv = *attribute;
    auto &data = local->style_uvs[requested];
    data.reserve(corners.size() * 3);
    for (const int3 corner : corners) {
      data.push_back(uv[corner.x]);
      data.push_back(uv[corner.y]);
      data.push_back(uv[corner.z]);
    }
  }
  std::vector<std::string> color_attributes;
  attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (ELEM(iter.domain, bke::AttrDomain::Point, bke::AttrDomain::Corner) &&
        ELEM(iter.data_type, bke::AttrType::ColorFloat, bke::AttrType::ColorByte)) {
      color_attributes.emplace_back(iter.name);
    }
  });
  for (const UVMapKey &requested : color_names) {
    std::string name = requested.name;
    if (requested.index >= 0) {
      if (requested.index >= int(color_attributes.size())) { continue; }
      name = color_attributes[requested.index];
    }
    const auto metadata = attributes.lookup_meta_data(name);
    if (!metadata || !ELEM(metadata->domain, bke::AttrDomain::Point, bke::AttrDomain::Corner) ||
        !ELEM(metadata->data_type, bke::AttrType::ColorFloat, bke::AttrType::ColorByte)) { continue; }
    const auto attribute = attributes.lookup<ColorGeometry4f>(name, bke::AttrDomain::Corner);
    if (attribute) {
      const VArray<ColorGeometry4f> colors = *attribute;
      auto &data = local->style_colors[requested];
      data.reserve(corners.size() * 3);
      for (const int3 corner : corners) {
        for (const int index : {corner.x, corner.y, corner.z}) {
          const ColorGeometry4f color = colors[index];
          data.emplace_back(color.r, color.g, color.b, color.a);
        }
      }
    }
    if (metadata->domain == bke::AttrDomain::Point) {
      const VArray<ColorGeometry4f> colors = *attributes.lookup<ColorGeometry4f>(name, bke::AttrDomain::Point);
      auto &data = local->point_colors[requested];
      data.reserve(positions.size());
      for (int i = 0; i < positions.size(); i++) {
        const ColorGeometry4f color = colors[i];
        data.emplace_back(color.r, color.g, color.b, color.a);
      }
    }
  }
  /* Mesh generations also change for UV/color edits. Content identities keep paint-only
   * changes out of the visibility/intersection caches. Never hash vector objects/padding. */
  uint64_t shape = 14695981039346656037ULL;
  auto hash_array = [&](const auto &array) {
    shape = hash_value(shape, array.size());
    shape = hash_bytes(shape, array.data(), array.size() * sizeof(array[0]));
  };
  hash_array(local->positions);
  hash_array(local->normals);
  hash_array(local->face_materials);
  hash_array(local->triangles);
  hash_array(local->triangle_faces);
  hash_array(local->edge_faces);
  hash_array(local->seam_representatives);
  for (const Edge &edge : local->edges) {
    shape = hash_value(shape, edge.vertices);
    shape = hash_value(shape, edge.types);
    shape = hash_value(shape, edge.face_start);
    shape = hash_value(shape, edge.face_count);
    shape = hash_value(shape, edge.seam_face);
    shape = hash_value(shape, edge.seam_types);
    shape = hash_value(shape, edge.seam_secondary);
  }
  local->revision = shape;
  uint64_t mask_hash = 14695981039346656037ULL;
  for (const auto &[name, uv] : local->triangle_uvs) {
    mask_hash = hash_bytes(mask_hash, name.data(), name.size());
    mask_hash = hash_value(mask_hash, uv.size());
    mask_hash = hash_bytes(mask_hash, uv.data(), uv.size() * sizeof(float2));
  }
  local->mask_uv_revision = mask_hash;
  uint64_t style_hash = 14695981039346656037ULL;
  for (const UVMapKey &key : style_uv_names) {
    style_hash = hash_bytes(style_hash, key.name.data(), key.name.size());
    style_hash = hash_value(style_hash, key.index);
    const auto found = local->style_uvs.find(key);
    if (found != local->style_uvs.end()) {
      style_hash = hash_bytes(style_hash, found->second.data(), found->second.size() * sizeof(float2));
    }
  }
  style_hash = hash_value(style_hash, 2);
  for (const UVMapKey &key : color_names) {
    style_hash = hash_bytes(style_hash, key.name.data(), key.name.size());
    style_hash = hash_value(style_hash, key.index);
    for (const auto *collection : {&local->style_colors, &local->point_colors}) {
      const auto found = collection->find(key);
      style_hash = hash_value(style_hash, found != collection->end());
      if (found != collection->end()) {
        style_hash = hash_bytes(style_hash, found->second.data(), found->second.size() * sizeof(float4));
      }
    }
  }
  local->attributes_revision = style_hash;
  return local;
}

std::shared_ptr<AlphaMask> read_alpha(blender::Image *image, float threshold)
{
  if (!image) { return nullptr; }
  ImageUser user = {};
  user.framenr = 1;
  void *lock = nullptr;
  ImBuf *buffer = BKE_image_acquire_ibuf(image, &user, &lock);
  if (!buffer) { return nullptr; }
  auto result = std::make_shared<AlphaMask>();
  result->width = buffer->x;
  result->height = buffer->y;
  result->threshold = threshold;
  result->alpha.resize(size_t(buffer->x) * buffer->y, 1.0f);
  const int channels = buffer->channels ? buffer->channels : 4;
  if (buffer->float_buffer.data && channels >= 4) {
    for (size_t i = 0; i < result->alpha.size(); i++) {
      result->alpha[i] = buffer->float_buffer.data[i * channels + 3];
    }
  }
  else if (buffer->byte_buffer.data) {
    for (size_t i = 0; i < result->alpha.size(); i++) {
      result->alpha[i] = buffer->byte_buffer.data[i * 4 + 3] / 255.0f;
    }
  }
  BKE_image_release_ibuf(image, buffer, lock);
  if (!result->alpha.empty()) {
    const auto range = std::minmax_element(result->alpha.begin(), result->alpha.end());
    result->minimum = *range.first;
    result->maximum = *range.second;
  }
  result->build_hierarchy();
  return result;
}

struct MeshInstance {
  std::shared_ptr<LocalMesh> mesh;
  float4x4 transform;
  uint64_t identity = 0;
  uint32_t resource_id = 0;
  std::vector<Surface> surfaces;
  std::vector<blender::Material *> materials;
  std::vector<std::string> uv_names;
  VelocityModule::GeometryLineStep previous;
  VelocityModule::GeometryLineStep next;
};

}  // namespace

struct GeometryBuilder::Impl {
  const Graph *graph = nullptr;
  VelocityModule *motion = nullptr;
  std::unordered_map<const Mesh *, std::shared_ptr<LocalMesh>> meshes;
  std::set<const Mesh *> used_meshes;
  std::vector<MeshInstance> instances;
  std::set<std::string> uv_names;
  std::set<UVMapKey> style_uv_names;
  std::set<UVMapKey> color_names;
  std::vector<std::shared_ptr<const AlphaMask>> masks;
  std::shared_ptr<const Geometry> cached;
  uint64_t uv_signature = 0;
  uint64_t image_revision = 0;
  uint64_t mask_signature = 0;
  uint64_t snapshot_signature = 0;
  bool repair_requested = false;
  std::string error;
  std::string mask_error;

  void begin(const Graph &new_graph, bool images_changed, VelocityModule *new_motion)
  {
    graph = &new_graph;
    motion = new_motion;
    error.clear();
    instances.clear();
    used_meshes.clear();
    uv_names.clear();
    style_uv_names.clear();
    color_names.clear();
    uv_signature = 14695981039346656037ULL;
    repair_requested = std::any_of(graph->scopes.begin(), graph->scopes.end(),
                                   [](const RuleScope &scope) { return scope.objects->repair_seams; });
    uv_signature = hash_value(uv_signature, repair_requested);
    if (images_changed && std::any_of(graph->occlusion.begin(), graph->occlusion.end(),
                                      [](const auto &rule) { return rule.mode == 2; })) { image_revision++; }
    const uint64_t signature = hash_value(graph->visibility_signature, image_revision);
    if (mask_signature != signature) {
      masks.clear();
      mask_error.clear();
      std::map<std::pair<blender::Image *, float>, std::shared_ptr<const AlphaMask>> shared_masks;
      for (const OcclusionRule &rule : graph->occlusion) {
        std::shared_ptr<const AlphaMask> mask;
        if (rule.mode == 2) {
          const auto key = std::make_pair(rule.image, rule.threshold);
          auto found = shared_masks.find(key);
          if (found == shared_masks.end()) { found = shared_masks.emplace(key, read_alpha(rule.image, rule.threshold)).first; }
          mask = found->second;
        }
        if (rule.mode == 2 && !mask) {
          mask_error = "A Geometry Lines alpha-mask rule has no readable image";
        }
        masks.push_back(std::move(mask));
      }
      mask_signature = signature;
    }
    error = mask_error;
    for (const OcclusionRule &rule : graph->occlusion) {
      if (rule.mode == 2) { uv_names.insert(rule.uv_map); }
    }
    for (const auto &name : uv_names) {
      uv_signature = hash_bytes(uv_signature, name.data(), name.size());
      uv_signature = hash_value(uv_signature, name.size());
    }
    for (const auto &brush : graph->brushes) {
      if (brush.fields) {
        for (const auto &source : brush.fields->sources) {
          if (source.type == 0) { style_uv_names.insert(source.attribute); }
          if (source.type == 1) { color_names.insert(source.attribute); }
        }
      }
      for (const int index : {brush.width_map, brush.color_map, brush.distortion_map, brush.brush_map}) {
        if (index >= 0 && graph->maps[index].coordinates == 0 &&
            ELEM(index, brush.width_map, brush.color_map, brush.distortion_map)) { style_uv_names.insert(graph->maps[index].uv); }
        if (index >= 0 && graph->maps[index].coordinates == 2) { color_names.insert(graph->maps[index].uv); }
      }
    }
    for (const UVMapKey &key : style_uv_names) {
      uv_signature = hash_bytes(uv_signature, key.name.data(), key.name.size());
      uv_signature = hash_value(uv_signature, key.index);
    }
    uv_signature = hash_value(uv_signature, 2);
    for (const UVMapKey &key : color_names) {
      uv_signature = hash_bytes(uv_signature, key.name.data(), key.name.size());
      uv_signature = hash_value(uv_signature, key.index);
    }
  }

  void add(const draw::ObjectRef &ref, uint32_t first_resource)
  {
    Mesh *mesh = BKE_object_get_evaluated_mesh(ref.object);
    if (!mesh) { return; }
    used_meshes.insert(mesh);
    const uint64_t generation = ref.object->runtime->last_update_geometry;
    auto found = meshes.find(mesh);
    if (found == meshes.end() || found->second->generation < generation ||
        found->second->positions.size() != size_t(mesh->verts_num) ||
        found->second->normals.size() != size_t(mesh->faces_num) ||
        found->second->uv_signature != uv_signature)
    {
      meshes[mesh] = read_mesh(*mesh, generation, uv_signature, uv_names, style_uv_names, color_names, repair_requested);
    }
    const auto &local = meshes[mesh];
    if (motion && local->topology == 0) {
      local->topology = VelocityModule::geometry_line_topology_hash(*mesh);
    }
    std::vector<bool> matches(graph->scopes.size()), occlusion_matches(graph->occlusion.size());
    for (int i = 0; i < int(graph->scopes.size()); i++) {
      matches[i] = scope_matches(*graph->scopes[i].objects, ref);
    }
    for (int i = 0; i < int(graph->occlusion.size()); i++) {
      occlusion_matches[i] = scope_matches(*graph->occlusion[i].scope.objects, ref);
    }
    const Object *object = original(ref.object);
    const Object *parent = original(ref.dupli_parent());
    uint64_t base_identity = id_hash(object->id);
    if (parent) { base_identity = hash_value(base_identity, id_hash(parent->id)); }
    const int material_count = std::max(1, BKE_object_material_count_eval(*ref.object, mesh->id));
    for (int instance_index = 0; instance_index < ref.instances_count(); instance_index++) {
      MeshInstance instance;
      instance.mesh = local;
      instance.resource_id = first_resource ? first_resource + instance_index : 0;
      instance.transform = ref.object_to_world(instance_index);
      if (motion) {
        if (const auto *step = motion->geometry_line_step_get(
                ref, instance_index, STEP_PREVIOUS, local->topology, int(local->positions.size()))) {
          instance.previous = *step;
        }
        if (const auto *step = motion->geometry_line_step_get(
                ref, instance_index, STEP_NEXT, local->topology, int(local->positions.size()))) {
          instance.next = *step;
        }
      }
      instance.identity = base_identity;
      if (const DupliObject *dupli = ref.dupli_object(instance_index)) {
        instance.identity = hash_bytes(instance.identity, dupli->persistent_id, sizeof(dupli->persistent_id));
      }
      instance.identity |= 1;
      for (int slot = 0; slot < material_count; slot++) {
        blender::Material *material = const_cast<blender::Material *>(
            BKE_object_material_get_eval(*ref.object, mesh->id, slot + 1));
        if (material) { material = reinterpret_cast<blender::Material *>(DEG_get_original_id(&material->id)); }
        Surface surface;
        surface.instance = instance.identity;
        surface.resource_id = instance.resource_id;
        surface.material_tag = material ? material->id.session_uid : 0;
        for (int rule = 0; rule < int(graph->scopes.size()); rule++) {
          if (matches[rule] && material_matches(graph->scopes[rule], material)) {
            surface.rules.push_back(rule);
          }
        }
        std::string uv_map;
        for (int rule = 0; rule < int(graph->occlusion.size()); rule++) {
          if (occlusion_matches[rule] && material_matches(graph->occlusion[rule].scope, material)) {
            surface.occlusion = graph->occlusion[rule].mode;
            surface.mask = masks[rule];
            uv_map = graph->occlusion[rule].uv_map;
            if (surface.occlusion == 2 && local->triangle_uvs.find(uv_map) == local->triangle_uvs.end()) {
              error = "A Geometry Lines alpha-mask rule references a missing UV map";
            }
            break;
          }
        }
        instance.surfaces.push_back(std::move(surface));
        instance.materials.push_back(material);
        instance.uv_names.push_back(std::move(uv_map));
      }
      instances.push_back(std::move(instance));
    }
  }

  std::shared_ptr<const Geometry> finish()
  {
    for (auto it = meshes.begin(); it != meshes.end();) {
      if (!used_meshes.contains(it->first)) { it = meshes.erase(it); }
      else { ++it; }
    }
    std::sort(instances.begin(), instances.end(), [](const auto &a, const auto &b) {
      return a.identity < b.identity;
    });
    uint64_t signature = hash_value(graph->visibility_signature, image_revision);
    uint64_t spatial_signature = 14695981039346656037ULL;
    uint64_t attributes_signature = 14695981039346656037ULL;
    for (const MeshInstance &instance : instances) {
      signature = hash_value(signature, instance.identity);
      signature = hash_value(signature, instance.mesh->revision);
      signature = hash_value(signature, instance.mesh->mask_uv_revision);
      attributes_signature = hash_value(attributes_signature, instance.identity);
      attributes_signature = hash_value(attributes_signature, instance.mesh->attributes_revision);
      attributes_signature = hash_value(attributes_signature, instance.resource_id);
      for (const auto &surface : instance.surfaces) { attributes_signature = hash_value(attributes_signature, surface.material_tag); }
      signature = hash_value(signature, instance.transform);
      if (motion) {
        signature = hash_value(signature, instance.previous.revision);
        signature = hash_value(signature, instance.next.revision);
        if (instance.previous.positions) {
          signature = hash_value(signature, instance.previous.object_to_world);
        }
        if (instance.next.positions) {
          signature = hash_value(signature, instance.next.object_to_world);
        }
      }
      spatial_signature = hash_value(spatial_signature, instance.identity);
      spatial_signature = hash_value(spatial_signature, instance.mesh->revision);
      spatial_signature = hash_value(spatial_signature, instance.transform);
      for (blender::Material *material : instance.materials) { signature = hash_value(signature, material); }
      for (const Surface &surface : instance.surfaces) {
        for (int rule : surface.rules) { signature = hash_value(signature, rule); }
        signature = hash_value(signature, surface.rules.size());
        signature = hash_value(signature, surface.occlusion);
        spatial_signature = hash_value(spatial_signature, surface.rules.empty());
      }
    }
    const uint64_t snapshot_key = hash_value(signature, attributes_signature);
    if (cached && snapshot_signature == snapshot_key) { return cached; }
    auto result = std::make_shared<Geometry>();
    result->revision = signature;
    result->attributes_revision = attributes_signature;
    result->intersection_revision = spatial_signature;
    result->has_motion_steps = motion != nullptr;
    size_t vertices = 0, faces = 0, triangles = 0, edges = 0, adjacencies = 0;
    for (const MeshInstance &instance : instances) {
      vertices += instance.mesh->positions.size();
      faces += instance.mesh->normals.size();
      triangles += instance.mesh->triangles.size();
      edges += instance.mesh->edges.size();
      adjacencies += instance.mesh->edge_faces.size();
    }
    result->positions.reserve(vertices);
    result->previous_positions.reserve(vertices);
    result->next_positions.reserve(vertices);
    result->vertex_identities.reserve(vertices);
    if (repair_requested) { result->seam_vertex_identities.reserve(vertices); }
    result->faces.reserve(faces);
    result->triangles.reserve(triangles);
    result->edges.reserve(edges);
    result->edge_faces.reserve(adjacencies);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const UVMapKey &key : style_uv_names) { result->style_uvs[key].resize(triangles * 3, float2(nan)); }
    for (const UVMapKey &key : color_names) {
      result->style_colors[key].resize(triangles * 3, float4(nan));
      result->point_colors[key].resize(vertices, float4(nan));
    }
    for (const MeshInstance &instance : instances) {
      const LocalMesh &mesh = *instance.mesh;
      const int vertex_base = int(result->positions.size());
      const int face_base = int(result->faces.size());
      const int surface_base = int(result->surfaces.size());
      const int adjacency_base = int(result->edge_faces.size());
      const double4x4 transform(instance.transform);
      const double3x3 normal_transform = math::transpose(math::invert(double3x3(transform)));
      result->surfaces.insert(result->surfaces.end(), instance.surfaces.begin(), instance.surfaces.end());
      for (int vertex = 0; vertex < int(mesh.positions.size()); vertex++) {
        const double3 current = math::transform_point(transform, double3(mesh.positions[vertex]));
        result->positions.push_back(current);
        const auto &previous = instance.previous;
        const auto &next = instance.next;
        result->previous_positions.push_back(previous.positions ?
            math::transform_point(double4x4(previous.object_to_world), double3((*previous.positions)[vertex])) : current);
        result->next_positions.push_back(next.positions ?
            math::transform_point(double4x4(next.object_to_world), double3((*next.positions)[vertex])) : current);
        result->vertex_identities.push_back(hash_value(instance.identity, vertex) | 1);
        if (repair_requested) {
          const int representative = mesh.seam_representatives.empty() ? vertex : mesh.seam_representatives[vertex];
          result->seam_vertex_identities.push_back(hash_value(instance.identity, representative) | 1);
        }
      }
      for (int face = 0; face < int(mesh.normals.size()); face++) {
        const int slot = std::clamp(mesh.face_materials[face], 0, int(instance.surfaces.size()) - 1);
        result->faces.push_back({surface_base + slot,
                                  math::normalize(normal_transform * double3(mesh.normals[face])),
                                  hash_value(instance.identity, face) | 1});
      }
      for (int i = 0; i < int(mesh.triangles.size()); i++) {
        const int face = mesh.triangle_faces[i];
        const int slot = std::clamp(mesh.face_materials[face], 0, int(instance.surfaces.size()) - 1);
        Triangle triangle;
        triangle.vertices = mesh.triangles[i] + int3(vertex_base);
        triangle.face = face_base + face;
        const auto uv = mesh.triangle_uvs.find(instance.uv_names[slot]);
        for (int j = 0; j < 3; j++) {
          triangle.uv[j] = uv != mesh.triangle_uvs.end() ? double2(uv->second[i * 3 + j]) : double2(0);
        }
        result->triangles.push_back(triangle);
      }
      const size_t triangle_start = result->triangles.size() - mesh.triangles.size();
      for (const auto &[key, uv] : mesh.style_uvs) {
        auto &target = result->style_uvs[key];
        std::copy(uv.begin(), uv.end(), target.begin() + triangle_start * 3);
      }
      for (const auto &[key, colors] : mesh.style_colors) {
        std::copy(colors.begin(), colors.end(), result->style_colors[key].begin() + triangle_start * 3);
      }
      for (const auto &[key, colors] : mesh.point_colors) {
        std::copy(colors.begin(), colors.end(), result->point_colors[key].begin() + vertex_base);
      }
      for (int face : mesh.edge_faces) { result->edge_faces.push_back(face_base + face); }
      int loose_face = -1;
      for (int i = 0; i < int(mesh.edges.size()); i++) {
        Edge edge = mesh.edges[i];
        edge.vertices += int2(vertex_base);
        edge.face_start += adjacency_base;
        edge.identity = hash_value(instance.identity, i) | 1;
        if (edge.seam_face >= 0) {
          edge.seam_face += face_base;
          const int own_surface = result->faces[result->edge_faces[edge.face_start]].surface;
          const int other_surface = result->faces[edge.seam_face].surface;
          if (instance.materials[own_surface - surface_base] != instance.materials[other_surface - surface_base]) {
            edge.seam_types |= MaterialBoundary;
          }
        }
        if (edge.face_count == 0) {
          if (loose_face < 0) {
            loose_face = int(result->faces.size());
            result->faces.push_back({surface_base, double3(0), hash_value(instance.identity, -1) | 1});
          }
          edge.loose = true;
          edge.face_start = int(result->edge_faces.size());
          edge.face_count = 1;
          result->edge_faces.push_back(loose_face);
        }
        if (edge.face_count > 1) {
          const Face &a = result->faces[result->edge_faces[edge.face_start]];
          const Face &b = result->faces[result->edge_faces[edge.face_start + 1]];
          edge.normal_dot = math::dot(a.normal, b.normal);
          const int first_slot = a.surface - surface_base;
          for (int j = 1; j < edge.face_count; j++) {
            const int slot = result->faces[result->edge_faces[edge.face_start + j]].surface - surface_base;
            if (instance.materials[slot] != instance.materials[first_slot]) {
              edge.types |= MaterialBoundary;
            }
          }
        }
        result->edges.push_back(edge);
      }
    }
    if (!motion && cached && cached->vertex_identities == result->vertex_identities) {
      result->previous_positions = cached->positions;
    }
    else if (!motion && cached) {
      std::unordered_map<uint64_t, int> previous;
      previous.reserve(cached->vertex_identities.size());
      for (int i = 0; i < int(cached->vertex_identities.size()); i++) { previous[cached->vertex_identities[i]] = i; }
      for (int i = 0; i < int(result->vertex_identities.size()); i++) {
        const auto found = previous.find(result->vertex_identities[i]);
        if (found != previous.end()) { result->previous_positions[i] = cached->positions[found->second]; }
      }
    }
    cached = result;
    snapshot_signature = snapshot_key;
    return result;
  }
};

GeometryBuilder::GeometryBuilder() : impl_(std::make_unique<Impl>()) {}
GeometryBuilder::~GeometryBuilder() = default;
void GeometryBuilder::begin(const Graph &graph, bool images_changed, VelocityModule *motion)
{
  impl_->begin(graph, images_changed, motion);
}
void GeometryBuilder::add(const draw::ObjectRef &object, uint32_t first_resource) { impl_->add(object, first_resource); }
std::shared_ptr<const Geometry> GeometryBuilder::finish() { return impl_->finish(); }
const std::string &GeometryBuilder::error() const { return impl_->error; }
void GeometryBuilder::clear() { impl_ = std::make_unique<Impl>(); }

}  // namespace blender::eevee::geometry_lines
