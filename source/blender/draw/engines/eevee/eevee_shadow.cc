/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * The shadow module manages shadow update tagging & shadow rendering.
 */

#include "BLI_math_matrix.hh"
#include "BLI_time.h"

#include "DNA_light_types.h"
#include "DNA_object_types.h"

#include "GPU_batch_utils.hh"
#include "GPU_capabilities.hh"
#include "GPU_compute.hh"

#include "GPU_context.hh"
#include "eevee_instance.hh"

#include "GPU_debug.hh"
#include "draw_cache.hh"
#include "draw_debug.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>

namespace blender::eevee {

namespace {

static constexpr int shadow_pool_retry_cooldown = 120;

struct DirectionalFocusData {
  float3 position = float3(0.0f);
  float distance = 0.0f;
  float blend = 0.0f;
};

static std::unordered_map<const ShadowModule *, DirectionalFocusData> directional_focus_cache;

static DirectionalFocusData &directional_focus_data_ensure(const ShadowModule &shadows)
{
  return directional_focus_cache[&shadows];
}

static const char *debug_light_type_name_get(const eLightType type)
{
  switch (type) {
    case LIGHT_SUN:
    case LIGHT_SUN_ORTHO:
      return "Sun";
    case LIGHT_RECT:
    case LIGHT_ELLIPSE:
      return "Area";
    case LIGHT_SPOT_SPHERE:
    case LIGHT_SPOT_DISK:
      return "Spot";
    case LIGHT_OMNI_SPHERE:
    case LIGHT_OMNI_DISK:
      return "Point";
  }
  return "Unknown";
}

static float finite_or_default(const float value, const float fallback)
{
  return std::isfinite(value) ? value : fallback;
}

}  // namespace

ShadowTechnique ShadowModule::shadow_technique = ShadowTechnique::ATOMIC_RASTER;

/* -------------------------------------------------------------------- */
/** \name Tile map
 *
 * \{ */

void ShadowTileMap::sync_orthographic(const float4x4 &object_mat_,
                                      int2 origin_offset,
                                      int clipmap_level,
                                      eShadowProjectionType projection_type_,
                                      uint2 shadow_set_membership_)
{
  if ((projection_type != projection_type_) || (level != clipmap_level) ||
      (shadow_set_membership_ != shadow_set_membership))
  {
    set_dirty();
  }
  projection_type = projection_type_;
  level = clipmap_level;
  light_type = eLightType::LIGHT_SUN;
  shadow_set_membership = shadow_set_membership_;

  grid_shift = origin_offset - grid_offset;
  grid_offset = origin_offset;

  if (!equals_m4m4(object_mat.ptr(), object_mat_.ptr())) {
    object_mat = object_mat_;
    set_dirty();
  }

  float tile_size = ShadowDirectional::tile_size_get(level);

  /* object_mat is a rotation matrix. Reduce imprecision by taking the transpose which is also the
   * inverse in this particular case. */
  viewmat = math::transpose(object_mat);

  half_size = ShadowDirectional::coverage_get(level) / 2.0f;
  center_offset = float2(grid_offset) * tile_size;

  winmat = math::projection::orthographic(-half_size + center_offset.x,
                                          half_size + center_offset.x,
                                          -half_size + center_offset.y,
                                          half_size + center_offset.y,
                                          /* Near/far is computed on GPU using casters bounds. */
                                          -1.0f,
                                          1.0f);
}

void ShadowTileMap::sync_cubeface(eLightType light_type_,
                                  const float4x4 &object_mat_,
                                  float near_,
                                  float far_,
                                  eCubeFace face,
                                  uint2 shadow_set_membership_)
{
  if (projection_type != SHADOW_PROJECTION_CUBEFACE || (cubeface != face) ||
      (shadow_set_membership_ != shadow_set_membership))
  {
    set_dirty();
  }
  projection_type = SHADOW_PROJECTION_CUBEFACE;
  cubeface = face;
  grid_offset = int2(0);
  light_type = light_type_;
  shadow_set_membership = shadow_set_membership_;

  if ((clip_near != near_) || (clip_far != far_)) {
    set_dirty();
  }

  clip_near = near_;
  half_size = near_;
  clip_far = far_;
  center_offset = float2(0.0f);

  if (!equals_m4m4(object_mat.ptr(), object_mat_.ptr())) {
    object_mat = object_mat_;
    set_dirty();
  }

  winmat = math::projection::perspective(
      -half_size, half_size, -half_size, half_size, clip_near, clip_far);
  viewmat = float4x4(float3x3(shadow_face_mat[cubeface])) * math::invert(object_mat);

  /* Same thing as inversion but avoid precision issues. */
  float4x4 viewinv = object_mat * float4x4(math::transpose(float3x3(shadow_face_mat[cubeface])));
  /* Update corners. */
  corners[0] = float4(viewinv.location(), 0.0f);
  corners[1] = float4(math::transform_point(viewinv, float3(-far_, -far_, -far_)), 0.0f);
  corners[2] = float4(math::transform_point(viewinv, float3(far_, -far_, -far_)), 0.0f);
  corners[3] = float4(math::transform_point(viewinv, float3(-far_, far_, -far_)), 0.0f);
  /* Store deltas. */
  corners[2] = (corners[2] - corners[1]) / float(SHADOW_TILEMAP_RES);
  corners[3] = (corners[3] - corners[1]) / float(SHADOW_TILEMAP_RES);
}

void ShadowTileMap::debug_draw() const
{
  /** Used for debug drawing. */
  const float4 debug_color[6] = {
      {1.0f, 0.1f, 0.1f, 1.0f},
      {0.1f, 1.0f, 0.1f, 1.0f},
      {0.0f, 0.2f, 1.0f, 1.0f},
      {1.0f, 1.0f, 0.3f, 1.0f},
      {0.1f, 0.1f, 0.1f, 1.0f},
      {1.0f, 1.0f, 1.0f, 1.0f},
  };
  float4 color = debug_color
      [((projection_type == SHADOW_PROJECTION_CUBEFACE ? int(cubeface) : level) + 9999) % 6];

  float4x4 persinv = winmat * viewmat;
  drw_debug_matrix_as_bbox(math::invert(persinv), color);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile map pool
 *
 * \{ */

ShadowTileMapPool::ShadowTileMapPool()
{
  free_indices.reserve(SHADOW_MAX_TILEMAP);
  /* Reverse order to help debugging (first allocated tile-map will get 0). */
  for (int i = SHADOW_MAX_TILEMAP - 1; i >= 0; i--) {
    free_indices.append(i * SHADOW_TILEDATA_PER_TILEMAP);
  }
  /* The initial free-list has never owned pages. Only later growth relative to this baseline
   * represents released tile-maps that need GPU-side page cleanup. */
  last_free_len = free_indices.size();

  int2 extent;
  extent.x = min_ii(SHADOW_MAX_TILEMAP, maps_per_row) * ShadowTileMap::tile_map_resolution;
  extent.y = (SHADOW_MAX_TILEMAP / maps_per_row) * ShadowTileMap::tile_map_resolution;

  eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE |
                           GPU_TEXTURE_USAGE_ATTACHMENT;
  tilemap_tx.ensure_2d(gpu::TextureFormat::UINT_32, extent, usage);
  tilemap_tx.clear(uint4(0));
}

ShadowTileMap *ShadowTileMapPool::acquire()
{
  if (free_indices.is_empty()) {
    /* Grow the tile-map buffer. See `end_sync`. */
    for (auto i : IndexRange(free_indices.size(), SHADOW_MAX_TILEMAP)) {
      free_indices.append(i * SHADOW_TILEDATA_PER_TILEMAP);
    }
  }
  int index = free_indices.pop_last();
  return &tilemap_pool.construct(ShadowTileMap(index));
}

void ShadowTileMapPool::release(Span<ShadowTileMap *> free_list)
{
  for (ShadowTileMap *map : free_list) {
    free_indices.append(map->tiles_index);
    tilemap_pool.destruct(*map);
  }
}

void ShadowTileMapPool::end_sync(ShadowModule &module)
{
  tilemaps_data.push_update();

  uint needed_tilemap_capacity = (free_indices.size() + tilemap_pool.size());
  if (needed_tilemap_capacity != (tiles_data.size() / SHADOW_TILEDATA_PER_TILEMAP)) {
    tiles_data.resize(needed_tilemap_capacity * SHADOW_TILEDATA_PER_TILEMAP);
    tilemaps_clip.resize(needed_tilemap_capacity);
    /* We reallocated the tile-map buffer, discarding all the data it contained.
     * We need to re-initialize the page heaps. */
    module.do_full_update_ = true;
  }

  tilemaps_unused.clear();
  int64_t newly_unused_count = free_indices.size() - last_free_len;
  if (newly_unused_count > 0) {
    /* Upload tile-map indices which pages needs to be pushed back to the free page heap. */
    Span<uint> newly_unused_indices = free_indices.as_span().slice(last_free_len,
                                                                   newly_unused_count);
    for (uint index : newly_unused_indices) {
      /* Push a dummy tile-map to a unused tile-map buffer. It is then processed through the some
       * of the setup steps to release the pages. */
      ShadowTileMapData tilemap_data = {};
      tilemap_data.tiles_index = index;
      tilemap_data.clip_data_index = -1;
      tilemap_data.grid_shift = int2(SHADOW_TILEMAP_RES);
      tilemap_data.projection_type = SHADOW_PROJECTION_CUBEFACE;

      tilemaps_unused.append(tilemap_data);
    }
    tilemaps_unused.push_update();
  }

  last_free_len = free_indices.size();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow Punctual
 *
 * \{ */

void ShadowPunctual::release_excess_tilemaps(const Light &light)
{
  int tilemaps_needed = light.local_tilemap_count();
  if (tilemaps_.size() <= tilemaps_needed) {
    return;
  }
  auto span = tilemaps_.as_span();
  shadows_.tilemap_pool.release(span.drop_front(tilemaps_needed));
  tilemaps_ = span.take_front(tilemaps_needed);
}

void ShadowPunctual::end_sync(Light &light)
{
  ShadowTileMapPool &tilemap_pool = shadows_.tilemap_pool;

  float4x4 object_to_world = light.object_to_world;

  /* Acquire missing tile-maps. */
  int tilemaps_needed = light.local_tilemap_count();
  while (tilemaps_.size() < tilemaps_needed) {
    tilemaps_.append(tilemap_pool.acquire());
  }

  float near = int_as_float(light.clip_near);
  float far = int_as_float(light.clip_far);
  for (int i : tilemaps_.index_range()) {
    eCubeFace face = eCubeFace(Z_NEG + i);
    tilemaps_[face]->sync_cubeface(
        light.type, object_to_world, near, far, face, light.shadow_set_membership);
  }

  light.local().tilemaps_count = tilemaps_needed;
  light.tilemap_index = tilemap_pool.tilemaps_data.size();
  for (ShadowTileMap *tilemap : tilemaps_) {
    /* Add shadow tile-maps grouped by lights to the GPU buffer. */
    tilemap_pool.tilemaps_data.append(*tilemap);
    tilemap->set_updated();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Directional Shadow Maps
 *
 * In order to improve shadow map density, we switch between two tile-map distribution mode.
 * One is beater suited for large FOV (clip-map), the other for really small FOV or Orthographic
 * projections (cascade).
 *
 * Clip-map distribution centers a number of log2 sized tile-maps around the view position.
 * https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry
 *
 * Cascade distribution puts tile-maps along the frustum projection to the light space.
 * https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus
 *
 * We choose to distribute cascades linearly to achieve uniform density and simplify lookup.
 * Using clip-map instead of cascades for perspective view also allows for better caching.
 * \{ */

eShadowProjectionType ShadowDirectional::directional_distribution_type_get(const Camera &camera)
{
  if (camera.is_perspective()) {
    /* Narrow FOV (< ~45°): cascade is more efficient than clipmap.
     * screen_diagonal_length ≈ 2*tan(diag_fov/2). Threshold 1.2 ≈ 45° hfov. */
    const CameraData &cam_data = camera.data_get();
    if (finite_or_default(cam_data.screen_diagonal_length, 1.0f) < 1.2f) {
      return SHADOW_PROJECTION_CASCADE;
    }
    return SHADOW_PROJECTION_CLIPMAP;
  }
  return SHADOW_PROJECTION_CASCADE;
}

static int clipmap_level_perspective_bias(const Camera &camera)
{
  if (!camera.is_perspective()) {
    return 0;
  }

  const CameraData &cam_data = camera.data_get();
  const float screen_diag = max_ff(finite_or_default(cam_data.screen_diagonal_length, 1.0f),
                                   1e-6f);
  if (screen_diag >= 1.0f) {
    return 0;
  }

  /* `screen_diagonal_length` is a normalized view-space frustum diagonal.
   * Values below 1 mean the visible frustum footprint is narrower than the unit depth used by the
   * default clipmap heuristic, which is exactly where telephoto views need finer directional LODs.
   */
  return clamp_i(int(floor(-log2(screen_diag))), 0, 8);
}

static void directional_focus_update(DirectionalFocusData &focus,
                                     const Camera &camera,
                                     const draw::StorageVectorBuffer<uint, 128> &curr_casters,
                                     const draw::Manager &manager)
{
  focus.position = camera.position();
  focus.distance = 0.0f;
  focus.blend = 0.0f;

  const int perspective_bias = clipmap_level_perspective_bias(camera);
  if (!camera.is_perspective() || (perspective_bias == 0) || curr_casters.is_empty()) {
    return;
  }

  const CameraData &cam_data = camera.data_get();
  const float screen_diag = max_ff(finite_or_default(cam_data.screen_diagonal_length, 1.0f),
                                   1e-6f);
  const float3 camera_position = camera.position();
  const float3 view_direction = -camera.forward();
  const auto &matrices = manager.matrix_buf.current();

  float best_score = FLT_MAX;
  float best_depth = 0.0f;
  bool found_focus = false;

  for (int64_t i : IndexRange(curr_casters.size())) {
    const uint resource_id = curr_casters[i];
    if (resource_id >= matrices.size()) {
      continue;
    }

    const float3 caster_center = matrices[resource_id].model.location();
    const float depth = math::dot(caster_center - camera_position, view_direction);
    if ((depth <= cam_data.clip_near) || (depth >= cam_data.clip_far)) {
      continue;
    }

    const float3 point_on_ray = camera_position + view_direction * depth;
    const float ray_distance_sq = math::distance_squared(caster_center, point_on_ray);
    const float projected_radius = max_ff(screen_diag * depth, 1e-4f);
    const float score = ray_distance_sq / (projected_radius * projected_radius) + depth * 1e-4f;

    if (score < best_score) {
      best_score = score;
      best_depth = depth;
      found_focus = true;
    }
  }

  if (!found_focus) {
    return;
  }

  focus.distance = best_depth;
  focus.position = camera_position + view_direction * best_depth;
  focus.blend = clamp_f(float(perspective_bias) / 3.0f, 0.0f, 1.0f);
}

/************************************************************************
 *                         Cascade Distribution                         *
 ************************************************************************/

void ShadowDirectional::cascade_tilemaps_distribution_near_far_points(const Camera &camera,
                                                                      const Light &light,
                                                                      float3 &near_point,
                                                                      float3 &far_point)
{
  const CameraData &cam_data = camera.data_get();
  const float3 ws_pos_shifted = camera.position() + camera.forward_shifted();
  /* Ideally we should only take the intersection with the scene bounds. */
  far_point = transform_direction_transposed(
      light.object_to_world, ws_pos_shifted - camera.forward() * cam_data.clip_far);
  near_point = transform_direction_transposed(
      light.object_to_world, ws_pos_shifted - camera.forward() * cam_data.clip_near);
}

ShadowDirectional::LevelSpan ShadowDirectional::cascade_level_range(const Light &light,
                                                                    const Camera &camera)
{
  /* NOTE: All tile-maps are meant to have the same LOD
   * but we still return a range starting at the unique LOD. */

  using namespace blender::math;

  /* 16 is arbitrary. To avoid too much tile-map per directional lights. */
  const int max_tilemap_per_shadows = 16;
  const CameraData &cam_data = camera.data_get();

  float3 near_point, far_point;
  cascade_tilemaps_distribution_near_far_points(camera, light, near_point, far_point);

  /* This gives the maximum resolution in depth we can have with a fixed set of tile-maps. Gives
   * the best results when view direction is orthogonal to the light direction. */
  float depth_range_in_shadow_space = distance(far_point.xy(), near_point.xy());
  depth_range_in_shadow_space = finite_or_default(depth_range_in_shadow_space, 0.0f);
  float min_depth_tilemap_size = 2 * (depth_range_in_shadow_space / max_tilemap_per_shadows);
  /* This allow coverage of the whole view with a single tile-map if camera forward is colinear
   * with the light direction. */
  float min_diagonal_tilemap_size = finite_or_default(cam_data.screen_diagonal_length, 1.0f);

  if (camera.is_perspective()) {
    /* Use the far plane diagonal if using perspective. */
    const float clip_near = std::max(finite_or_default(cam_data.clip_near, 0.01f), 0.01f);
    const float clip_far = std::max(finite_or_default(cam_data.clip_far, clip_near), clip_near);
    min_diagonal_tilemap_size *= clip_far / clip_near;
  }

  /* TODO(fclem): Zoomed in camera can have very small diagonal size which will then result in
   * negative lod_level. Since negative ranges are not supported inside `IndexRange` we have to
   * ensure this doesn't happen. */
  min_diagonal_tilemap_size = max(min_diagonal_tilemap_size, 0.5f);

  /* Level of detail (or size) of every tile-maps of this light. */
  /* TODO(fclem): Add support for lod bias from light. */
  int lod_level = ceil(log2(max_ff(min_depth_tilemap_size, min_diagonal_tilemap_size)) + 0.5);

  /* Tile-maps "rotate" around the first one so their effective range is only half their size. */
  float per_tilemap_coverage = ShadowDirectional::coverage_get(lod_level) * 0.5f;
  per_tilemap_coverage = std::max(finite_or_default(per_tilemap_coverage, 0.5f), 0.5f);
  /* Number of tile-maps needed to cover the whole view. */
  /* NOTE: floor + 0.5 to avoid 0 when parallel. */
  int tilemap_len = ceil(0.5f + depth_range_in_shadow_space / per_tilemap_coverage);
  return LevelSpan{lod_level, lod_level + max_ii(tilemap_len, 1) - 1};
}

void ShadowDirectional::cascade_tilemaps_distribution(Light &light, const Camera &camera)
{
  using namespace blender::math;

  float4x4 object_mat = light.object_to_world;
  object_mat.location() = float3(0.0f);
  light.lod_bias = shadows_.global_lod_bias();
  light.sun().focus_distance = 0.0f;
  light.sun().focus_blend = 0.0f;

  /* All tile-maps use the first level size. */
  float half_size = ShadowDirectional::coverage_get(levels_.lod_min) / 2.0f;
  float tile_size = ShadowDirectional::tile_size_get(levels_.lod_min);

  float3 near_point, far_point;
  cascade_tilemaps_distribution_near_far_points(camera, light, near_point, far_point);

  float2 local_view_direction = normalize(far_point.xy() - near_point.xy());
  float2 farthest_tilemap_center = local_view_direction * half_size * (levels_.size() - 1);

  /* Offset for smooth level transitions. */
  light.object_to_world.x.w = near_point.x;
  light.object_to_world.y.w = near_point.y;
  light.object_to_world.z.w = near_point.z;

  /* Offset in tiles from the scene origin to the center of the first tile-maps. */
  int2 origin_offset = int2(round(float2(near_point) / tile_size));
  /* Offset in tiles between the first and the last tile-maps. */
  int2 offset_vector = int2(round(farthest_tilemap_center / tile_size));

  light.sun().clipmap_base_offset_neg = int2(0); /* Unused. */
  light.sun().clipmap_base_offset_pos = (offset_vector * (1 << 16)) /
                                        max_ii(levels_.size() - 1, 1);

  /* \note cascade_level_range starts the range at the unique LOD to apply to all tile-maps. */
  int level = levels_.lod_min;
  for (int i : IndexRange(levels_.size())) {
    ShadowTileMap *tilemap = tilemaps_[i];

    /* Equal spacing between cascades layers since we want uniform shadow density. */
    int2 level_offset = origin_offset +
                        shadow_cascade_grid_offset(light.sun().clipmap_base_offset_pos, i);
    tilemap->sync_orthographic(object_mat,
                               level_offset,
                               level,
                               SHADOW_PROJECTION_CASCADE,
                               light.shadow_set_membership);

    /* Add shadow tile-maps grouped by lights to the GPU buffer. */
    shadows_.tilemap_pool.tilemaps_data.append(*tilemap);
    tilemap->set_updated();
  }

  light.sun().clipmap_origin = float2(origin_offset) * tile_size;

  light.type = LIGHT_SUN_ORTHO;

  /* Not really clip-maps, but this is in order to make #light_tilemap_max_get() work and determine
   * the scaling. */
  light.sun().clipmap_lod_min = levels_.lod_min;
  light.sun().clipmap_lod_max = levels_.lod_max;
}

/************************************************************************
 *                         Clip-map Distribution                        *
 ************************************************************************/

ShadowDirectional::LevelSpan ShadowDirectional::clipmap_level_range(const Camera &cam)
{
  using namespace blender::math;

  const CameraData &cam_data = cam.data_get();
  /* Covers the closest points of the view. */
  int min_level = floor(log2(max_ff(cam_data.clip_near, 1e-8f)));
  /* Covers the farthest points of the view. */
  int max_level = ceil(log2(cam.bound_radius() + distance(cam.bound_center(), cam.position())));

  /* We actually need to cover a bit more because of clipmap origin snapping. */
  max_level = max(min_level, max_level) + 1;
  LevelSpan span{min_level, max_level};
  /* 32 to be able to pack offset into a single int2.
   * The maximum level count is bounded by the mantissa of a 32bit float. */
  const int max_tilemap_per_shadows = 24;
  if (span.size() > max_tilemap_per_shadows) {
    /* Keep the coarsest levels to preserve wide coverage when we hit the tile-map budget. */
    span.lod_min = span.lod_max - (max_tilemap_per_shadows - 1);
  }

  return span;
}

void ShadowDirectional::clipmap_tilemaps_distribution(Light &light, const Camera &camera)
{
  const DirectionalFocusData &focus = directional_focus_data_ensure(shadows_);
  const float3 clipmap_center = math::interpolate(
      camera.position(), focus.position, focus.blend);

  float4x4 object_mat = light.object_to_world;
  object_mat.location() = float3(0.0f);
  light.lod_bias = shadows_.global_lod_bias();
  light.sun().focus_distance = focus.distance;
  light.sun().focus_blend = focus.blend;

  for (int lod : IndexRange(levels_.size())) {
    ShadowTileMap *tilemap = tilemaps_[lod];

    int level = levels_.lod_min + lod;
    /* Compute full offset from world origin to the smallest clipmap tile centered around the
     * clipmap focus point. The offset is computed in smallest tile unit. */
    float tile_size = ShadowDirectional::tile_size_get(level);
    /* Moving to light space by multiplying by the transpose (which is the inverse). */
    float2 light_space_center = clipmap_center * float2x3(object_mat.view<2, 3>());
    int2 level_offset = int2(math::round(light_space_center / tile_size));

    tilemap->sync_orthographic(object_mat,
                               level_offset,
                               level,
                               SHADOW_PROJECTION_CLIPMAP,
                               light.shadow_set_membership);

    /* Add shadow tile-maps grouped by lights to the GPU buffer. */
    shadows_.tilemap_pool.tilemaps_data.append(*tilemap);
    tilemap->set_updated();
  }

  int2 pos_offset = int2(0);
  int2 neg_offset = int2(0);
  for (int lod : IndexRange(levels_.size() - 1)) {
    /* Since offset can only differ by one tile from the higher level, we can compress that as a
     * single integer where one bit contains offset between 2 levels. Then a single bit shift in
     * the shader gives the number of tile to offset in the given tile-map space. However we need
     * also the sign of the offset for each level offset. To this end, we split the negative
     * offsets to a separate int. */
    int2 lvl_offset_next = tilemaps_[lod + 1]->grid_offset;
    int2 lvl_offset = tilemaps_[lod]->grid_offset;
    int2 lvl_delta = lvl_offset - (lvl_offset_next * 2);
    BLI_assert(math::abs(lvl_delta.x) <= 1 && math::abs(lvl_delta.y) <= 1);
    pos_offset |= math::max(lvl_delta, int2(0)) << lod;
    neg_offset |= math::max(-lvl_delta, int2(0)) << lod;
  }

  /* Number of levels is limited to 32 by `clipmap_level_range()` for this reason. */
  light.sun().clipmap_base_offset_pos = pos_offset;
  light.sun().clipmap_base_offset_neg = neg_offset;

  float tile_size_max = ShadowDirectional::tile_size_get(levels_.lod_max);
  int2 level_offset_max = tilemaps_[levels_.size() - 1]->grid_offset;

  light.type = LIGHT_SUN;

  /* Used for selecting the clipmap level. */
  float3 location = transform_direction_transposed(light.object_to_world, clipmap_center);
  /* Offset for smooth level transitions. */
  light.object_to_world.x.w = location.x;
  light.object_to_world.y.w = location.y;
  light.object_to_world.z.w = location.z;
  /* Used as origin for the clipmap_base_offset trick. */
  light.sun().clipmap_origin = float2(level_offset_max * tile_size_max);

  light.sun().clipmap_lod_min = levels_.lod_min;
  light.sun().clipmap_lod_max = levels_.lod_max;
}

void ShadowDirectional::release_excess_tilemaps(const Light &light, const Camera &camera)
{
  LevelSpan levels_new = directional_distribution_type_get(camera) == SHADOW_PROJECTION_CASCADE ?
                             cascade_level_range(light, camera) :
                             clipmap_level_range(camera);

  if (levels_ == levels_new) {
    return;
  }

  const int isect_min = max_ii(levels_.lod_min, levels_new.lod_min);
  const int isect_max = min_ii(levels_.lod_max, levels_new.lod_max);
  if (isect_max < isect_min) {
    shadows_.tilemap_pool.release(tilemaps_);
    tilemaps_.clear();
    levels_ = {};
    return;
  }

  const int before_count = isect_min - levels_.lod_min;
  const int isect_count = isect_max - isect_min + 1;
  const int after_count = levels_.lod_max - isect_max;

  auto span = tilemaps_.as_span();
  if (before_count > 0) {
    shadows_.tilemap_pool.release(span.take_front(before_count));
  }
  if (after_count > 0) {
    shadows_.tilemap_pool.release(span.take_back(after_count));
  }
  tilemaps_ = span.slice(before_count, isect_count);
  levels_ = LevelSpan{isect_min, isect_max};
}

void ShadowDirectional::end_sync(Light &light, const Camera &camera)
{
  ShadowTileMapPool &tilemap_pool = shadows_.tilemap_pool;
  release_excess_tilemaps(light, camera);
  LevelSpan levels_new = directional_distribution_type_get(camera) == SHADOW_PROJECTION_CASCADE ?
                             cascade_level_range(light, camera) :
                             clipmap_level_range(camera);

  if (levels_ != levels_new) {
    /* Acquire missing tile-maps. */
    int64_t before_range = (levels_.size() > 0) ? (levels_.lod_min - levels_new.lod_min) :
                                                levels_new.size();
    int64_t after_range = (levels_.size() > 0) ? (levels_new.lod_max - levels_.lod_max) : 0;

    Vector<ShadowTileMap *> cached_tilemaps = tilemaps_;
    tilemaps_.clear();
    for (int64_t i = 0; i < before_range; i++) {
      tilemaps_.append(tilemap_pool.acquire());
    }
    /* Keep cached LOD's. */
    tilemaps_.extend(cached_tilemaps);
    for (int64_t i = 0; i < after_range; i++) {
      tilemaps_.append(tilemap_pool.acquire());
    }
    levels_ = levels_new;
  }

  light.tilemap_index = tilemap_pool.tilemaps_data.size();
  light.clip_near = 0x7F7FFFFF;                    /* floatBitsToOrderedInt(FLT_MAX) */
  light.clip_far = int(0xFF7FFFFFu ^ 0x7FFFFFFFu); /* floatBitsToOrderedInt(-FLT_MAX) */

  if (directional_distribution_type_get(camera) == SHADOW_PROJECTION_CASCADE) {
    cascade_tilemaps_distribution(light, camera);
  }
  else {
    clipmap_tilemaps_distribution(light, camera);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow Module
 *
 * \{ */

ShadowModule::ShadowModule(Instance &inst, ShadowSceneData &data) : inst_(inst), data_(data)
{
  for (int i = 0; i < statistics_buf_.size(); i++) {
    UNUSED_VARS(i);
    statistics_buf_.current().clear_to_zero();
    statistics_buf_.swap();
  }
}

void ShadowModule::ensure_caster_atlas()
{
  eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE |
                           GPU_TEXTURE_USAGE_ATOMIC;
  if (use_caster_atlas_) {
    int3 atlas_size = atlas_tx_.size();
    if (atlas_size.x > 0 && atlas_size.y > 0 && atlas_size.z > 0) {
      const bool created = caster_atlas_tx_.ensure_2d_array(ShadowModule::atlas_type,
                                                            atlas_size.xy(),
                                                            atlas_size.z,
                                                            usage);
      if (caster_atlas_tx_.is_valid()) {
        if (created) {
          caster_atlas_tx_.filter_mode(false);
        }
        caster_atlas_allocation_failed_ = false;
        return;
      }
    }

    caster_atlas_tx_.free();
    use_caster_atlas_ = false;
    use_caster_atlas_push_ = 0;
    if (!caster_atlas_allocation_failed_) {
      inst_.info_append_i18n(
          "Warning: Could not allocate Shader Info shadow classification atlas. Self Shadow and "
          "Cast Shadow outputs will stay unclassified.");
    }
    caster_atlas_allocation_failed_ = true;
  }

  caster_atlas_tx_.free();
  const bool created = caster_atlas_dummy_tx_.ensure_2d_array(
      ShadowModule::atlas_type, int2(1), 1, usage);
  if (created) {
    caster_atlas_dummy_tx_.filter_mode(false);
  }
}

void ShadowModule::init()
{
  blender::Scene &scene = *inst_.scene;

  global_lod_bias_ = (1.0f - scene.eevee.shadow_resolution_scale) * SHADOW_TILEMAP_LOD;

  do_full_update_ |= assign_if_different(
      data_.use_debug_cost, bool32_t(inst_.debug_mode == eDebugMode::DEBUG_SHADOW_ATOMIC_COST));

  bool update_lights = false;
  bool enable_shadow = (scene.eevee.flag & SCE_EEVEE_SHADOW_ENABLED) != 0;
  bool use_jitter = enable_shadow && !inst_.is_color_bake &&
                    (inst_.is_image_render ||
                     (!inst_.is_navigating && !inst_.is_transforming && !inst_.is_playback &&
                      (scene.eevee.flag & SCE_EEVEE_SHADOW_JITTERED_VIEWPORT)));
  const bool was_enabled = enabled_;
  const bool was_jittered = data_.use_jitter;
  const bool jitter_was_disabled_by_transform = viewport_jitter_disabled_by_transform_;
  viewport_jitter_disabled_by_transform_ = inst_.is_viewport() && enable_shadow &&
                                           inst_.is_transforming &&
                                           (scene.eevee.flag &
                                            SCE_EEVEE_SHADOW_JITTERED_VIEWPORT);
  update_lights |= assign_if_different(enabled_, enable_shadow);
  update_lights |= assign_if_different(data_.use_jitter, bool32_t(use_jitter));
  viewport_history_invalidated_ =
      inst_.is_viewport() &&
      ((was_enabled != enabled_) ||
       (!was_jittered && data_.use_jitter && jitter_was_disabled_by_transform));
  if (update_lights) {
    /* Force light reset. */
    for (Light &light : inst_.lights.light_map_.values()) {
      light.initialized = false;
    }
  }

  if (enabled_) {
    data_.ray_count = clamp_i(scene.eevee.shadow_ray_count, 1, SHADOW_MAX_RAY);
    data_.step_count = clamp_i(scene.eevee.shadow_step_count, 1, SHADOW_MAX_STEP);
  }
  else {
    data_.ray_count = 1;
    data_.step_count = 1;
  }

  /* Read shadow page resolution from user setting. */
  const int new_lod = log2_ceil_u(scene.eevee.shadow_page_resolution);
  if (shadow_page_lod_ != new_lod) {
    shadow_page_lod_ = new_lod;
    /* Force full rebuild on resolution change. */
    do_full_update_ = true;
    for (Light &light : inst_.lights.light_map_.values()) {
      light.initialized = false;
    }
  }

  const int2 atlas_extent = (1 << shadow_page_lod_) * int2(SHADOW_PAGE_PER_ROW, SHADOW_PAGE_PER_COL);

  eGPUTextureUsage tex_usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  if (ShadowModule::shadow_technique == ShadowTechnique::ATOMIC_RASTER) {
    tex_usage |= GPU_TEXTURE_USAGE_ATOMIC;
  }

  const int previous_pool_size_requested = shadow_pool_size_requested_;
  shadow_pool_size_requested_ = enabled_ ? max_ii(scene.eevee.shadow_pool_size, 16) : 0;
  const bool is_same_pool_request = previous_pool_size_requested == shadow_pool_size_requested_;
  const bool has_limited_pool = enabled_ && atlas_tx_.is_valid() &&
                                (shadow_pool_size_allocated_ < shadow_pool_size_requested_);
  const bool keep_limited_pool = has_limited_pool && is_same_pool_request &&
                                 (shadow_pool_retry_countdown_ != 0);
  if (keep_limited_pool && shadow_pool_retry_countdown_ > 0) {
    shadow_pool_retry_countdown_--;
  }
  if (!enabled_ || !is_same_pool_request || !has_limited_pool) {
    shadow_pool_retry_countdown_ = 0;
  }

  const size_t page_byte_size = square_i(1 << shadow_page_lod_) * sizeof(int);
  const uint64_t requested_pool_byte_size = uint64_t(shadow_pool_size_requested_) * square_i(1024);
  const int requested_page_len = enabled_ ?
                                     max_ii(1,
                                            int(divide_ceil_ul(requested_pool_byte_size,
                                                               page_byte_size))) :
                                     1;
  const uint64_t max_hardware_page_len = uint64_t(max_ii(1, GPU_max_texture_layers())) *
                                         SHADOW_PAGE_PER_LAYER;
  const int max_page_len = int(max_hardware_page_len < uint64_t(SHADOW_MAX_PAGE) ?
                                   max_hardware_page_len :
                                   uint64_t(SHADOW_MAX_PAGE));
  const int clamped_page_len = min_ii(requested_page_len, max_page_len);

  if (!keep_limited_pool) {
    int pool_size_to_try = min_ii(
        shadow_pool_size_requested_,
        int((uint64_t(clamped_page_len) * page_byte_size) / square_i(1024)));
    pool_size_to_try = power_of_2_min_i(max_ii(16, pool_size_to_try));
    pool_size_to_try = min_ii(pool_size_to_try, shadow_pool_size_requested_);
    const int pool_size_retry_ceiling = pool_size_to_try;
    for (; enabled_ && pool_size_to_try >= 16; pool_size_to_try /= 2) {
      const uint64_t pool_byte_size = uint64_t(pool_size_to_try) * square_i(1024);
      const int page_len = min_ii(
          clamped_page_len, int(divide_ceil_ul(pool_byte_size, page_byte_size)));
      const int atlas_layers = divide_ceil_u(page_len, SHADOW_PAGE_PER_LAYER);

      const bool created = atlas_tx_.ensure_2d_array(
          ShadowModule::atlas_type, atlas_extent, atlas_layers, tex_usage);
      if (atlas_tx_.is_valid()) {
        const int allocated_size = int((uint64_t(page_len) * page_byte_size) / square_i(1024));
        if (created || shadow_page_len_ != page_len ||
            shadow_pool_size_allocated_ != allocated_size)
        {
          do_full_update_ = true;
        }
        shadow_page_len_ = page_len;
        shadow_pool_size_allocated_ = allocated_size;
        if (shadow_pool_size_allocated_ >= shadow_pool_size_requested_) {
          shadow_pool_retry_countdown_ = 0;
        }
        else if (shadow_pool_size_allocated_ < pool_size_retry_ceiling) {
          shadow_pool_retry_countdown_ = shadow_pool_retry_cooldown;
        }
        else {
          shadow_pool_retry_countdown_ = -1;
        }
        break;
      }
      shadow_pool_size_allocated_ = 0;
      atlas_tx_.free();
    }
    if (enabled_ && shadow_pool_size_allocated_ == 0) {
      shadow_page_len_ = 0;
      shadow_pool_retry_countdown_ = shadow_pool_retry_cooldown;
    }
  }

  if (!enabled_) {
    if (shadow_page_len_ != 1 || shadow_pool_size_allocated_ != 0) {
      do_full_update_ = true;
    }
    shadow_page_len_ = 1;
    shadow_pool_size_allocated_ = 0;
    shadow_pool_retry_countdown_ = 0;
    if (atlas_tx_.ensure_2d_array(ShadowModule::atlas_type, int2(1), 1)) {
      do_full_update_ = true;
    }
  }

  /* Make allocation safe. Avoids crashes later on. */
  if (!atlas_tx_.is_valid()) {
    atlas_tx_.free();
    do_full_update_ = true;
    shadow_page_len_ = 0;
    shadow_pool_size_allocated_ = 0;
    do_full_update_ |= atlas_tx_.ensure_2d_array(ShadowModule::atlas_type, int2(1), 1);
    inst_.info_append_i18n(
        "Error: Could not allocate shadow atlas. Most likely out of GPU memory.");
  }
  else if (enabled_ && shadow_pool_size_allocated_ != shadow_pool_size_requested_) {
    inst_.info_append_i18n(
        "Warning: Could not allocate shadow pool of {} MB, using {} MB instead.",
        shadow_pool_size_requested_,
        shadow_pool_size_allocated_);
  }

  /* Read end of the swap-chain to avoid stall. */
  if (inst_.is_viewport()) {
    if (inst_.sampling.finished_viewport()) {
      /* Swap enough to read the last one. */
      for (int i = 0; i < statistics_buf_.size(); i++) {
        statistics_buf_.swap();
      }
    }
    else {
      statistics_buf_.swap();
    }
    statistics_buf_.current().read();
    ShadowStatistics stats = statistics_buf_.current();

    if (stats.page_used_count > shadow_page_len_ && enabled_) {
      inst_.info_append_i18n(
          "Error: Shadow buffer full, may result in missing shadows and lower "
          "performance. ({} / {})",
          stats.page_used_count,
          shadow_page_len_);
    }
    if (stats.view_needed_count > SHADOW_VIEW_MAX && enabled_) {
      inst_.info_append_i18n("Error: Too many shadow updates, some shadows might be incorrect.");
    }
  }

  atlas_tx_.filter_mode(false);
  if (caster_atlas_tx_.is_valid()) {
    caster_atlas_tx_.filter_mode(false);
  }
  if (caster_atlas_dummy_tx_.is_valid()) {
    caster_atlas_dummy_tx_.filter_mode(false);
  }
  update_caster_atlas_ref();

  /* Create different viewport to support different update region size. The most fitting viewport
   * is then selected during the tilemap finalize stage in `viewport_select`. */
  for (int i = 0; i < multi_viewports_.size(); i++) {
    /** IMPORTANT: Reflect changes in TBDR tile vertex shader which assumes viewport index 15
     * covers the whole framebuffer. */
    int size_in_tile = min_ii(1 << i, SHADOW_TILEMAP_RES);
    multi_viewports_[i][0] = 0;
    multi_viewports_[i][1] = 0;
    multi_viewports_[i][2] = size_in_tile * (1 << shadow_page_lod_);
    multi_viewports_[i][3] = size_in_tile * (1 << shadow_page_lod_);
  }
}

void ShadowModule::begin_sync()
{
  use_caster_atlas_next_ = false;

  past_casters_updated_.clear();
  curr_casters_updated_.clear();
  curr_casters_.clear();
  jittered_transparent_casters_.clear();
  bake_receivers_.clear();
  update_casters_ = true;

  if (box_batch_ == nullptr) {
    box_batch_ = GPU_batch_unit_cube();
  }

  {
    Manager &manager = *inst_.manager;

    PassMain &pass = tilemap_usage_ps_;
    pass.init();

    if (inst_.is_baking()) {
      SurfelBuf &surfels_buf = inst_.volume_probes.bake.surfels_buf_;
      CaptureInfoBuf &capture_info_buf = inst_.volume_probes.bake.capture_info_buf_;
      float surfel_coverage_area = inst_.volume_probes.bake.surfel_density_;

      /* Directional shadows. */
      float texel_size = ShadowDirectional::tile_size_get(0) / float(SHADOW_PAGE_RES);
      int directional_level = std::max(0, int(std::ceil(log2(surfel_coverage_area / texel_size))));

      PassMain::Sub &sub = pass.sub("Surfels");
      sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_USAGE_SURFELS));
      sub.bind_ssbo("tilemaps_buf", &tilemap_pool.tilemaps_data);
      sub.bind_ssbo("tiles_buf", &tilemap_pool.tiles_data);
      sub.bind_ssbo("surfel_buf", &surfels_buf);
      sub.bind_ssbo("capture_info_buf", &capture_info_buf);
      sub.push_constant("directional_level", directional_level);
      sub.bind_resources(inst_.uniform_data);
      sub.bind_resources(inst_.lights);
      sub.dispatch(&inst_.volume_probes.bake.dispatch_per_surfel_);

      /* Skip opaque and transparent tagging for light baking. */
      return;
    }

    {
      /* Use depth buffer to tag needed shadow pages for opaque geometry. */
      PassMain::Sub &sub = pass.sub("Opaque");
      sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_USAGE_OPAQUE));
      sub.bind_ssbo("tilemaps_buf", &tilemap_pool.tilemaps_data);
      sub.bind_ssbo("tiles_buf", &tilemap_pool.tiles_data);
      sub.bind_texture("depth_tx", &src_depth_tx_);
      sub.push_constant("input_depth_extent", &input_depth_extent_);
      sub.bind_resources(inst_.lights);
      sub.bind_resources(inst_.uniform_data);
      sub.bind_resources(inst_.hiz_buffer.front);
      sub.dispatch(&dispatch_depth_scan_size_);
    }
    {
      /* Use bounding boxes for transparent geometry. */
      PassMain::Sub &sub = pass.sub("Transparent");
      /* WORKAROUND: The DRW_STATE_WRITE_STENCIL is here only to avoid enabling the rasterizer
       * discard inside draw manager. */
      sub.state_set(DRW_STATE_CULL_FRONT | DRW_STATE_WRITE_STENCIL);
      sub.state_stencil(0, 0, 0);
      sub.framebuffer_set(&usage_tag_fb);
      sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_USAGE_TRANSPARENT));
      sub.bind_ssbo("tilemaps_buf", &tilemap_pool.tilemaps_data);
      sub.bind_ssbo("tiles_buf", &tilemap_pool.tiles_data);
      sub.bind_ssbo("bounds_buf", &manager.bounds_buf.current());
      sub.push_constant("fb_resolution", &usage_tag_fb_resolution_);
      sub.push_constant("fb_lod", &usage_tag_fb_lod_);
      sub.bind_resources(inst_.uniform_data);
      sub.bind_resources(inst_.hiz_buffer.front);
      sub.bind_resources(inst_.lights);

      tilemap_usage_transparent_ps_ = &sub;
    }
  }
}

void ShadowModule::sync_object(const ObjectHandle &ob_handle,
                               bool is_alpha_blend,
                               bool has_transparent_shadows,
                               bool has_time_dependent_shadows)
{
  if (is_alpha_blend && !inst_.is_baking()) {
    tilemap_usage_transparent_ps_->draw(box_batch_, ob_handle.res_handle);
  }

  bool is_shadow_caster = !(ob_handle.object->visibility_flag & OB_HIDE_SHADOW);
  if (!is_shadow_caster) {
    return;
  }

  const bool shape_changed = has_time_dependent_shadows && inst_.materials.material_time_changed;

  for (int i : IndexRange(ob_handle.instances_count())) {
    ShadowObject &shadow_ob = objects_.lookup_or_add_default(ObjectKey(ob_handle, i));
    shadow_ob.used = true;
    const bool is_initialized = shadow_ob.resource_handle.is_valid();
    const bool has_jittered_transparency = has_transparent_shadows && data_.use_jitter;
    const bool caster_changed = ob_handle.recalc || !is_initialized || shape_changed;
    ResourceHandle instance_handle = ob_handle.res_handle.sub_handle(i);
    if (is_shadow_caster && (caster_changed || has_jittered_transparency)) {
      viewport_history_invalidated_ |= inst_.is_viewport() && data_.use_jitter && caster_changed;
      if (ob_handle.recalc && is_initialized) {
        past_casters_updated_.append(shadow_ob.resource_handle.raw());
      }

      if (has_jittered_transparency) {
        jittered_transparent_casters_.append(instance_handle.raw());
      }
      else {
        curr_casters_updated_.append(instance_handle.raw());
      }
    }
    shadow_ob.resource_handle = instance_handle;

    if (is_shadow_caster) {
      curr_casters_.append(instance_handle.raw());
    }
  }
}

void ShadowModule::sync_bake_receiver_bounds(const ResourceHandleRange &resource_handle)
{
  if (!inst_.is_color_bake || !resource_handle.is_valid()) {
    return;
  }

  bake_receivers_.append(resource_handle.raw());
}

void ShadowModule::end_sync()
{
  const bool previous_use_caster_atlas = use_caster_atlas_;
  const bool requested_use_caster_atlas = use_caster_atlas_next_ && enabled_ &&
                                          ShadowModule::shadow_technique ==
                                              ShadowTechnique::ATOMIC_RASTER;
  if (!requested_use_caster_atlas) {
    caster_atlas_allocation_failed_ = false;
  }
  use_caster_atlas_ = requested_use_caster_atlas && !caster_atlas_allocation_failed_;
  use_caster_atlas_push_ = int(use_caster_atlas_);
  do_full_update_ |= previous_use_caster_atlas != use_caster_atlas_;
  ensure_caster_atlas();
  update_caster_atlas_ref();
  data_.use_caster_atlas = bool32_t(use_caster_atlas_);

  const DirectionalFocusData old_focus = directional_focus_data_ensure(*this);

  /* Delete unused shadows first to release tile-maps that could be reused for new lights. */
  for (Light &light : inst_.lights.light_map_.values()) {
    /* Do not discard lights in baking mode. See WORKAROUND in `surfels_create`. */
    if ((!light.used || !enabled_) && !inst_.is_baking()) {
      light.shadow_discard_safe(*this);
    }
    else if (light.directional != nullptr) {
      light.directional->release_excess_tilemaps(light, inst_.camera);
    }
    else if (light.punctual != nullptr) {
      light.punctual->release_excess_tilemaps(light);
    }
  }

  directional_focus_update(
      directional_focus_data_ensure(*this), inst_.camera, curr_casters_, *inst_.manager);
  const DirectionalFocusData &new_focus = directional_focus_data_ensure(*this);
  viewport_history_invalidated_ |= inst_.is_viewport() && data_.use_jitter &&
                                   (math::distance_squared(old_focus.position,
                                                           new_focus.position) > 1.0e-10f ||
                                    std::abs(old_focus.distance - new_focus.distance) > 1.0e-5f ||
                                    std::abs(old_focus.blend - new_focus.blend) > 1.0e-5f);

  /* Allocate new tile-maps and fill shadow data of the lights. */
  tilemap_pool.tilemaps_data.clear();
  for (Light &light : inst_.lights.light_map_.values()) {
    if (enabled_ == false) {
      light.tilemap_index = LIGHT_NO_SHADOW;
    }
    else if (light.directional != nullptr) {
      light.directional->end_sync(light, inst_.camera);
    }
    else if (light.punctual != nullptr) {
      light.punctual->end_sync(light);
    }
    else {
      light.tilemap_index = LIGHT_NO_SHADOW;
    }
  }
  tilemap_pool.end_sync(*this);
  inst_.lights.update_shadow_light_costs();

  /* Search for deleted or updated shadow casters */
  auto it_end = objects_.items().end();
  for (auto it = objects_.items().begin(); it != it_end; ++it) {
    ShadowObject &shadow_ob = (*it).value;
    /* Do not discard casters in baking mode. See WORKAROUND in `surfels_create`. */
    if (!shadow_ob.used && !inst_.is_baking()) {
      /* May not be a caster, but it does not matter, be conservative. */
      past_casters_updated_.append(shadow_ob.resource_handle.raw());
      viewport_history_invalidated_ |= inst_.is_viewport() && data_.use_jitter;
      objects_.remove(it);
    }
    else {
      /* Clear for next sync. */
      shadow_ob.used = false;
    }
  }
  past_casters_updated_.push_update();
  curr_casters_updated_.push_update();
  jittered_transparent_casters_.push_update();
  bake_receivers_.push_update();

  curr_casters_.push_update();

  if (do_full_update_) {
    do_full_update_ = false;
    /* Put all pages in the free heap. */
    for (uint i : IndexRange(shadow_page_len_)) {
      uint3 page = {i % SHADOW_PAGE_PER_ROW,
                    (i / SHADOW_PAGE_PER_ROW) % SHADOW_PAGE_PER_COL,
                    i / SHADOW_PAGE_PER_LAYER};
      pages_free_data_[i] = shadow_page_pack(page);
    }
    for (uint i : IndexRange(shadow_page_len_, SHADOW_MAX_PAGE - shadow_page_len_)) {
      pages_free_data_[i] = 0xFFFFFFFFu;
    }
    pages_free_data_.push_update();

    /* Clear tiles to not reference any page. */
    tilemap_pool.tiles_data.clear_to_zero();
    tilemap_pool.tilemaps_clip.clear_to_zero();

    /* Clear cached page buffer. */
    GPU_storagebuf_clear(pages_cached_data_, -1);

    /* Reset info to match new state. */
    pages_infos_data_.page_free_count = shadow_page_len_;
    pages_infos_data_.page_alloc_count = 0;
    pages_infos_data_.page_cached_next = 0u;
    pages_infos_data_.page_cached_start = 0u;
    pages_infos_data_.page_cached_end = 0u;
    pages_infos_data_.push_update();
  }

  {
    Manager &manager = *inst_.manager;

    {
      PassSimple &pass = tilemap_setup_ps_;
      pass.init();

      {
        /* Clear tile-map clip buffer. */
        PassSimple::Sub &sub = pass.sub("ClearClipmap");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_CLIPMAP_CLEAR));
        sub.bind_ssbo("tilemaps_clip_buf", tilemap_pool.tilemaps_clip);
        sub.push_constant("tilemaps_clip_buf_len", int(tilemap_pool.tilemaps_clip.size()));
        sub.dispatch(int3(
            divide_ceil_u(tilemap_pool.tilemaps_clip.size(), SHADOW_CLIPMAP_GROUP_SIZE), 1, 1));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }

      {
        /* Compute near/far clip distances for directional shadows based on casters bounds. */
        PassSimple::Sub &sub = pass.sub("DirectionalBounds");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_BOUNDS));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tilemaps_clip_buf", tilemap_pool.tilemaps_clip);
        sub.bind_ssbo("casters_id_buf", curr_casters_);
        sub.bind_ssbo("bounds_buf", &manager.bounds_buf.current());
        /* Bind again using a writable binding. */
        sub.bind_ssbo("light_buf_write", &inst_.lights.culling_light_buf_);
        sub.push_constant("resource_len", int(curr_casters_.size()));
        sub.bind_resources(inst_.lights);
        sub.dispatch(int3(
            divide_ceil_u(std::max(curr_casters_.size(), int64_t(1)), SHADOW_BOUNDS_GROUP_SIZE),
            1,
            1));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /* Clear usage bits. Tag update from the tile-map for sun shadow clip-maps shifting. */
        PassSimple::Sub &sub = pass.sub("Init");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_INIT));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tilemaps_clip_buf", tilemap_pool.tilemaps_clip);
        sub.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        sub.bind_ssbo("pages_cached_buf", pages_cached_data_);
        sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        /* Free unused tiles from tile-maps not used by any shadow. */
        if (tilemap_pool.tilemaps_unused.size() > 0) {
          sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_unused);
          sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_unused.size()));
        }
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
    }

    {
      PassSimple &pass = tilemap_usage_bake_receiver_ps_;
      pass.init();
      if (bake_receivers_.size() > 0) {
        pass.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_USAGE_BOUNDS));
        pass.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        pass.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        pass.bind_ssbo("bounds_buf", &manager.bounds_buf.current());
        pass.bind_ssbo("resource_ids_buf", bake_receivers_);
        pass.bind_resources(inst_.lights);
        pass.dispatch(int3(bake_receivers_.size(), 1, 1));
        pass.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
    }

    {
      /* Mark for update all shadow pages touching an updated shadow caster. */
      PassSimple &pass = caster_update_ps_;
      pass.init();
      pass.framebuffer_set(&update_tag_fb_);
      pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_CULL_FRONT);
      pass.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_UPDATE));
      pass.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
      pass.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
      pass.push_constant("tilemap_count", int(tilemap_pool.tilemaps_data.size()));
      /* Past caster transforms. */
      if (past_casters_updated_.size() > 0) {
        pass.bind_ssbo("bounds_buf", &manager.bounds_buf.previous());
        pass.bind_ssbo("resource_ids_buf", past_casters_updated_);
        pass.draw(box_batch_, past_casters_updated_.size() * tilemap_pool.tilemaps_data.size());
      }
      /* Current caster transforms. */
      if (curr_casters_updated_.size() > 0) {
        pass.bind_ssbo("bounds_buf", &manager.bounds_buf.current());
        pass.bind_ssbo("resource_ids_buf", curr_casters_updated_);
        pass.draw(box_batch_, curr_casters_updated_.size() * tilemap_pool.tilemaps_data.size());
      }
      pass.barrier(GPU_BARRIER_SHADER_STORAGE);
    }

    {
      /* Mark for update all shadow pages touching a jittered transparency shadow caster. */
      PassSimple &pass = jittered_transparent_caster_update_ps_;
      pass.init();
      if (jittered_transparent_casters_.size() > 0) {
        pass.framebuffer_set(&update_tag_fb_);
        pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_CULL_FRONT);
        pass.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_UPDATE));
        pass.push_constant("tilemap_count", int(tilemap_pool.tilemaps_data.size()));
        pass.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        pass.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        pass.bind_ssbo("bounds_buf", &manager.bounds_buf.current());
        pass.bind_ssbo("resource_ids_buf", jittered_transparent_casters_);
        pass.draw(box_batch_,
                  jittered_transparent_casters_.size() * tilemap_pool.tilemaps_data.size());
        pass.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
    }

    {
      /* Propagate the update tag to the lower LODs. */
      PassSimple &pass = update_propagate_ps_;
      pass.init();
      if (past_casters_updated_.size() > 0 || curr_casters_updated_.size() > 0 ||
          jittered_transparent_casters_.size() > 0)
      {
        pass.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_UPDATE_PROPAGATE));
        pass.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        pass.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        pass.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        pass.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
    }

    /* Non volume usage tagging happens between these two steps.
     * (Setup at begin_sync) */

    if (inst_.volume.needs_shadow_tagging() && !inst_.is_baking()) {
      PassMain::Sub &sub = tilemap_usage_ps_.sub("World Volume");
      sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_TAG_USAGE_VOLUME));
      sub.bind_ssbo("tilemaps_buf", &tilemap_pool.tilemaps_data);
      sub.bind_ssbo("tiles_buf", &tilemap_pool.tiles_data);
      sub.bind_resources(inst_.uniform_data);
      sub.bind_resources(inst_.hiz_buffer.front);
      sub.bind_resources(inst_.sampling);
      sub.bind_resources(inst_.lights);
      sub.bind_resources(inst_.volume.properties);
      sub.bind_resources(inst_.volume.result);
      sub.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
      sub.dispatch(math::divide_ceil(inst_.volume.grid_size(), int3(VOLUME_GROUP_SIZE)));
    }

    {
      PassSimple &pass = tilemap_update_ps_;
      pass.init();
      {
        /* Mark tiles that are redundant in the mipmap chain as unused. */
        PassSimple::Sub &sub = pass.sub("MaskLod");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_PAGE_MASK));
        sub.push_constant("max_view_per_tilemap", &max_view_per_tilemap_);
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /* Free unused pages & Reclaim cached pages. */
        PassSimple::Sub &sub = pass.sub("Free");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_PAGE_FREE));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        sub.bind_ssbo("pages_infos_buf", pages_infos_data_);
        sub.bind_ssbo("pages_free_buf", pages_free_data_);
        sub.bind_ssbo("pages_cached_buf", pages_cached_data_);
        sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        /* Free unused tiles from tile-maps not used by any shadow. */
        if (tilemap_pool.tilemaps_unused.size() > 0) {
          sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_unused);
          sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_unused.size()));
        }
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /* De-fragment the free page heap after cache reuse phase which can leave hole. */
        PassSimple::Sub &sub = pass.sub("Defrag");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_PAGE_DEFRAG));
        sub.push_constant("shadow_page_lod", shadow_page_lod_);
        sub.push_constant("shadow_page_res", 1 << shadow_page_lod_);
        sub.bind_ssbo("pages_infos_buf", pages_infos_data_);
        sub.bind_ssbo("pages_free_buf", pages_free_data_);
        sub.bind_ssbo("pages_cached_buf", pages_cached_data_);
        sub.bind_ssbo("statistics_buf", statistics_buf_.current());
        sub.bind_ssbo("clear_dispatch_buf", clear_dispatch_buf_);
        sub.bind_ssbo("tile_draw_buf", tile_draw_buf_);
        sub.dispatch(int3(1, 1, 1));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /* Assign pages to tiles that have been marked as used but possess no page. */
        PassSimple::Sub &sub = pass.sub("AllocatePages");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_PAGE_ALLOCATE));
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tiles_buf", tilemap_pool.tiles_data);
        sub.bind_ssbo("statistics_buf", statistics_buf_.current());
        sub.bind_ssbo("pages_infos_buf", pages_infos_data_);
        sub.bind_ssbo("pages_free_buf", pages_free_data_);
        sub.bind_ssbo("pages_cached_buf", pages_cached_data_);
        sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /* Convert the unordered tiles into a texture used during shading. Creates views. */
        PassSimple::Sub &sub = pass.sub("Finalize");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_FINALIZE));
        sub.bind_ssbo("tilemaps_buf", &tilemap_pool.tilemaps_data);
        sub.bind_ssbo("tiles_buf", &tilemap_pool.tiles_data);
        sub.bind_ssbo("pages_infos_buf", &pages_infos_data_);
        sub.bind_ssbo("statistics_buf", &statistics_buf_.current());
        sub.bind_ssbo("view_infos_buf", &shadow_multi_view_.matrices_ubo_get());
        sub.bind_ssbo("render_view_buf", &render_view_buf_);
        sub.bind_ssbo("tilemaps_clip_buf", &tilemap_pool.tilemaps_clip);
        sub.bind_image("tilemaps_img", &tilemap_pool.tilemap_tx);
        sub.dispatch(int3(1, 1, tilemap_pool.tilemaps_data.size()));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_UNIFORM | GPU_BARRIER_TEXTURE_FETCH |
                    GPU_BARRIER_SHADER_IMAGE_ACCESS);
      }
      {
        /* Convert the unordered tiles into a texture used during shading. Creates views. */
        PassSimple::Sub &sub = pass.sub("RenderMap");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_RENDERMAP));
        sub.bind_ssbo("statistics_buf", &statistics_buf_.current());
        sub.bind_ssbo("render_view_buf", &render_view_buf_);
        sub.bind_ssbo("tiles_buf", &tilemap_pool.tiles_data);
        sub.bind_ssbo("clear_dispatch_buf", &clear_dispatch_buf_);
        sub.bind_ssbo("tile_draw_buf", &tile_draw_buf_);
        sub.bind_ssbo("dst_coord_buf", &dst_coord_buf_);
        sub.bind_ssbo("src_coord_buf", &src_coord_buf_);
        sub.bind_ssbo("render_map_buf", &render_map_buf_);
        sub.dispatch(int3(1, 1, SHADOW_VIEW_MAX));
        sub.barrier(GPU_BARRIER_SHADER_STORAGE);
      }
      {
        /* Amend tilemap_tx content to support clipmap LODs. */
        PassSimple::Sub &sub = pass.sub("Amend");
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_TILEMAP_AMEND));
        sub.bind_image("tilemaps_img", tilemap_pool.tilemap_tx);
        sub.bind_ssbo("tilemaps_buf", tilemap_pool.tilemaps_data);
        sub.push_constant("tilemaps_buf_len", int(tilemap_pool.tilemaps_data.size()));
        /* Bind again using a writable binding. */
        sub.bind_ssbo("light_buf_write", &inst_.lights.culling_light_buf_);
        sub.bind_resources(inst_.lights);
        sub.dispatch(int3(1, 1, max_ii(inst_.lights.sun_lights_len_, 1)));
        sub.barrier(GPU_BARRIER_TEXTURE_FETCH);
      }

      {
        /** Clear pages that need to be rendered. */
        PassSimple::Sub &sub = pass.sub("RenderClear");
        sub.framebuffer_set(&render_fb_);
        sub.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_ALWAYS);
        sub.shader_set(inst_.shaders.static_shader_get(SHADOW_PAGE_CLEAR));
        sub.push_constant("shadow_page_lod", shadow_page_lod_);
        sub.push_constant("shadow_page_res", 1 << shadow_page_lod_);
        sub.bind_ssbo("pages_infos_buf", pages_infos_data_);
        sub.bind_ssbo("dst_coord_buf", dst_coord_buf_);
        sub.push_constant("use_shadow_caster_atlas", use_caster_atlas_push_);
        sub.bind_image("shadow_atlas_img", atlas_tx_);
        sub.bind_image("shadow_caster_atlas_img", caster_atlas_ref());
        sub.dispatch(clear_dispatch_buf_);
        sub.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
      }
    }
  }

  debug_end_sync();
}

