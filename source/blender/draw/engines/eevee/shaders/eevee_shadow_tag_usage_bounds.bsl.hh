/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Virtual shadow-mapping: Conservative bounds usage tagging for UV-space color bake.
 *
 * Color bake does not render a normal camera depth prepass. Instead, conservatively tag the
 * shadow pages that may be sampled by the receiver bounds. Directional lights use the same 5.2
 * LightData clipmap/cascade coordinate helpers as shadow evaluation so receiver usage and lookup
 * stay in sync. Local lights keep the tilemap projection fallback.
 */

#pragma once

#include "draw_aabb_lib.glsl"
#include "draw_intersect_lib.glsl"
#include "eevee_light_iter.bsl.hh"
#include "eevee_light_lib.bsl.hh"
#include "eevee_shadow_page_ops.bsl.hh"
#include "eevee_shadow_shared.hh"
#include "eevee_shadow_tilemap_lib.bsl.hh"

namespace eevee::shadow {

struct TagUsageBounds {
  [[resource_table]] srt_t<LightRenderData> light_data;
  [[resource_table]] srt_t<TileMaps> tilemaps;
  [[resource_table]] srt_t<Tiles> tiles;
  [[storage(5, read)]] const ObjectBounds (&bounds_buf)[];
  [[storage(6, read)]] const uint (&resource_ids_buf)[];
};

float3 tag_usage_bounds_safe_project(float4x4 winmat, float4x4 viewmat, int &clipped, float3 v)
{
  float4 tmp = winmat * (viewmat * float4(v, 1.0f));
  clipped += int(tmp.w < 0.0f);
  return tmp.xyz / tmp.w;
}

void tag_usage_bounds_tile([[resource_table]] TagUsageBounds &srt,
                           LightData light,
                           uint2 tile_co,
                           int lod,
                           int tilemap_index)
{
  if (tilemap_index > light.tilemap_max_get()) {
    return;
  }

  tile_co >>= uint(lod);
  [[resource_table]] TileMaps &maps = srt.tilemaps;
  [[resource_table]] Tiles &tiles_ref = srt.tiles;
  ShadowTileMapData tilemap = maps.tilemaps_buf[tilemap_index];
  int tile_index = shadow_tile_offset(tile_co, tilemap.tiles_index, lod);
  atomicOr(tiles_ref.tiles_buf[tile_index], uint(SHADOW_IS_USED));
}

void tag_usage_bounds_directional([[resource_table]] TagUsageBounds &srt,
                                  uint l_idx,
                                  float3 receiver_center,
                                  float receiver_radius)
{
  [[resource_table]] LightRenderData &lrd = srt.light_data;
  LightData light = lrd.light_buf[l_idx];

  if (light.tilemap_index == LIGHT_NO_SHADOW) {
    return;
  }

  float3 lP = light_world_to_local_direction(light, receiver_center);
  LightSunData sun = light.sun();
  int min_level = sun.clipmap_lod_max;
  int max_level = sun.clipmap_lod_min;

  for (int z = -1; z <= 1; z += 2) {
    for (int y = -1; y <= 1; y += 2) {
      for (int x = -1; x <= 1; x += 2) {
        float3 corner_lP = lP + float3(x, y, z) * receiver_radius;
        int level = shadow_directional_level(light, corner_lP - light.position());
        min_level = min(min_level, level);
        max_level = max(max_level, level);
      }
    }
  }

  min_level = clamp(min_level, sun.clipmap_lod_min, sun.clipmap_lod_max);
  max_level = clamp(max_level, sun.clipmap_lod_min, sun.clipmap_lod_max);

  for (int level = min_level; level <= max_level; level++) {
    ShadowCoordinates coord_min = shadow_directional_coordinates_at_level(
        light, lP - float3(receiver_radius, receiver_radius, 0.0f), level);
    ShadowCoordinates coord_max = shadow_directional_coordinates_at_level(
        light, lP + float3(receiver_radius, receiver_radius, 0.0f), level);
    uint2 tile_min = min(coord_min.tilemap_tile, coord_max.tilemap_tile);
    uint2 tile_max = max(coord_min.tilemap_tile, coord_max.tilemap_tile);
    tile_min = clamp(tile_min, uint2(0), uint2(SHADOW_TILEMAP_RES - 1));
    tile_max = clamp(tile_max, uint2(0), uint2(SHADOW_TILEMAP_RES - 1));
    int tilemap_index = light.tilemap_index + level - sun.clipmap_lod_min;

    for (uint y = tile_min.y; y <= tile_max.y; y++) {
      for (uint x = tile_min.x; x <= tile_max.x; x++) {
        tag_usage_bounds_tile(srt, light, uint2(x, y), 0, tilemap_index);
      }
    }
  }
}

void tag_usage_bounds_tilemap([[resource_table]] TagUsageBounds &srt,
                              IsectBox box,
                              uint tilemap_index)
{
  [[resource_table]] TileMaps &maps = srt.tilemaps;
  [[resource_table]] Tiles &tiles_ref = srt.tiles;
  ShadowTileMapData tilemap = maps.tilemaps_buf[tilemap_index];

  IsectPyramid frustum;
  if (tilemap.projection_type == SHADOW_PROJECTION_CUBEFACE) {
    Pyramid pyramid = shadow_tilemap_cubeface_bounds(tilemap, int2(0), int2(SHADOW_TILEMAP_RES));
    frustum = isect_pyramid_setup(pyramid);
  }

  int clipped = 0;
  AABB aabb_ndc = aabb_init_min_max();
  for (int v = 0; v < 8; v++) {
    aabb_merge(aabb_ndc,
               tag_usage_bounds_safe_project(tilemap.winmat, tilemap.viewmat, clipped, box.corners[v]));
  }

  if (tilemap.projection_type == SHADOW_PROJECTION_CUBEFACE) {
    if (clipped == 8) {
      return;
    }
    if (clipped > 0) {
      if (intersect(frustum, box)) {
        aabb_ndc.max = float3(1.0f);
        aabb_ndc.min = float3(-1.0f);
      }
      else {
        return;
      }
    }
  }

  AABB aabb_tag;
  AABB aabb_map = shape_aabb(float3(-0.99999f), float3(0.99999f));
  if (tilemap.projection_type != SHADOW_PROJECTION_CUBEFACE) {
    aabb_map.min.z = -FLT_MAX;
    aabb_map.max.z = FLT_MAX;
  }

  if (!aabb_clip(aabb_map, aabb_ndc, aabb_tag)) {
    return;
  }

  constexpr float tilemap_half_res = float(SHADOW_TILEMAP_RES / 2);
  int2 box_min = int2(aabb_tag.min.xy * tilemap_half_res + tilemap_half_res);
  int2 box_max = int2(aabb_tag.max.xy * tilemap_half_res + tilemap_half_res);

  for (int lod = 0; lod <= SHADOW_TILEMAP_LOD; lod++, box_min >>= 1, box_max >>= 1) {
    for (int y = box_min.y; y <= box_max.y; y++) {
      for (int x = box_min.x; x <= box_max.x; x++) {
        int tile_index = shadow_tile_offset(uint2(uint(x), uint(y)), tilemap.tiles_index, lod);
        atomicOr(tiles_ref.tiles_buf[tile_index], uint(SHADOW_IS_USED));
      }
    }
  }
}

struct TagUsageBoundsCtx {
  IsectBox box;
  float3 receiver_center;
  float receiver_radius;

