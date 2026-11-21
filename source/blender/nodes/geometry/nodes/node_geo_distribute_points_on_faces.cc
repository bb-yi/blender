/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "BLI_hash_mm3.hh"
#include "BLI_implicit_sharing_ptr.hh"
#include "BLI_kdtree_new.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation_c.hh"
#include "BLI_memory_cache.hh"
#include "BLI_memory_counter.hh"
#include "BLI_noise.hh"
#include "BLI_rand.hh"
#include "BLI_shared_cache.hh"
#include "BLI_task.hh"
#include "BLI_timeit.hh"

#include "CLG_log.h"

#include "DNA_pointcloud_types.h"

#include "BKE_attribute_math.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_mesh_types.hh"
#include "BKE_pointcloud.hh"

#include "FN_multi_function_registry.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_randomize.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_distribute_points_on_faces_cc {

static CLG_LogRef CACHE_LOG = {"nodes.scatter_cache"};

static void node_declare(NodeDeclarationBuilder &b)
{
  auto enable_random = [](bNode &node) {
    node.custom1 = GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_RANDOM;
  };
  auto enable_poisson = [](bNode &node) {
    node.custom1 = GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_POISSON;
  };

  b.add_input<decl::Geometry>("Mesh"_ustr)
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Mesh on whose faces to distribute points on");
  b.add_input<decl::Bool>("Selection"_ustr)
      .default_value(true)
      .hide_value()
      .evaluated_geometry_field();
  auto &distance_min = b.add_input<decl::Float>("Distance Min"_ustr)
                           .min(0.0f)
                           .subtype(PROP_DISTANCE)
                           .make_available(enable_poisson)
                           .available(false);
  auto &density_max = b.add_input<decl::Float>("Density Max"_ustr)
                          .default_value(10.0f)
                          .min(0.0f)
                          .make_available(enable_poisson)
                          .available(false);
  auto &density = b.add_input<decl::Float>("Density"_ustr)
                      .default_value(10.0f)
                      .min(0.0f)
                      .evaluated_geometry_field()
                      .make_available(enable_random)
                      .available(false);
  auto &density_factor = b.add_input<decl::Float>("Density Factor"_ustr)
                             .default_value(1.0f)
                             .min(0.0f)
                             .max(1.0f)
                             .subtype(PROP_FACTOR)
                             .evaluated_geometry_field()
                             .make_available(enable_poisson)
                             .available(false);
  b.add_input<decl::Int>("Seed"_ustr);

  b.add_output<decl::Geometry>("Points"_ustr).propagate_all_geometry();
  b.add_output<decl::Vector>("Normal"_ustr).anonymous_attribute_output();
  b.add_output<decl::Rotation>("Rotation"_ustr).anonymous_attribute_output();

  const bNode *node = b.node_or_null();
  if (node != nullptr) {
    switch (node->custom1) {
      case GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_POISSON:
        distance_min.available(true);
        density_max.available(true);
        density_factor.available(true);
        break;
      case GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_RANDOM:
        density.available(true);
        break;
    }
  }
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "distribute_method", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_layout_ex(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "use_legacy_normal", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

/**
 * Use an arbitrary choice of axes for a usable rotation attribute directly out of this node.
 */
static math::Quaternion normal_to_rotation(const float3 normal)
{
  float quat[4];
  vec_to_quat(quat, normal, OB_NEGZ, OB_POSY);
  return math::normalize(math::Quaternion(quat));
}

static OffsetIndices<int> calc_tri_point_offsets(const Mesh &mesh,
                                                 const Span<float> densities,
                                                 const int seed,
                                                 Array<int> &r_count_data)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();

  r_count_data = Array<int>(corner_tris.size() + 1);
  threading::parallel_for(corner_tris.index_range(), 1024, [&](const IndexRange range) {
    for (const int64_t tri_i : range) {
      const int3 &tri = corner_tris[tri_i];
      const float density = (densities[tri[0]] + densities[tri[1]] + densities[tri[2]]) / 3.0f;
      const float area = area_tri_v3(positions[corner_verts[tri[0]]],
                                     positions[corner_verts[tri[1]]],
                                     positions[corner_verts[tri[2]]]);
      const int corner_tri_seed = noise::hash(tri_i, seed);
      RandomNumberGenerator corner_tri_rng(corner_tri_seed);
      r_count_data[tri_i] = corner_tri_rng.round_probabilistic(area * density);
    }
  });

  return offset_indices::accumulate_counts_to_offsets(r_count_data);
}

static OffsetIndices<int> calc_tri_point_offsets(const Mesh &mesh,
                                                 const float density,
                                                 const int seed,
                                                 Array<int> &r_count_data)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();

  r_count_data = Array<int>(corner_tris.size() + 1);
  threading::parallel_for(corner_tris.index_range(), 1024, [&](const IndexRange range) {
    for (const int64_t tri_i : range) {
      const int3 &tri = corner_tris[tri_i];
      const float area = area_tri_v3(positions[corner_verts[tri[0]]],
                                     positions[corner_verts[tri[1]]],
                                     positions[corner_verts[tri[2]]]);
      const int corner_tri_seed = noise::hash(tri_i, seed);
      RandomNumberGenerator corner_tri_rng(corner_tri_seed);
      r_count_data[tri_i] = corner_tri_rng.round_probabilistic(area * density);
    }
  });

  return offset_indices::accumulate_counts_to_offsets(r_count_data);
}