void ShadowModule::debug_end_sync()
{
  debug_draw_ready_ = false;
  debug_draw_mode_ = int(eDebugMode::DEBUG_NONE);

  const bool show_shadow_lod = inst_.shadow_lod_overlay_enabled();
  if (!show_shadow_lod &&
      !ELEM(inst_.debug_mode,
            eDebugMode::DEBUG_SHADOW_TILEMAPS,
            eDebugMode::DEBUG_SHADOW_VALUES,
            eDebugMode::DEBUG_SHADOW_TILE_RANDOM_COLOR,
            eDebugMode::DEBUG_SHADOW_TILEMAP_RANDOM_COLOR,
            eDebugMode::DEBUG_SHADOW_ATOMIC_COST))
  {
    return;
  }

  const eDebugMode debug_mode = show_shadow_lod ? eDebugMode::DEBUG_SHADOW_LOD :
                                                  inst_.debug_mode;

  /* Init but not filled if no active object. */
  debug_draw_ps_.init();

  int tilemap_index = 0;
  bool has_debug_light = false;
  Object *object_active = inst_.draw_ctx->obact;
  const bool active_light_selected = object_active != nullptr && object_active->type == OB_LAMP;
  ObjectKey selected_light_key;
  std::string selected_light_name;
  bool active_light_has_valid_tilemap = false;
  if (show_shadow_lod && active_light_selected) {
    ObjectKey object_key(ObjectRef(DEG_get_original(object_active)));
    if (inst_.lights.light_map_.contains(object_key)) {
      const Light &light = inst_.lights.light_map_.lookup(object_key);
      if (light.tilemap_index != LIGHT_NO_SHADOW && light.tilemap_index >= 0 &&
          light.tilemap_index < SHADOW_MAX_TILEMAP &&
          light.tilemap_index <= light.tilemap_max_get())
      {
        active_light_has_valid_tilemap = true;
        debug_light_key_ = object_key;
        has_debug_light_key_ = true;
        debug_light_name_ = object_active->id.name + 2;
      }
    }
  }

  if (show_shadow_lod && has_debug_light_key_ &&
      (!active_light_selected || active_light_has_valid_tilemap) &&
      inst_.lights.light_map_.contains(debug_light_key_))
  {
    const Light &light = inst_.lights.light_map_.lookup(debug_light_key_);
    if (light.tilemap_index != LIGHT_NO_SHADOW && light.tilemap_index >= 0 &&
        light.tilemap_index < SHADOW_MAX_TILEMAP && light.tilemap_index <= light.tilemap_max_get())
    {
      selected_light_key = debug_light_key_;
      selected_light_name = debug_light_name_;
      tilemap_index = light.tilemap_index;
      has_debug_light = true;
    }
  }
  else if (!show_shadow_lod && debug_mode != eDebugMode::DEBUG_SHADOW_ATOMIC_COST &&
           object_active != nullptr)
  {
    ObjectKey object_key(ObjectRef(DEG_get_original(object_active)));
    if (inst_.lights.light_map_.contains(object_key)) {
      const Light &light = inst_.lights.light_map_.lookup(object_key);
      if (light.tilemap_index != LIGHT_NO_SHADOW && light.tilemap_index >= 0 &&
          light.tilemap_index < SHADOW_MAX_TILEMAP &&
          light.tilemap_index <= light.tilemap_max_get())
      {
        selected_light_key = object_key;
        selected_light_name = object_active->id.name + 2;
        tilemap_index = light.tilemap_index;
        has_debug_light = true;
      }
    }
  }

  if (show_shadow_lod && has_debug_light_key_ && !has_debug_light) {
    has_debug_light_key_ = false;
  }

  if (!has_debug_light && debug_mode != eDebugMode::DEBUG_SHADOW_ATOMIC_COST) {
    if (show_shadow_lod) {
      if (object_active == nullptr) {
        inst_.info_append("Shadow LOD: Select a shadow-casting light to inspect.");
      }
      else if (object_active->type != OB_LAMP) {
        inst_.info_append("Shadow LOD: Select a shadow-casting light to inspect.");
      }
      else if (!(id_cast<::blender::Light *>(object_active->data)->mode & LA_SHADOW)) {
        inst_.info_append(
            "Shadow LOD: Active light \"{}\" has shadows disabled.", object_active->id.name + 2);
      }
      else if (!enabled_) {
        inst_.info_append("Shadow LOD: EEVEE shadows are disabled.");
      }
      else {
        inst_.info_append("Shadow LOD: Active light \"{}\" has no valid shadow tilemap.",
                          object_active->id.name + 2);
      }
    }
    return;
  }

  DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | inst_.film.depth.test_state |
                   DRW_STATE_BLEND_CUSTOM;

  debug_draw_ps_.state_set(state);
  debug_draw_ps_.shader_set(inst_.shaders.static_shader_get(SHADOW_DEBUG));
  debug_draw_ps_.push_constant("debug_mode", int(debug_mode));
  debug_draw_ps_.push_constant("debug_tilemap_index", tilemap_index);
  debug_draw_ps_.push_constant("shadow_lod_opacity", inst_.shadow_lod_overlay_opacity());
  debug_draw_ps_.bind_ssbo("tilemaps_buf", &tilemap_pool.tilemaps_data);
  debug_draw_ps_.bind_ssbo("tiles_buf", &tilemap_pool.tiles_data);
  debug_draw_ps_.bind_resources(inst_.uniform_data);
  debug_draw_ps_.bind_resources(inst_.hiz_buffer.front);
  debug_draw_ps_.bind_resources(inst_.lights);
  debug_draw_ps_.bind_resources(inst_.shadows);
  debug_draw_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  debug_draw_mode_ = int(debug_mode);
  debug_draw_ready_ = true;

  if (show_shadow_lod) {
    const Light &light = inst_.lights.light_map_.lookup(selected_light_key);
    const bool is_sun = ELEM(light.type, LIGHT_SUN, LIGHT_SUN_ORTHO);
    inst_.info_append("Shadow LOD: {} ({}){}\n"
                      " - L row: {}\n"
                      " - D row: N (near) to F (far) depth\n"
                      " - Legend: Effective LOD and equivalent virtual resolution\n"
                      " - Magenta in L row: Invalid shadow tile",
                      selected_light_name,
                      debug_light_type_name_get(light.type),
                      object_active != nullptr && object_active->type != OB_LAMP ?
                          " (pinned while editing another object)" :
                          "",
                      is_sun ?
                          "Sun level; each +1 covers about 2x wider and is coarser" :
                          "Effective LOD 0 (fine) to 7 (coarse)");
  }
}

