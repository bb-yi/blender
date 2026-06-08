/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Virtual shadow-mapping: Conservative bounds usage tagging for UV-space color bake.
 *
 * Color bake does not render a normal camera depth prepass. Instead, project the bake receiver
 * bounds into every shadow tilemap and mark the covered tiles as used. Shadow caster passes still
 * own dirty-page update tagging.
 */

#pragma once

#include "draw_aabb_lib.glsl"
#include "draw_intersect_lib.glsl"
#include "eevee_shadow_shared.hh"
#include "eevee_shadow_tilemap_lib.bsl.hh"

namespace eevee::shadow {

struct TagUsageBounds {
  [[storage(0, read)]] const ShadowTileMapData (&tilemaps_buf)[];
  [[storage(1, read_write)]] uint (&tiles_buf)[];
  [[storage(5, read)]] const ObjectBounds (&bounds_buf)[];
  [[storage(6, read)]] const uint (&resource_ids_buf)[];
};

float3 tag_usage_bounds_safe_project(float4x4 winmat, float4x4 viewmat, int &clipped, float3 v)
{
  float4 tmp = winmat * (viewmat * float4(v, 1.0f));
  clipped += int(tmp.w < 0.0f);
  return tmp.xyz / tmp.w;
}

[[compute, local_size(1, 1, 1)]]
void tag_usage_bounds_comp([[resource_table]] TagUsageBounds &srt,
                           [[global_invocation_id]] const uint3 global_id)
{
  ShadowTileMapData tilemap = srt.tilemaps_buf[global_id.z];

  IsectPyramid frustum;
  if (tilemap.projection_type == SHADOW_PROJECTION_CUBEFACE) {
    Pyramid pyramid = shadow_tilemap_cubeface_bounds(tilemap, int2(0), int2(SHADOW_TILEMAP_RES));
    frustum = isect_pyramid_setup(pyramid);
  }

  uint resource_id = srt.resource_ids_buf[global_id.x] & 0x7FFFFFFFu;
  ObjectBounds bounds = srt.bounds_buf[resource_id];
  if (!drw_bounds_are_valid(bounds)) {
    return;
  }

  IsectBox box = isect_box_setup(bounds.bounding_corners[0].xyz,
                                 bounds.bounding_corners[1].xyz,
                                 bounds.bounding_corners[2].xyz,
                                 bounds.bounding_corners[3].xyz);

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
        atomicOr(srt.tiles_buf[tile_index], uint(SHADOW_IS_USED));
      }
    }
  }
}

PipelineCompute tag_usage_bounds(tag_usage_bounds_comp);

}  // namespace eevee::shadow