static void sample_bary_coords(const Mesh &mesh,
                               const int seed,
                               const OffsetIndices<int> points_by_tri,
                               Vector<float3> &r_positions,
                               Vector<float3> &r_bary_coords,
                               Vector<int> &r_tri_indices)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();

  r_positions.resize(points_by_tri.total_size());
  r_bary_coords.resize(points_by_tri.total_size());
  r_tri_indices.resize(points_by_tri.total_size());

  threading::parallel_for(
      corner_tris.index_range(),
      4096,
      [&](const IndexRange range) {
        for (const int64_t tri_i : range) {
          const int3 &tri = corner_tris[tri_i];
          const float3 &v0_pos = positions[corner_verts[tri[0]]];
          const float3 &v1_pos = positions[corner_verts[tri[1]]];
          const float3 &v2_pos = positions[corner_verts[tri[2]]];

          const int corner_tri_seed = noise::hash(tri_i, seed);
          RandomNumberGenerator corner_tri_rng(corner_tri_seed);

          /* Retain legacy behavior. */
          corner_tri_rng.skip(1);

          for (const int i : points_by_tri[tri_i]) {
            const float3 bary_coord = corner_tri_rng.get_barycentric_coordinates();
            r_positions[i] = bke::attribute_math::mix3(bary_coord, v0_pos, v1_pos, v2_pos);
            r_bary_coords[i] = bary_coord;
            r_tri_indices[i] = tri_i;
          }
        }
      },
      threading::accumulated_task_sizes(
          [&](const IndexRange range) { return points_by_tri[range].size(); }));
}

BLI_NOINLINE static void update_elimination_mask_for_close_points(
    Span<float3> positions, const float minimum_distance, MutableSpan<bool> elimination_mask)
{
  if (minimum_distance <= 0.0f) {
    return;
  }

  const KDTreeNew<float3> tree(positions);

  for (const int i : positions.index_range()) {
    if (elimination_mask[i]) {
      continue;
    }

    tree.foreach_in_radius(
        positions[i], minimum_distance, [&](const int index, const float /*distance_sq*/) {
          if (index != i) {
            elimination_mask[index] = true;
          }
        });
  }
}

BLI_NOINLINE static void update_elimination_mask_based_on_density_factors(
    const Mesh &mesh,
    const Span<float> density_factors,
    const Span<float3> bary_coords,
    const Span<int> tri_indices,
    const MutableSpan<bool> elimination_mask)
{
  const Span<int3> corner_tris = mesh.corner_tris();
  for (const int i : bary_coords.index_range()) {
    if (elimination_mask[i]) {
      continue;
    }

    const int3 &tri = corner_tris[tri_indices[i]];
    const float3 bary_coord = bary_coords[i];

    const float v0_density_factor = density_factors[tri[0]];
    const float v1_density_factor = density_factors[tri[1]];
    const float v2_density_factor = density_factors[tri[2]];

    const float probability = v0_density_factor * bary_coord.x + v1_density_factor * bary_coord.y +
                              v2_density_factor * bary_coord.z;

    const float hash = noise::hash_float_to_float(bary_coord);
    if (hash > probability) {
      elimination_mask[i] = true;
    }
  }
}

BLI_NOINLINE static void eliminate_points_based_on_mask(const Span<bool> elimination_mask,
                                                        Vector<float3> &positions,
                                                        Vector<float3> &bary_coords,
                                                        Vector<int> &tri_indices)
{
  for (int i = positions.size() - 1; i >= 0; i--) {
    if (elimination_mask[i]) {
      positions.remove_and_reorder(i);
      bary_coords.remove_and_reorder(i);
      tri_indices.remove_and_reorder(i);
    }
  }
}