  void eval_directional([[resource_table]] TagUsageBounds &srt, uint l_idx, LightData light)
  {
    tag_usage_bounds_directional(srt, l_idx, receiver_center, receiver_radius);
  }

  void eval_local([[resource_table]] TagUsageBounds &srt, uint /*l_idx*/, LightData light)
  {
    if (light.tilemap_index == LIGHT_NO_SHADOW) {
      return;
    }
    for (int i = light.tilemap_index; i <= light.tilemap_max_get(); i++) {
      tag_usage_bounds_tilemap(srt, box, uint(i));
    }
  }
};

}  // namespace eevee::shadow

template void eevee::light::foreach<eevee::shadow::TagUsageBoundsCtx,
                                    eevee::shadow::TagUsageBounds>(
    const eevee::LightRenderData &,
    eevee::shadow::TagUsageBoundsCtx &,
    eevee::shadow::TagUsageBounds &);

namespace eevee::shadow {

[[compute, local_size(1, 1, 1)]]
void tag_usage_bounds_comp([[resource_table]] TagUsageBounds &srt,
                           [[global_invocation_id]] const uint3 global_id)
{
  uint resource_id = srt.resource_ids_buf[global_id.x] & 0x7FFFFFFFu;
  ObjectBounds bounds = srt.bounds_buf[resource_id];
  if (!drw_bounds_are_valid(bounds)) {
    return;
  }

  IsectBox box = isect_box_setup(bounds.bounding_corners[0].xyz,
                                 bounds.bounding_corners[1].xyz,
                                 bounds.bounding_corners[2].xyz,
                                 bounds.bounding_corners[3].xyz);

  TagUsageBoundsCtx ctx;
  ctx.box = box;
  ctx.receiver_center = bounds.bounding_sphere.xyz;
  ctx.receiver_radius = bounds.bounding_sphere.w;

  [[resource_table]] LightRenderData &lrd = srt.light_data;
  light::foreach(lrd, ctx, srt);
}

PipelineCompute tag_usage_bounds(tag_usage_bounds_comp);

}  // namespace eevee::shadow
