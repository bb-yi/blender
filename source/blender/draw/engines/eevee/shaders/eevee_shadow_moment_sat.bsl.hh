/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_defines.hh"
#include "eevee_shadow_shared.hh"
#include "gpu_shader_math_base.bsl.hh"

namespace eevee::shadow {

struct MomentSAT {
  [[storage(6, read)]] const uint (&dst_coord_buf)[SHADOW_RENDER_MAP_SIZE];
  [[sampler(0)]] usampler2DArrayAtomic depth_tx;
  [[sampler(1)]] usampler2DArray caster_tx;
  [[push_constant]] bool has_casters;
  [[image(0, write, SFLOAT_32_32_32_32)]] image2DArray sat_img;
  [[shared]] float2 bounds[16];
  [[shared]] float4 prefix[4][4];
  [[shared]] float count[4][4];
};

/* Each workgroup operates on one 32x32 physical tile from the list of pages actually redrawn.
 * Each thread reduces an 8x8 cell. Local normalization avoids powers of large world depths and
 * the short prefix sums avoid catastrophic subtraction at the far end of a large atlas. */
[[compute, local_size(4, 4)]]
void moment_sat_comp([[resource_table]] MomentSAT &s,
                     [[work_group_id]] const uint3 group,
                     [[local_invocation_id]] const uint3 local_id)
{
  constexpr int cell = SHADOW_PAGE_CLEAR_GROUP_SIZE / 4;
  uint3 page = shadow_page_unpack(s.dst_coord_buf[group.z]);
  int2 cell_co = int2(page.xy) * (SHADOW_PAGE_RES / cell) + int2(group.xy * 4u + local_id.xy);
  int2 id = int2(local_id.xy);
  int lane = id.y * 4 + id.x;
  float depths[cell * cell];
  float2 range = float2(FLT_MAX, -FLT_MAX);
  int owner = -2; /* Empty cell; -1 denotes mixed or unrepresentable ownership. */
  for (int y = 0; y < cell; y++) {
    for (int x = 0; x < cell; x++) {
      float d = uintBitsToFloat(texelFetch(s.depth_tx, int3(cell_co * cell + int2(x, y), int(page.z)), 0).r);
      depths[y * cell + x] = d;
      if (d >= 0.0f && d < 1e30f) {
        if (s.has_casters && owner != -1) {
          uint caster = texelFetch(s.caster_tx, int3(cell_co * cell + int2(x, y), int(page.z)), 0).r;
          int current = caster <= 16777215u ? int(caster) : -1;
          owner = owner == -2 || owner == current ? current : -1;
        }
        range = float2(min(range.x, d), max(range.y, d));
      }
    }
  }
  s.bounds[lane] = range;
  barrier();
  for (int stride = 8; stride > 0; stride /= 2) {
    if (lane < stride) {
      s.bounds[lane] = float2(min(s.bounds[lane].x, s.bounds[lane + stride].x),
                              max(s.bounds[lane].y, s.bounds[lane + stride].y));
    }
    barrier();
  }
  range = s.bounds[0];
  if (range.y < range.x) {
    range = float2(0.0f);
  }
  float inv_span = range.y > range.x ? 1.0f / (range.y - range.x) : 0.0f;
  float4 m = float4(0.0f);
  float valid_depths = 0.0f;
  for (int i = 0; i < cell * cell; i++) {
    if (depths[i] >= 0.0f && depths[i] < 1e30f) {
      float z = (depths[i] - range.x) * inv_span;
      float z2 = z * z;
      m += float4(z, z2, z * z2, z2 * z2);
      valid_depths += 1.0f;
    }
  }
  s.prefix[id.y][id.x] = m;
  s.count[id.y][id.x] = valid_depths;
  barrier();
  if (id.x == 0) {
    for (int x = 1; x < 4; x++) {
      s.prefix[id.y][x] += s.prefix[id.y][x - 1];
      s.count[id.y][x] += s.count[id.y][x - 1];
    }
  }
  barrier();
  if (id.y == 0) {
    for (int y = 1; y < 4; y++) {
      s.prefix[y][id.x] += s.prefix[y - 1][id.x];
      s.count[y][id.x] += s.count[y - 1][id.x];
    }
  }
  barrier();
  imageStoreFast(s.sat_img, int3(cell_co, int(page.z) * 2), s.prefix[id.y][id.x]);
  imageStoreFast(s.sat_img, int3(cell_co, int(page.z) * 2 + 1),
                 float4(s.count[id.y][id.x], range, float(owner)));
}

PipelineCompute moment_sat(moment_sat_comp);

}  // namespace eevee::shadow