float ShadowModule::screen_pixel_radius(const float4x4 &wininv,
                                        bool is_perspective,
                                        const int2 &extent)
{
  float min_dim = float(min_ii(extent.x, extent.y));
  float3 p0 = float3(-1.0f, -1.0f, 0.0f);
  float3 p1 = float3(float2(min_dim / extent) * 2.0f - 1.0f, 0.0f);
  p0 = math::project_point(wininv, p0);
  p1 = math::project_point(wininv, p1);
  /* Compute radius at unit plane from the camera. This is NOT the perspective division. */
  if (is_perspective) {
    p0 = p0 / p0.z;
    p1 = p1 / p1.z;
  }
  return math::distance(p0, p1) / min_dim;
}

bool ShadowModule::shadow_update_finished(int loop_count)
{
  if (loop_count >= (SHADOW_MAX_TILEMAP * SHADOW_TILEMAP_LOD) / SHADOW_VIEW_MAX) {
    /* We have reach the maximum theoretical number of updates.
     * This can indicate a problem in the statistic buffer read-back or update tagging. */
    inst_.info_append_i18n("Error: Reached max shadow updates.");
    return true;
  }

  if (!inst_.is_image_render && !inst_.is_light_bake && !inst_.is_color_bake) {
    /* For viewport, only run the shadow update once per redraw.
     * This avoids the stall from the read-back and freezes from long shadow update. */
    return true;
  }

  int max_updated_view_count = tilemap_pool.tilemaps_data.size() * SHADOW_TILEMAP_LOD;
  if (max_updated_view_count <= SHADOW_VIEW_MAX) {
    /* There is enough shadow views to cover all tile-map updates.
     * No read-back needed as it is guaranteed that all of them will be updated. */
    return true;
  }

  /* Read back and check if there is still tile-map to update. */
  statistics_buf_.current().async_flush_to_host();
  statistics_buf_.current().read();
  ShadowStatistics stats = statistics_buf_.current();

  if (stats.page_used_count > shadow_page_len_) {
    inst_.info_append_i18n(
        "Error: Shadow buffer full, may result in missing shadows and lower "
        "performance. ({} / {})",
        stats.page_used_count,
        shadow_page_len_);
  }

  /* Rendering is finished if we rendered all the remaining pages. */
  return stats.view_needed_count <= SHADOW_VIEW_MAX;
}