BLI_NOINLINE static void interpolate_attribute(const Mesh &mesh,
                                               const Span<float3> bary_coords,
                                               const Span<int> tri_indices,
                                               const AttrDomain source_domain,
                                               const GVArray &source_data,
                                               GMutableSpan output_data)
{
  switch (source_domain) {
    case AttrDomain::Point: {
      bke::mesh_surface_sample::sample_point_attribute(mesh.corner_verts(),
                                                       mesh.corner_tris(),
                                                       tri_indices,
                                                       bary_coords,
                                                       source_data,
                                                       IndexMask(output_data.size()),
                                                       output_data);
      break;
    }
    case AttrDomain::Corner: {
      bke::mesh_surface_sample::sample_corner_attribute(mesh.corner_tris(),
                                                        tri_indices,
                                                        bary_coords,
                                                        source_data,
                                                        IndexMask(output_data.size()),
                                                        output_data);
      break;
    }
    case AttrDomain::Face: {
      bke::mesh_surface_sample::sample_face_attribute(mesh.corner_tri_faces(),
                                                      tri_indices,
                                                      source_data,
                                                      IndexMask(output_data.size()),
                                                      output_data);
      break;
    }
    default: {
      /* Not supported currently. */
      return;
    }
  }
}

BLI_NOINLINE static void propagate_attributes(const Mesh &mesh,
                                              const bke::AttributeFilter &filter,
                                              PointCloud &points,
                                              const Span<float3> bary_coords,
                                              const Span<int> tri_indices)
{
  const AttributeAccessor mesh_attributes = mesh.attributes();
  MutableAttributeAccessor point_attributes = points.attributes_for_write();

  mesh_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.domain == AttrDomain::Edge) {
      return;
    }
    const StringRef name = iter.name;
    if (iter.is_builtin && !point_attributes.is_builtin(name)) {
      return;
    }
    if (ELEM(name, "position", ".select_vert")) {
      return;
    }
    if (filter.allow_skip(name)) {
      return;
    }
    const StringRef dst_name = name == ".select_poly" ? ".selection" : name;
    GAttributeReader src = iter.get();
    if (!src) {
      return;
    }
    const CommonVArrayInfo info = src.varray.common_info();
    if (info.type == CommonVArrayInfo::Type::Single) {
      const bke::AttributeInitValue init(GPointer(src.varray.type(), info.data));
      if (point_attributes.add(dst_name, AttrDomain::Point, iter.data_type, init)) {
        return;
      }
    }
    GSpanAttributeWriter dst = point_attributes.lookup_or_add_for_write_only_span(
        dst_name, AttrDomain::Point, iter.data_type);
    if (!dst) {
      return;
    }
    interpolate_attribute(mesh, bary_coords, tri_indices, src.domain, src.varray, dst.span);
    dst.finish();
  });
}

namespace {
struct AttributeOutputs {
  std::optional<std::string> normal_id;
  std::optional<std::string> rotation_id;
};
}  // namespace

static void compute_normal_outputs(const Mesh &mesh,
                                   const Span<float3> bary_coords,
                                   const Span<int> tri_indices,
                                   MutableSpan<float3> r_normals)
{
  switch (mesh.normals_domain()) {
    case bke::MeshNormalDomain::Point: {
      const Span<int> corner_verts = mesh.corner_verts();
      const Span<int3> corner_tris = mesh.corner_tris();
      const Span<float3> vert_normals = mesh.vert_normals();
      threading::parallel_for(bary_coords.index_range(), 512, [&](const IndexRange range) {
        bke::mesh_surface_sample::sample_point_normals(
            corner_verts, corner_tris, tri_indices, bary_coords, vert_normals, range, r_normals);
      });
      break;
    }
    case bke::MeshNormalDomain::Face: {
      const Span<int> tri_faces = mesh.corner_tri_faces();
      VArray<float3> face_normals = VArray<float3>::from_span(mesh.face_normals());
      threading::parallel_for(bary_coords.index_range(), 512, [&](const IndexRange range) {
        bke::mesh_surface_sample::sample_face_attribute(
            tri_faces, tri_indices, face_normals, range, r_normals);
      });
      break;
    }
    case bke::MeshNormalDomain::Corner: {
      const Span<int3> corner_tris = mesh.corner_tris();
      const Span<float3> corner_normals = mesh.corner_normals();
      threading::parallel_for(bary_coords.index_range(), 512, [&](const IndexRange range) {
        bke::mesh_surface_sample::sample_corner_normals(
            corner_tris, tri_indices, bary_coords, corner_normals, range, r_normals);
      });
      break;
    }
  }
}

static void compute_legacy_normal_outputs(const Mesh &mesh,
                                          const Span<float3> bary_coords,
                                          const Span<int> tri_indices,
                                          MutableSpan<float3> r_normals)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();

  for (const int i : bary_coords.index_range()) {
    const int tri_i = tri_indices[i];
    const int3 &tri = corner_tris[tri_i];

    const int v0_index = corner_verts[tri[0]];
    const int v1_index = corner_verts[tri[1]];
    const int v2_index = corner_verts[tri[2]];
    const float3 v0_pos = positions[v0_index];
    const float3 v1_pos = positions[v1_index];
    const float3 v2_pos = positions[v2_index];

    float3 normal;
    normal_tri_v3(normal, v0_pos, v1_pos, v2_pos);
    r_normals[i] = normal;
  }
}

