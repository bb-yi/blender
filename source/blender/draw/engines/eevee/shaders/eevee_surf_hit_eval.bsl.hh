/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_nodetree_frag_lib.bsl.hh"
#include "eevee_raytrace_shared.hh"

void hit_eval_material(KernelGlobals &kg, ShadingData &sd, int2 texel, ThicknessMode mode);
void hit_eval_shadow_tint(KernelGlobals &kg, ShadingData &sd, uint hit_index, float2 footprint);

namespace eevee {

struct HitEvalInterp {
  [[flat]] uint packed_texel;
  [[flat]] uint resource_id_raw;
  [[flat]] int front_facing;
  [[flat]] float3 view_origin;
};

struct HitEvalResources {
  [[legacy_info]] ShaderCreateInfo eevee_surf_hit_eval;
};

struct HitEvalVertexResources {
  [[storage(12, read)]] const HardwareHitEvalRecord (&hit_eval_list_buf)[];
  [[sampler(4)]] sampler2D ray_data_tx;
};

[[vertex]] void geom_hit_mesh([[resource_table]] const HitEvalVertexResources &srt,
                              [[vertex_id]] const int vertex_id,
                              [[out]] HitEvalInterp &hit_interp,
                              [[position]] float4 &out_position)
{
  const HardwareHitEvalRecord record = srt.hit_eval_list_buf[uint(vertex_id) / 3u];
  const bool front_facing = (record.flags & HIT_EVAL_FLAG_FRONT_FACING) != 0u;
  uint corner = uint(vertex_id) % 3u;
  if (!front_facing && corner != 0u) {
    corner = 3u - corner;
  }
  hit_interp.packed_texel = record.packed_texel;
  hit_interp.resource_id_raw = record.resource_id_raw;
  hit_interp.front_facing = front_facing ? 1 : 0;
  hit_interp.view_origin = record.view_origin;

  const float2 texel = float2(unpackUvec2x16(record.packed_texel));
  const float2 extent = float2(textureSize(srt.ray_data_tx, 0));
  const float2 corner_offset = float2(corner == 1u ? 2.0f : 0.0f, corner == 2u ? 2.0f : 0.0f);
  out_position = float4((texel + corner_offset) * (2.0f / extent) - 1.0f, 0.0f, 1.0f);
}

[[fragment]] void surf_hit_eval([[resource_table]] KernelGlobals &kg,
                                [[resource_table]] const HitEvalResources & /*srt*/,
                                [[in]] const HitEvalInterp &hit_interp,
                                [[frag_coord]] const float4 frag_co)
{
  ShadingData sd = {};
  sd.frag_co = frag_co;
  sd.resource_id_raw = hit_interp.resource_id_raw;
  sd.front_facing = hit_interp.front_facing != 0;
  sd.hit_view_origin = hit_interp.view_origin;
  hit_eval_material(kg,
                    sd,
                    int2(unpackUvec2x16(hit_interp.packed_texel)),
                    ThicknessMode(kg.nt.node_tree.thickness_mode));
}

struct ShadowTintInterp {
  [[flat]] uint hit_index;
  [[smooth]] float2 footprint;
};

struct ShadowTintVertexResources {
  [[storage(12, read)]] const uint (&shadow_tint_order)[];
  [[push_constant]] int2 shadow_tint_grid;
  [[push_constant]] bool shadow_tint_direct_order;
};

[[vertex]] void geom_shadow_tint([[resource_table]] const ShadowTintVertexResources &srt,
                                 [[vertex_id]] const int vertex_id,
                                 [[out]] ShadowTintInterp &interp,
                                 [[position]] float4 &out_position)
{
  uint index = uint(vertex_id) / 3u;
  uint corner = uint(vertex_id) % 3u;
  interp.hit_index = srt.shadow_tint_direct_order ? index : srt.shadow_tint_order[index];
  const uint2 grid = uint2(srt.shadow_tint_grid);
  float2 texel = float2(index % grid.x, (index / grid.x) % grid.y);
  /* The proxy covers one pixel center, even where the grid is reused for later records. */
  const float2 corner_offset = float2(corner == 1u ? 1.5f : 0.0f, corner == 2u ? 1.5f : 0.0f);
  interp.footprint = corner_offset - 0.5f;
  out_position = float4((texel + corner_offset) * (2.0f / float2(grid)) - 1.0f, 0.0f, 1.0f);
}

struct ShadowTintResources {
  [[legacy_info]] ShaderCreateInfo eevee_surf_shadow_tint;
};

[[fragment]] void surf_shadow_tint([[resource_table]] KernelGlobals &kg,
                                   [[resource_table]] const ShadowTintResources & /*srt*/,
                                   [[in]] const ShadowTintInterp &interp,
                                   [[frag_coord]] const float4 frag_co)
{
  ShadingData sd = {};
  sd.frag_co = frag_co;
  hit_eval_shadow_tint(kg, sd, interp.hit_index, interp.footprint);
}

}  // namespace eevee