int ShadowModule::max_view_per_tilemap()
{
  if (inst_.is_image_render || inst_.is_color_bake) {
    /* No need to limit updates per lights as we ensure all lights levels will be rendered.
     * is_image_render. */
    return SHADOW_TILEMAP_LOD;
  }
  /* For now very simple heuristic. Can be improved later by taking into consideration how many
   * tile-maps are updating, but we cannot know the ones updated by casters. */
  int potential_view_count = 0;
  for (auto i : IndexRange(tilemap_pool.tilemaps_data.size())) {
    if (tilemap_pool.tilemaps_data[i].projection_type == SHADOW_PROJECTION_CUBEFACE) {
      potential_view_count += SHADOW_TILEMAP_LOD;
    }
    else {
      potential_view_count += 1;
    }
  }
  int max_view_count = divide_ceil_u(SHADOW_VIEW_MAX, math::max(potential_view_count, 1));
  /* For viewport interactivity, have a hard maximum. This allows smoother experience. */
  if (inst_.is_transforming || inst_.is_navigating) {
    max_view_count = math::min(2, max_view_count);
  }
  /* For animation playback, we always want the maximum performance. */
  if (inst_.is_playback) {
    max_view_count = math::min(1, max_view_count);
  }

  return max_view_count;
}

void ShadowModule::ShadowView::compute_visibility(ObjectBoundsBuf &bounds,
                                                  ObjectInfosBuf &infos,
                                                  uint resource_len,
                                                  bool /*debug_freeze*/)
{
  GPU_debug_group_begin("View.compute_visibility");

  uint word_per_draw = this->visibility_word_per_draw();
  /* Switch between tightly packed and set of whole word per instance. */
  uint words_len = (view_len_ == 1) ? divide_ceil_u(resource_len, 32) :
                                      resource_len * word_per_draw;
  words_len = ceil_to_multiple_u(max_ii(1, words_len), 4);
  /* TODO(fclem): Resize to nearest pow2 to reduce fragmentation. */
  visibility_buf_.resize(words_len);

  const uint32_t data = 0xFFFFFFFFu;
  GPU_storagebuf_clear(visibility_buf_, data);

  if (do_visibility_) {
    gpu::Shader *shader = inst_.shaders.static_shader_get(SHADOW_VIEW_VISIBILITY);
    GPU_shader_bind(shader);
    GPU_shader_uniform_1i(shader, "resource_len", resource_len);
    GPU_storagebuf_bind(bounds, GPU_shader_get_ssbo_binding(shader, "bounds_buf"));
    GPU_storagebuf_bind(visibility_buf_, GPU_shader_get_ssbo_binding(shader, "visibility_buf"));
    GPU_storagebuf_bind(render_view_buf_, GPU_shader_get_ssbo_binding(shader, "render_view_buf"));
    GPU_storagebuf_bind(infos, DRW_OBJ_INFOS_SLOT);
    GPU_uniformbuf_bind(data_, DRW_VIEW_UBO_SLOT);
    GPU_uniformbuf_bind(culling_, DRW_VIEW_CULLING_UBO_SLOT);
    GPU_compute_dispatch(shader, divide_ceil_u(resource_len, DRW_VISIBILITY_GROUP_SIZE), 1, 1);
    GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  }

  GPU_debug_group_end();
}