static void compute_rotation_output(const Span<float3> normals,
                                    MutableSpan<math::Quaternion> r_rotations)
{
  threading::parallel_for(normals.index_range(), 512, [&](const IndexRange range) {
    for (const int i : range) {
      r_rotations[i] = normal_to_rotation(normals[i]);
    }
  });
}

BLI_NOINLINE static void compute_attribute_outputs(const Mesh &mesh,
                                                   PointCloud &points,
                                                   const Span<float3> bary_coords,
                                                   const Span<int> tri_indices,
                                                   const AttributeOutputs &attribute_outputs,
                                                   const bool use_legacy_normal)
{
  MutableAttributeAccessor point_attributes = points.attributes_for_write();

  SpanAttributeWriter<int> ids = point_attributes.lookup_or_add_for_write_only_span<int>(
      "id", AttrDomain::Point);

  SpanAttributeWriter<float3> normals;
  SpanAttributeWriter<math::Quaternion> rotations;

  if (attribute_outputs.normal_id) {
    normals = point_attributes.lookup_or_add_for_write_only_span<float3>(
        *attribute_outputs.normal_id, AttrDomain::Point);
  }
  if (attribute_outputs.rotation_id) {
    rotations = point_attributes.lookup_or_add_for_write_only_span<math::Quaternion>(
        *attribute_outputs.rotation_id, AttrDomain::Point);
  }

  threading::parallel_for(bary_coords.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const int tri_i = tri_indices[i];
      const float3 &bary_coord = bary_coords[i];
      ids.span[i] = noise::hash(noise::hash_float(bary_coord), tri_i);
    }
  });

  if (normals) {
    if (use_legacy_normal) {
      compute_legacy_normal_outputs(mesh, bary_coords, tri_indices, normals.span);
    }
    else {
      compute_normal_outputs(mesh, bary_coords, tri_indices, normals.span);
    }

    if (rotations) {
      compute_rotation_output(normals.span, rotations.span);
    }
  }

  ids.finish();
  normals.finish();
  rotations.finish();
}

static Array<float> calc_full_density_factors_with_selection(const Mesh &mesh,
                                                             const Field<float> &density_field,
                                                             const Field<bool> &selection_field)
{
  const AttrDomain domain = AttrDomain::Corner;
  const int domain_size = mesh.attributes().domain_size(domain);
  Array<float> densities(domain_size, 0.0f);

  const bke::MeshFieldContext field_context{mesh, domain};
  fn::FieldEvaluator evaluator{field_context, domain_size};
  evaluator.set_selection(selection_field);
  evaluator.add_with_destination(density_field, densities.as_mutable_span());
  evaluator.evaluate();
  return densities;
}

/* Sampling only depends on these mesh arrays. Holding references forces subsequent writes
 * to detach them, including when an evaluated mesh is replaced between frames. Normals and
 * arbitrary propagated attributes are always read from the current evaluation below. */
struct SampleKey : GenericKey {
  ImplicitSharingPtr<> positions_owner, corners_owner;
  SharedCache<Array<int3>> triangulation;
  Span<float3> positions;
  Span<int> corners;
  Span<int3> tris;
  int seed;
  /* Nonnegative values are block offsets. Whole-surface modes also differ in their key. */
  static constexpr int uniform_random = -2;
  int first = -1;
  float density_max = 0.0f, distance = 0.0f;
  Vector<float> densities;
  uint32_t density_hash = 0;

  SampleKey(const Mesh &mesh, const Span<int3> tris, const int seed)
      : triangulation(mesh.runtime->corner_tris_cache.data),
        positions(mesh.vert_positions()),
        corners(mesh.corner_verts()),
        tris(tris),
        seed(seed)
  {
    const auto retain = [](const bke::GAttributeReader &attr,
                           const void *data) -> ImplicitSharingPtr<> {
      if (!attr || !attr.sharing_info || !attr.varray.is_span() ||
          attr.varray.get_internal_span().data() != data)
      {
        return {};
      }
      attr.sharing_info->add_user();
      return ImplicitSharingPtr<>(attr.sharing_info);
    };
    const bke::AttributeAccessor attributes = mesh.attributes();
    positions_owner = retain(attributes.lookup("position"), positions.data());
    corners_owner = retain(attributes.lookup(".corner_vert"), corners.data());
  }

  uint64_t hash() const override
  {
    return get_default_hash(get_default_hash(positions.data(), corners.data(), tris.data(), seed),
                            tris.size(),
                            first,
                            density_max,
                            distance,
                            density_hash);
  }
  bool equal_to(const GenericKey &key) const override
  {
    const auto *other = dynamic_cast<const SampleKey *>(&key);
    return other && positions.data() == other->positions.data() &&
           corners.data() == other->corners.data() && tris.data() == other->tris.data() &&
           tris.size() == other->tris.size() && seed == other->seed && first == other->first &&
           density_max == other->density_max && distance == other->distance &&
           densities.size() == other->densities.size() &&
           (densities.is_empty() ||
            std::memcmp(densities.data(), other->densities.data(), densities.size() * sizeof(float)) == 0);
  }
  std::unique_ptr<GenericKey> to_storable() const override
  {
    return std::make_unique<SampleKey>(*this);
  }
};

struct Samples : memory_cache::CachedValue {
  Vector<float3> positions, bary_coords;
  Vector<int> tris;
  Array<bool> eliminated;
  int64_t input_bytes = 0;

  void count_memory(MemoryCounter &memory) const override
  {
    memory.add(input_bytes + (positions.capacity() + bary_coords.capacity()) * sizeof(float3) +
               tris.capacity() * sizeof(int) + eliminated.size());
  }
};

struct UniformRandomSamples : memory_cache::CachedValue {
  ImplicitSharingPtr<ImplicitSharedValue<Vector<float3>>> positions;
  Vector<float3> bary_coords;
  Vector<int> tris;
  SharedCache<bke::bvh::Tree> bvh_cache;
  int64_t input_bytes = 0;

  void count_memory(MemoryCounter &memory) const override
  {
    const int64_t position_capacity = positions ? positions->data.capacity() : 0;
    /* Reserve budget for the lazily created BVH, including external allocations. */
    const int64_t bvh_bytes = tris.size() * int64_t(256);
    memory.add(sizeof(*this) + input_bytes + bvh_bytes +
               (position_capacity + bary_coords.capacity()) * sizeof(float3) +
               tris.capacity() * sizeof(int));
  }
};

static bool use_uniform_scatter_cache()
{
  static const bool enabled = [] {
    const char *value = std::getenv("BLENDER_GN_UNIFORM_SCATTER_CACHE");
    return value == nullptr || !STREQ(value, "0");
  }();
  return enabled;
}

static bool use_sample_cache(const Mesh &mesh)
{
  static const bool enabled = [] {
    const char *value = std::getenv("BLENDER_GN_SCATTER_CACHE");
    return value == nullptr || !STREQ(value, "0");
  }();
  return enabled && mesh.corners_num >= 4096;
}

static void sample_random_cached_blocks(const SampleKey &surface,
                                        const Span<float> densities,
                                        const float uniform_density,
                                        Vector<float3> &positions,
                                        Vector<float3> &bary_coords,
                                        Vector<int> &tri_indices)
{
  const Span<int3> triangles = surface.tris;
  const int seed = surface.seed;
  constexpr int block_size = 1024;
  const bool log = CLOG_CHECK(&CACHE_LOG, CLG_LEVEL_INFO);
  std::atomic<int> built_blocks = 0;
  Array<std::shared_ptr<const Samples>> blocks((triangles.size() + block_size - 1) / block_size);
  threading::parallel_for(blocks.index_range(), 1, [&](IndexRange range) {
    for (const int block_index : range) {
      const int first = block_index * block_size;
      const Span<int3> block = triangles.drop_front(first).take_front(block_size);
      SampleKey key = surface;
      key.first = first;
      key.density_max = uniform_density;
      if (!densities.is_empty()) {
        key.densities.resize(block.size());
        bool all_zero = true;
        for (const int triangle_index : block.index_range()) {
          const int3 tri = block[triangle_index];
          const float density = (densities[tri.x] + densities[tri.y] + densities[tri.z]) / 3.0f;
          key.densities[triangle_index] = density;
          all_zero &= density == 0.0f;
        }
        if (all_zero) {
          continue;
        }
        key.density_hash = BLI_hash_mm3(reinterpret_cast<const unsigned char *>(key.densities.data()),
                                        key.densities.size() * sizeof(float), 0);
      }
      blocks[block_index] = memory_cache::get<Samples>(key, [&] {
        if (log) {
          built_blocks.fetch_add(1, std::memory_order_relaxed);
        }
        auto result = std::make_unique<Samples>();
        result->input_bytes = sizeof(key) + key.densities.size() * sizeof(float) +
                              block.size() * (sizeof(int3) + 3 * (sizeof(int) + sizeof(float3)));
        for (const int triangle_index : block.index_range()) {
          const int3 tri = block[triangle_index];
          const float density = key.densities.is_empty() ? uniform_density :
                                                         key.densities[triangle_index];
          if (density == 0.0f) {
            continue;
          }
          const float3 &v0 = surface.positions[surface.corners[tri.x]];
          const float3 &v1 = surface.positions[surface.corners[tri.y]];
          const float3 &v2 = surface.positions[surface.corners[tri.z]];
          const float area = area_tri_v3(v0, v1, v2);
          RandomNumberGenerator rng(noise::hash(first + triangle_index, seed));
          const int count = rng.round_probabilistic(area * density);
          /* round_probabilistic consumes the legacy skip(1) before barycentric sampling. */
          for ([[maybe_unused]] const int point : IndexRange(count)) {
            const float3 bary = rng.get_barycentric_coordinates();
            result->bary_coords.append(bary);
            result->positions.append(bke::attribute_math::mix3(bary, v0, v1, v2));
            result->tris.append(first + triangle_index);
          }
        }
        return result;
      });
    }
  });
  Array<int> offsets(blocks.size() + 1);
  int empty_blocks = 0;
  for (const int i : blocks.index_range()) {
    offsets[i] = blocks[i] ? int(blocks[i]->tris.size()) : 0;
    empty_blocks += !blocks[i];
  }
  CLOG_INFO(&CACHE_LOG,
            "Random triangles=%lld blocks=%lld empty=%d computed=%d",
            static_cast<long long>(triangles.size()),
            static_cast<long long>(blocks.size()),
            empty_blocks,
            built_blocks.load(std::memory_order_relaxed));
  const OffsetIndices points = offset_indices::accumulate_counts_to_offsets(offsets);
  positions.resize(points.total_size());
  bary_coords.resize(points.total_size());
  tri_indices.resize(points.total_size());
  threading::parallel_for(blocks.index_range(), 8, [&](IndexRange range) {
    for (const int i : range) {
      if (points[i].is_empty()) {
        continue;
      }
      positions.as_mutable_span().slice(points[i]).copy_from(blocks[i]->positions);
      bary_coords.as_mutable_span().slice(points[i]).copy_from(blocks[i]->bary_coords);
      tri_indices.as_mutable_span().slice(points[i]).copy_from(blocks[i]->tris);
    }
  });
}