void ShadowModule::set_view(View &view, int2 extent, const TelemetryShadowContext context)
{
  active_shadow_context_ = context;
  data_.film_pixel_radius = screen_pixel_radius(view.wininv(), view.is_persp(), extent);
}

void ShadowModule::render(View &view, int2 extent)
{
  if (enabled_ == false) {
    /* All lights have been tagged to have no shadow. */
    return;
  }

  const bool record_shadow_context = inst_.telemetry.enabled() && inst_.telemetry.frame_active();
  const double context_start_time = record_shadow_context ? BLI_time_now_seconds() : 0.0;

  input_depth_extent_ = extent;

  gpu::FrameBuffer *prev_fb = GPU_framebuffer_active_get();

  dispatch_depth_scan_size_ = int3(math::divide_ceil(extent, int2(SHADOW_DEPTH_SCAN_GROUP_SIZE)),
                                   1);
  max_view_per_tilemap_ = max_view_per_tilemap();

  usage_tag_fb_resolution_ = math::divide_ceil(extent, int2(std::exp2(usage_tag_fb_lod_)));
  usage_tag_fb.ensure(usage_tag_fb_resolution_);

  eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_MEMORYLESS;
  int2 fb_size = int2(SHADOW_TILEMAP_RES * (1 << shadow_page_lod_));
  int fb_layers = SHADOW_VIEW_MAX;

  if (shadow_technique == ShadowTechnique::ATOMIC_RASTER) {
    /* Create attachment-less framebuffer. */
    shadow_depth_fb_tx_.free();
    shadow_depth_accum_tx_.free();
    render_fb_.ensure(fb_size);
  }
  else if (shadow_technique == ShadowTechnique::TILE_COPY) {
    /* Create memoryless depth attachment for on-tile surface depth accumulation. */
    shadow_depth_fb_tx_.ensure_2d_array(
        gpu::TextureFormat::SFLOAT_32_DEPTH, fb_size, fb_layers, usage);
    shadow_depth_accum_tx_.ensure_2d_array(
        gpu::TextureFormat::SFLOAT_32, fb_size, fb_layers, usage);
    render_fb_.ensure(GPU_ATTACHMENT_TEXTURE(shadow_depth_fb_tx_),
                      GPU_ATTACHMENT_TEXTURE(shadow_depth_accum_tx_));
  }
  else {
    BLI_assert_unreachable();
  }

  update_tag_fb_.ensure(int2(SHADOW_TILEMAP_RES));

  inst_.hiz_buffer.update();

  int loop_count = 0;
  do {
    GPU_debug_group_begin("Shadow");
    {
      {
        ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                               TelemetryStageId::ShadowTilemapSetup);
        GPU_uniformbuf_clear_to_zero(shadow_multi_view_.matrices_ubo_get());
        inst_.manager->submit(tilemap_setup_ps_, view);
      }
      if (loop_count == 0) {
        if (assign_if_different(update_casters_, false)) {
          /* Run caster update only once. */
          /* TODO(fclem): There is an optimization opportunity here where we can
           * test casters only against the static tile-maps instead of all of them. */
          ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                                 TelemetryStageId::ShadowCasterUpdate);
          inst_.manager->submit(caster_update_ps_, view);
        }
        {
          ScopedTelemetrySample telemetry_sample(
              inst_.telemetry, TelemetryStageId::ShadowTransparentCasterUpdate);
          inst_.manager->submit(jittered_transparent_caster_update_ps_, view);
          inst_.manager->submit(update_propagate_ps_, view);
        }
      }
      {
        ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                               TelemetryStageId::ShadowUsageMarking);
        if (inst_.is_color_bake) {
          inst_.manager->submit(tilemap_usage_bake_receiver_ps_, view);
        }
        inst_.manager->submit(tilemap_usage_ps_, view);
      }

      {
        ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                               TelemetryStageId::ShadowTilemapUpdate);
        inst_.manager->submit(tilemap_update_ps_, view);

        shadow_multi_view_.compute_procedural_bounds();

        statistics_buf_.current().async_flush_to_host();
      }

      /* Isolate shadow update into its own command buffer on the heavy Metal tile-copy path. */
      bool use_flush = (shadow_technique == ShadowTechnique::TILE_COPY) &&
                       (GPU_backend_get_type() == GPU_BACKEND_METAL);
      /* Flush every loop as these passes are very heavy. */
      use_flush |= loop_count != 0;

      {
        ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::ShadowSurface);
        if (use_flush) {
          GPU_flush();
        }

        /* TODO(fclem): Move all of this to the draw::PassMain. */
        if (shadow_depth_fb_tx_.is_valid() && shadow_depth_accum_tx_.is_valid()) {
          GPU_framebuffer_bind_ex(
              render_fb_,
              {
                  /* Depth is cleared to 0 for TBDR optimization. */
                  {GPU_LOADACTION_CLEAR, GPU_STOREACTION_DONT_CARE, {0.0f, 0.0f, 0.0f, 0.0f}},
                  {GPU_LOADACTION_CLEAR,
                   GPU_STOREACTION_DONT_CARE,
                   {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX}},
              });
        }
        else if (shadow_depth_fb_tx_.is_valid()) {
          GPU_framebuffer_bind_ex(render_fb_,
                                  {
                                      {GPU_LOADACTION_CLEAR,
                                       GPU_STOREACTION_DONT_CARE,
                                       {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX}},
                                  });
        }
        else {
          GPU_framebuffer_bind(render_fb_);
        }

        GPU_framebuffer_multi_viewports_set(render_fb_,
                                            reinterpret_cast<int (*)[4]>(multi_viewports_.data()));

        inst_.pipelines.shadow.render(shadow_multi_view_);

        if (use_flush) {
          GPU_flush();
        }

        GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
      }
    }
    GPU_debug_group_end();

    loop_count++;

    {
      ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::ShadowUpdateFinish);
      if (shadow_update_finished(loop_count)) {
        break;
      }
    }
  } while (true);

  if (record_shadow_context) {
    inst_.telemetry.shadow_context_add(
        active_shadow_context_, BLI_time_now_seconds() - context_start_time, loop_count);
  }

  if (prev_fb) {
    GPU_framebuffer_bind(prev_fb);
  }
}