static bool sample_random_cached(const Mesh &mesh,
                                 const int seed,
                                 const Span<float> densities,
                                 const float uniform_density,
                                 Vector<float3> &positions,
                                 Vector<float3> &bary_coords,
                                 Vector<int> &tri_indices,
                                 std::shared_ptr<const UniformRandomSamples> &r_uniform_samples)
{
  SampleKey surface(mesh, mesh.corner_tris(), seed);
  if (!surface.positions_owner || !surface.corners_owner) {
    return false;
  }
  if (densities.is_empty() && use_uniform_scatter_cache()) {
    SampleKey key = surface;
    key.first = SampleKey::uniform_random;
    key.density_max = uniform_density;
    bool assembled = false;
    const auto samples = memory_cache::get<UniformRandomSamples>(key, [&] {
      auto result = std::make_unique<UniformRandomSamples>();
      result->input_bytes = sizeof(SampleKey) + key.positions.size_in_bytes() +
                            key.corners.size_in_bytes() + key.tris.size_in_bytes();
      sample_random_cached_blocks(
          surface, densities, uniform_density, positions, bary_coords, tri_indices);
      assembled = true;
      if (positions.size() <= 128 * 1024) {
        result->positions = ImplicitSharingPtr(
            new ImplicitSharedValue<Vector<float3>>(std::move(positions)));
        result->bary_coords = std::move(bary_coords);
        result->tris = std::move(tri_indices);
      }
      return result;
    });
    CLOG_INFO(&CACHE_LOG,
              "Random uniform points=%lld reused=%d layout=%d",
              static_cast<long long>(samples->positions ? samples->tris.size() : positions.size()),
              int(!assembled),
              int(bool(samples->positions)));
    if (samples->positions) {
      r_uniform_samples = samples;
      return true;
    }
    if (assembled) {
      /* An oversized layout was already assembled for this evaluation. Keep just its blocks in
       * the cache, so another evaluation can cheaply determine that it must assemble them. */
      return true;
    }
  }
  sample_random_cached_blocks(
      surface, densities, uniform_density, positions, bary_coords, tri_indices);
  return true;
}

static PointCloud *create_points_random(const Mesh &mesh,
                                        const Field<bool> &selection_field,
                                        const Field<float> &density_field,
                                        const int seed,
                                        const AttributeOutputs &attribute_outputs,
                                        const bke::AttributeFilter &attribute_filter,
                                        const bool use_legacy_normal)
{
  const bool log = CLOG_CHECK(&CACHE_LOG, CLG_LEVEL_INFO);
  const auto start = log ? timeit::Clock::now() : timeit::TimePoint();
  Array<float> densities;
  float uniform_density = 0.0f;
  if (selection_field.depends_on_input() || density_field.depends_on_input()) {
    densities = calc_full_density_factors_with_selection(
        mesh, density_field, selection_field);
  }
  else {
    uniform_density = fn::evaluate_constant_field<float>(density_field);
    if (uniform_density == 0.0f) {
      return nullptr;
    }
  }

  const auto after_field = log ? timeit::Clock::now() : timeit::TimePoint();
  Vector<float3> positions;
  Vector<float3> bary_coords;
  Vector<int> tri_indices;
  std::shared_ptr<const UniformRandomSamples> uniform_samples;
  const bool cached = use_sample_cache(mesh) && sample_random_cached(mesh,
                                                                     seed,
                                                                     densities,
                                                                     uniform_density,
                                                                     positions,
                                                                     bary_coords,
                                                                     tri_indices,
                                                                     uniform_samples);
  if (!cached) {
    Array<int> count_data;
    const OffsetIndices points_by_tri = densities.is_empty() ?
                                            calc_tri_point_offsets(mesh, uniform_density, seed, count_data) :
                                            calc_tri_point_offsets(mesh, densities, seed, count_data);
    sample_bary_coords(mesh, seed, points_by_tri, positions, bary_coords, tri_indices);
  }
  const int64_t points_num = uniform_samples ? uniform_samples->tris.size() : positions.size();
  CLOG_INFO(&CACHE_LOG,
            "Random field_ms=%.3f sample_ms=%.3f points=%lld cache_path=%d",
            std::chrono::duration<double, std::milli>(after_field - start).count(),
            std::chrono::duration<double, std::milli>(timeit::Clock::now() - after_field).count(),
            static_cast<long long>(points_num),
            int(cached));
  if (points_num == 0) {
    return nullptr;
  }

  PointCloud *pointcloud = bke::pointcloud_new_no_attributes(PointCloudType::Points, points_num);
  bke::MutableAttributeAccessor point_attributes = pointcloud->attributes_for_write();
  if (uniform_samples) {
    const auto &shared_positions = *uniform_samples->positions;
    point_attributes.add<float3>("position",
                                 bke::AttrDomain::Point,
                                 bke::AttributeInitShared(shared_positions.data.data(),
                                                          shared_positions));
    /* The BVH references those exact positions. Position edits detach both through Blender's
     * existing attribute and runtime cache invalidation. */
    pointcloud->runtime->bvh_cache = uniform_samples->bvh_cache;
  }
  else {
    VectorData<float3, GuardedAllocator> positions_data = positions.release();
    const auto *attr_data = implicit_sharing::info_for_mem_free(positions_data.data);
    point_attributes.add<float3>("position",
                                 bke::AttrDomain::Point,
                                 bke::AttributeInitShared(positions_data.data, *attr_data));
    attr_data->remove_user_and_delete_if_last();
  }
  point_attributes.add<float>("radius", bke::AttrDomain::Point, bke::AttributeInitValue(0.05f));

  const Span<float3> sampled_bary = uniform_samples ? uniform_samples->bary_coords : bary_coords;
  const Span<int> sampled_tris = uniform_samples ? uniform_samples->tris : tri_indices;
  propagate_attributes(mesh, attribute_filter, *pointcloud, sampled_bary, sampled_tris);

  compute_attribute_outputs(
      mesh, *pointcloud, sampled_bary, sampled_tris, attribute_outputs, use_legacy_normal);

  geometry::debug_randomize_point_order(pointcloud);

  return pointcloud;
}