void ShadowModule::debug_draw(View &view, gpu::FrameBuffer *view_fb)
{
  if (!debug_draw_ready_) {
    return;
  }

  switch (eDebugMode(debug_draw_mode_)) {
    case DEBUG_SHADOW_TILEMAPS:
      inst_.info_append(
          "Debug Mode: Shadow Tilemap (active light)\n"
          " - Green: Used\n"
          " - Yellow: Used & Updated\n"
          " - Purple: Cached\n");
      break;
    case DEBUG_SHADOW_VALUES:
      inst_.info_append("Debug Mode: Shadow Values (active light)");
      break;
    case DEBUG_SHADOW_TILE_RANDOM_COLOR:
      inst_.info_append("Debug Mode: Shadow Tile Random Color (active light)");
      break;
    case DEBUG_SHADOW_TILEMAP_RANDOM_COLOR:
      inst_.info_append("Debug Mode: Shadow Tilemap Random Color (active light)");
      break;
    case DEBUG_SHADOW_ATOMIC_COST:
      inst_.info_append(
          "Debug Mode: Shadow Atomic Cost\n"
          " - Blue: Low\n"
          " - Red: Medium\n"
          " - White: High");
      break;
    case DEBUG_SHADOW_LOD:
      break;
    default:
      return;
  }

  inst_.hiz_buffer.update();

  GPU_framebuffer_bind(view_fb);
  inst_.manager->submit(debug_draw_ps_, view);
}

/** \} */

}  // namespace blender::eevee