static PointCloud *create_points_poisson_disk(const Mesh &mesh,
                                              const Field<bool> &selection_field,
                                              const Field<float> &density_factor_field,
                                              const float minimum_distance,
                                              const float density_max,
                                              const int seed,
                                              const AttributeOutputs &attribute_outputs,
                                              const bke::AttributeFilter &attribute_filter,
                                              const bool use_legacy_normal)
{
  if (density_max <= 0.0f) {
    return nullptr;
  }
  bool computed = false;
  auto prepare = [&] {
    computed = true;
    auto s = std::make_unique<Samples>();
    Array<int> counts;
    const OffsetIndices points_by_tri = calc_tri_point_offsets(mesh, density_max, seed, counts);
    sample_bary_coords(mesh, seed, points_by_tri, s->positions, s->bary_coords, s->tris);
    s->eliminated = Array<bool>(s->positions.size(), false);
    if (!s->positions.is_empty()) {
      update_elimination_mask_for_close_points(s->positions, minimum_distance, s->eliminated);
    }
    s->input_bytes = mesh.verts_num*sizeof(float3) + mesh.corners_num*sizeof(int) +
                     mesh.corner_tris().size_in_bytes();
    return s;
  };
  Samples working;
  if (use_sample_cache(mesh)) {
    SampleKey key(mesh, mesh.corner_tris(), seed);
    key.distance = minimum_distance;
    key.density_max = density_max;
    if (key.positions_owner && key.corners_owner) {
      working = *memory_cache::get<Samples>(key, prepare);
    }
    else {
      working = std::move(*prepare());
    }
  }
  else {
    working = std::move(*prepare());
  }
  auto &positions = working.positions;
  auto &bary_coords = working.bary_coords;
  auto &tri_indices = working.tris;
  auto &elimination_mask = working.eliminated;
  CLOG_INFO(&CACHE_LOG, "Poisson candidates=%lld computed=%d", static_cast<long long>(positions.size()), int(computed));
  if (positions.is_empty()) {
    return nullptr;
  }

  const Array<float> density_factors = calc_full_density_factors_with_selection(
      mesh, density_factor_field, selection_field);

  update_elimination_mask_based_on_density_factors(
      mesh, density_factors, bary_coords, tri_indices, elimination_mask.as_mutable_span());

  eliminate_points_based_on_mask(elimination_mask.as_span(), positions, bary_coords, tri_indices);

  if (positions.is_empty()) {
    return nullptr;
  }

  PointCloud *pointcloud = bke::pointcloud_new_no_attributes(PointCloudType::Points,
                                                             positions.size());
  bke::MutableAttributeAccessor point_attributes = pointcloud->attributes_for_write();
  pointcloud->positions_for_write().copy_from(positions);
  point_attributes.add<float>("radius", bke::AttrDomain::Point, bke::AttributeInitValue(0.05f));

  propagate_attributes(mesh, attribute_filter, *pointcloud, bary_coords, tri_indices);

  compute_attribute_outputs(
      mesh, *pointcloud, bary_coords, tri_indices, attribute_outputs, use_legacy_normal);

  geometry::debug_randomize_point_order(pointcloud);

  return pointcloud;
}

static Field<float> extract_non_negative_density(GeoNodeExecParams &params, const UString input)
{
  const static mf::MultiFunction &max_fn = fn::multi_function::registry::lookup(
      "max(float, float)"_ustr);
  return Field<float>(FieldOperation::from(
      max_fn, {params.extract_input<Field<float>>(input), fn::Field<float>(0.0f)}));
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh"_ustr);

  const Field<bool> selection = params.extract_input<Field<bool>>("Selection"_ustr);
  const int seed = params.extract_input<int>("Seed"_ustr) * 5383843;
  const NodeAttributeFilter attribute_filter = params.get_attribute_filter("Points"_ustr);
  const bool use_legacy_normal = params.node().custom2 != 0;

  AttributeOutputs attribute_outputs;
  attribute_outputs.rotation_id = params.get_output_anonymous_attribute_id_if_needed(
      "Rotation"_ustr);
  attribute_outputs.normal_id = params.get_output_anonymous_attribute_id_if_needed(
      "Normal"_ustr, bool(attribute_outputs.rotation_id));

  lazy_threading::send_hint();

  switch (GeometryNodeDistributePointsOnFacesMode(params.node().custom1)) {
    case GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_RANDOM: {
      const Field<float> density = extract_non_negative_density(params, "Density"_ustr);
      geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
        if (const Mesh *mesh = geometry_set.get_mesh()) {
          PointCloud *pointcloud = create_points_random(*mesh,
                                                        selection,
                                                        density,
                                                        seed,
                                                        attribute_outputs,
                                                        attribute_filter,
                                                        use_legacy_normal);
          geometry_set.replace_pointcloud(pointcloud);
        }
        geometry_set.keep_only(
            {GeometryComponent::Type::PointCloud, GeometryComponent::Type::Edit});
      });
      break;
    }
    case GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_POISSON: {
      const Field<float> factors = extract_non_negative_density(params, "Density Factor"_ustr);
      const float minimum_distance = params.extract_input<float>("Distance Min"_ustr);
      const float density_max = params.extract_input<float>("Density Max"_ustr);
      geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
        if (const Mesh *mesh = geometry_set.get_mesh()) {
          PointCloud *pointcloud = create_points_poisson_disk(*mesh,
                                                              selection,
                                                              factors,
                                                              minimum_distance,
                                                              density_max,
                                                              seed,
                                                              attribute_outputs,
                                                              attribute_filter,
                                                              use_legacy_normal);
          geometry_set.replace_pointcloud(pointcloud);
          geometry_set.keep_only(
              {GeometryComponent::Type::PointCloud, GeometryComponent::Type::Edit});
        }
      });
      break;
    }
  }

  params.set_output("Points"_ustr, std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(
      &ntype, "GeometryNodeDistributePointsOnFaces"_ustr, GEO_NODE_DISTRIBUTE_POINTS_ON_FACES);
  ntype.ui_name = "Distribute Points on Faces";
  ntype.ui_description = "Generate points spread out on the surface of a mesh";
  ntype.enum_name_legacy = "DISTRIBUTE_POINTS_ON_FACES";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.default_width = bke::NodeWidth::_180;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_distribute_points_on_faces_cc
