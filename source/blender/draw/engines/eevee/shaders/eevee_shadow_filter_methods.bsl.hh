/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_shadow_tracing.bsl.hh"

namespace eevee {

/* Values mirror NodeShaderInfoShadowMode. All radii below are world-space lengths in the
 * light basis. Resolve the virtual page for each tap, including cube-face and clipmap edges. */
enum class NodeShadowFilter : int { Hard = 3, PCF = 4, PCSS = 5, VSM = 6, VSSM = 7, MSM = 8, CHS = 9 };

struct NodeFilterReceiver {
  float3 position;
  float3 normal;
  float3 right;
  float3 up;
  float depth;
  float screen_pixel;
  float depth_slope;
  bool directional;
};

struct NodeFilterSamples {
  float count;
  float finite_count;
  float blocked;
  float2 ownership;
  float blocker_separation;
  float depth_scale;
  float4 moments;
  float sat_count;
  float sat_occlusion;
  float sat_blocker_sum;
  float2 sat_ownership;
};

/* Four-moment Hamburger bound (Peters and Klein, Moment Shadow Mapping, I3D 2015).
 * The direct kernel uses receiver-relative moments, the SAT uses its tile's normalized depths.
 * Both represent identical depth tests under an affine change of coordinates. */
float node_distribution_visibility(float4 b, float t, bool four_moments)
{
  float variance = max(0.0f, b.y - b.x * b.x);
  float distance = t - b.x;
  float lit = distance <= 0.0f ? 1.0f : variance / max(variance + distance * distance, 1e-20f);
  if (four_moments) {
    float cross_moment = b.z - b.x * b.y;
    float determinant = variance * (b.w - b.y * b.y) - cross_moment * cross_moment;
    /* A rank-two moment matrix represents two distinct depths. Recover both atoms and their
     * masses directly instead of dividing by a nearly zero third Cholesky pivot. */
    if (variance > 1e-7f && determinant <= 1e-7f * variance) {
      float half_sum = 0.5f * cross_moment / variance;
      float rad = sqrt(max(0.0f, half_sum * half_sum + b.y - 2.0f * half_sum * b.x));
      if (rad > 1e-6f) {
        lit = t <= half_sum - rad ? 1.0f : t > half_sum + rad ? 0.0f :
                  (b.x - half_sum + rad) / (2.0f * rad);
      }
    }
    if (variance > 1e-7f && determinant > 1e-7f * variance) {
      float l32 = cross_moment / variance;
      float c2 = (t * t - b.y - l32 * distance) * variance / determinant;
      float c1 = distance / variance - l32 * c2;
      float c0 = 1.0f - c1 * b.x - c2 * b.y;
      if (abs(c2) > 1e-7f) {
        float half_p = 0.5f * c1 / c2;
        float radicand = half_p * half_p - c0 / c2;
        if (radicand > 0.0f) {
          float r = sqrt(radicand);
          float z1 = -half_p - r, z2 = -half_p + r;
          if (z1 >= t) {
            lit = 1.0f;
          }
          else if (z2 < t) {
            lit = (z1 * z2 - b.x * (z1 + z2) + b.y) / ((t - z2) * (t - z1));
          }
          else {
            lit = 1.0f - (t * z2 - b.x * (t + z2) + b.y) / ((z2 - z1) * (t - z1));
          }
        }
      }
    }
  }
  return saturate(lit);
}

float node_sample_occlusion(NodeFilterSamples s, bool four)
{
  if (s.blocked == 0.0f || s.blocked == s.finite_count) {
    return s.blocked;
  }
  return s.finite_count * (1.0f - node_distribution_visibility(s.moments / s.finite_count, 0.0f, four));
}

/* SAT prefix coordinates are local to a physical macro tile. A zero prefix lies just outside
 * its left/bottom edge, so sums never blend unrelated virtual pages or cube faces. */
void node_prefix_add([[resource_table]] ShadowRenderData &srd, int3 base, int2 p, float sign,
                     float4 &sum, float &count)
{
  if (all(greaterThanEqual(p, int2(0)))) {
    int3 co = base + int3(p, 0);
    sum += sign * srd.read_node_moment(co);
    count += sign * srd.read_node_moment(co + int3(0, 0, 1)).x;
  }
}

void node_sat_sample([[resource_table]] ShadowRenderData &srd, int3 physical, int cells,
                     float receiver, bool four, bool hierarchical_pcf, bool classify,
                     uint receiver_id, NodeFilterSamples &s)
{
  constexpr int cell = SHADOW_PAGE_CLEAR_GROUP_SIZE / 4;
  int2 cell_co = physical.xy / cell;
  int2 tile = (cell_co / 4) * 4;
  int2 lo = ((cell_co - tile) / cells) * cells;
  int2 hi = lo + cells - 1;
  int3 base = int3(tile, physical.z * 2);
  float4 meta = srd.read_node_moment(int3(cell_co, base.z + 1));
  s.sat_count += 1.0f;
  if (receiver <= meta.y) {
    return; /* The macro tile's minimum conservatively bounds every cell in it. */
  }
  if (hierarchical_pcf && receiver <= meta.z) {
    /* Ambiguous depth interval: estimate the selected scale with one depth comparison in each
     * quadrant. The virtual tap position carries the film-dependent rotation into the box. */
    int half_width = cells * cell / 2;
    int2 origin = (tile + lo) * cell;
    int2 offset = (physical.xy - origin) % half_width;
    for (int j = 0; j < 4; j++) {
      int2 quadrant = int2(j & 1, j >> 1);
      int2 sample_co = origin + quadrant * half_width + offset;
      int3 co = int3(sample_co, physical.z);
      float d = uintBitsToFloat(texelFetch(srd.shadow_atlas_tx, co, 0).r);
      if (d < receiver) {
        s.sat_occlusion += 0.25f;
        if (classify) {
          uint id = texelFetch(srd.shadow_caster_atlas_tx, co, 0).r;
          if (id != SHADOW_CASTER_UNKNOWN_ID) {
            s.sat_ownership += id == receiver_id ? float2(0.25f, 0) : float2(0, 0.25f);
          }
        }
      }
    }
    return;
  }
  float4 sum = float4(0.0f);
  float count = 0.0f;
  node_prefix_add(srd, base, hi, 1.0f, sum, count);
  node_prefix_add(srd, base, int2(lo.x - 1, hi.y), -1.0f, sum, count);
  node_prefix_add(srd, base, int2(hi.x, lo.y - 1), -1.0f, sum, count);
  node_prefix_add(srd, base, lo - 1, 1.0f, sum, count);
  if (count <= 0.0f) {
    return;
  }
  float4 b = sum / count;
  float span = meta.z - meta.y;
  float occ = receiver > meta.z ? 1.0f :
                  (1.0f - node_distribution_visibility(b, (receiver - meta.y) * safe_rcp(span), four));
  float coverage = count / float(cells * cell * cells * cell);
  float blocked = occ * coverage;
  s.sat_occlusion += blocked;
  /* Unblocked depth is approximated by the receiver for the VSSM blocker estimate. */
  s.sat_blocker_sum += coverage * clamp(receiver - (meta.y + b.x * span), 0.0f,
                                         occ * max(0.0f, receiver - meta.y));
  if (classify && blocked > 0.0f) {
    /* Moments cannot encode a caster ID. Count ownership from actual depth comparisons only
     * when requested, and normalize to the same reconstructed occlusion as the Shadow socket. */
    int owner = -2;
    for (int y = 0; y < cells && owner != -1; y++) {
      for (int x = 0; x < cells; x++) {
        int id = int(srd.read_node_moment(int3(tile + lo + int2(x, y), base.z + 1)).w);
        if (id != -2) {
          owner = owner == -2 || id == owner ? id : -1;
        }
      }
    }
    if (owner >= 0) {
      s.sat_ownership += uint(owner) == receiver_id ? float2(blocked, 0) : float2(0, blocked);
      return;
    }
    float2 owned = float2(0.0f);
    float hits = 0.0f;
    int2 origin = (tile + lo) * cell;
    for (int y = 0; y < cells * cell; y++) {
      for (int x = 0; x < cells * cell; x++) {
        int3 co = int3(origin + int2(x, y), physical.z);
        float d = uintBitsToFloat(texelFetch(srd.shadow_atlas_tx, co, 0).r);
        if (d < receiver) {
          uint id = texelFetch(srd.shadow_caster_atlas_tx, co, 0).r;
          if (id != SHADOW_CASTER_UNKNOWN_ID) {
            owned += id == receiver_id ? float2(1, 0) : float2(0, 1);
          }
          hits += 1.0f;
        }
      }
    }
    s.sat_ownership += owned * (blocked * safe_rcp(hits));
  }
}

NodeFilterSamples node_filter_kernel([[resource_table]] ShadowRenderData &srd,
                                     LightData light,
                                     NodeFilterReceiver r,
                                     float radius,
                                     float radius_texels,
                                     int taps,
                                     float2 rotation,
                                     bool rectangle,
                                     bool search,
                                     bool moments,
                                     bool four_moments,
                                     bool hierarchical_pcf,
                                     bool classify,
                                     uint receiver_id)
{
  NodeFilterSamples s = {};
  for (int i = 0; i < taps; i++) {
    float2 disk = float2(0.0f);
    if (radius > 0.0f && (!search || i != 0)) {
      float2 xi = fract(random::hammersley_2d(i, taps) + rotation);
      disk = rectangle ? 2.0f * xi - 1.0f : sample_disk(xi);
    }
    float3 q = r.position + radius * (disk.x * r.right + disk.y * r.up);
    ShadowCoordinates coord;
    float plane_depth;
    if (r.directional) {
      float dz = dot(r.normal.xy, q.xy - r.position.xy) * safe_rcp(r.normal.z);
      plane_depth = r.depth + clamp(dz, -2.0f * radius, 2.0f * radius);
      coord = shadow_directional_coordinates(light, q, r.screen_pixel);
    }
    else {
      float denom = dot(r.normal, q);
      float scale = dot(r.normal, r.position) * safe_rcp(denom);
      float dz = scale > 0.0f ? length(q) * scale - r.depth : 0.0f;
      plane_depth = r.depth + clamp(dz, -2.0f * radius, 2.0f * radius);
      int face = shadow_punctual_face_index_get(q);
      coord = shadow_punctual_coordinates(light, shadow_punctual_local_position_to_face_local(face, q), face);
    }
    ShadowSamplingTile tile = shadow_tile_load(srd.shadow_tilemaps_tx, coord.tilemap_tile, coord.tilemap_index);
    if (!tile.is_valid) {
      continue;
    }
    int3 physical = srd.atlas_coordinates(coord, tile);
    float comparison_depth = min(plane_depth, r.depth);
    if (srd.constants.use_node_moment_atlas) [[static_branch]] {
      constexpr int cell = SHADOW_PAGE_CLEAR_GROUP_SIZE / 4;
      float width = radius_texels * inversesqrt(float(taps)) / exp2(float(tile.lod));
      if ((moments || hierarchical_pcf) && width >= float(cell) && textureSize(srd.moment_sat_tx, 0).x > 1) {
        int cells = width >= float(4 * cell) ? 4 : width >= float(2 * cell) ? 2 : 1;
        /* A box stores depths, not their correlation with surface position. On oblique receivers
         * a single comparison depth across the box mixes the receiver's own slope into its
         * variance. Use plane-corrected taps if that variation exceeds one physical texel. */
        if (r.depth_slope * float(cells * cell) <= 2.0f) {
          node_sat_sample(srd, physical, cells, comparison_depth, four_moments, hierarchical_pcf,
                          classify, receiver_id, s);
          continue;
        }
      }
    }
    float depth = uintBitsToFloat(texelFetch(srd.shadow_atlas_tx, physical, 0).r);
    s.count += 1.0f;
    if (depth >= 1e30f) {
      continue; /* Clear texels are exactly lit; do not raise their sentinel depth to a power. */
    }
    s.finite_count += 1.0f;
    float delta = depth - comparison_depth;
    if (delta < 0.0f) {
      s.blocked += 1.0f;
      s.blocker_separation += -delta;
      if (classify) {
        uint id = texelFetch(srd.shadow_caster_atlas_tx, physical, 0).r;
        if (id != SHADOW_CASTER_UNKNOWN_ID) {
          s.ownership += (id == receiver_id) ? float2(1, 0) : float2(0, 1);
        }
      }
    }
    if (moments) {
      /* Keep the center-depth blocker guard, but measure the distribution relative to the
       * receiver plane at this tap. Otherwise a lit tilted floor creates artificial variance. */
      delta = delta < 0.0f ? depth - plane_depth : max(0.0f, depth - plane_depth);
      if (abs(delta) > s.depth_scale) {
        float ratio = s.depth_scale / abs(delta);
        float sq = ratio * ratio;
        s.moments.xy *= float2(ratio, sq);
        if (four_moments) {
          s.moments.zw *= float2(ratio * sq, sq * sq);
        }
        s.depth_scale = abs(delta);
      }
      float z = delta * safe_rcp(s.depth_scale);
      float zz = z * z;
      s.moments.xy += float2(z, zz);
      if (four_moments) {
        s.moments.zw += float2(zz * z, zz * zz);
      }
    }
  }
  return s;
}

float4 shadow_filter_material(LightData light,
                              bool directional,
                              float2 frag_co,
                              float3 P, float3 Ng, float3 N,
                              float normal_offset, float geometry_offset,
                              int method, float radius_input, float max_penumbra, int quality,
                              bool classify, uint receiver_id)
{
  if (light.tilemap_index == LIGHT_NO_SHADOW) {
    return float4(1, 0, 0, 0);
  }
  [[resource_table]] ShadowRenderData &srd = resource_table_get(eevee::ShadowRenderData);
  [[resource_table]] const Uniform &uni = srd.uniforms;
  [[resource_table]] const draw::View &views = srd.views;
  const ViewMatrices view = views.get(0);
  float texel = shadow_texel_radius_at_position(uni, views, light, directional, P);
  float3 L = directional ? light.z_axis() : safe_normalize(light.position() - P);
  NodeFilterReceiver r;
  r.screen_pixel = shadow_screen_pixel_size(uni.uniform_buf.shadow.film_pixel_radius,
                                            view.is_perspective(), view.z_distance(P));
  P = shadow_ray_offset(P, Ng, texel) + Ng * shadow_normal_offset(Ng, L, texel) +
      N * shadow_terminator_offset(N, L, normal_offset, geometry_offset);
  r.directional = directional;
  r.position = directional ? light_world_to_local_direction(light, P) :
                             light_world_to_local_point(light, P) - light.local.local.shadow_position;
  r.normal = light_world_to_local_direction(light, Ng);
  r.depth = directional ? -r.position.z - orderedIntBitsToFloat(light.clip_near) : length(r.position);
  float cos_incidence = directional ? abs(r.normal.z) : abs(dot(r.normal, safe_normalize(r.position)));
  r.depth_slope = sin_from_cos(saturate(cos_incidence)) / max(cos_incidence, 1e-4f);
  r.right = float3(1, 0, 0);
  r.up = float3(0, 1, 0);
  bool chs = method == int(NodeShadowFilter::CHS);
  bool contact = chs || method == int(NodeShadowFilter::PCSS) || method == int(NodeShadowFilter::VSSM);
  bool area = contact && !directional && is_area_light(light.type);
  float source = directional ? tan(light.sun.shadow_angle) : light.local.local.shadow_radius;
  if (area) {
    float2 axes = light.area.size * light.area.shadow_scale;
    source = reduce_max(axes);
    r.right.x = axes.x * safe_rcp(source);
    r.up.y = axes.y * safe_rcp(source);
  }
  else if (!directional) {
    make_orthonormal_basis(safe_normalize(r.position), r.right, r.up);
  }
  bool rectangle = area && light.type == LIGHT_RECT;
  int taps = clamp(quality, 1, 32);
  float radius = texel * clamp(radius_input < 0.0f ? (contact ? 64.0f : light.filter_radius) :
                                                       radius_input, 0.0f, 64.0f);
  if (chs) {
    radius = max(0.0f, max_penumbra);
  }
  if (method == int(NodeShadowFilter::Hard)) {
    radius = 0.0f;
  }
  float2 rotation = float2(0.5f);
  if (srd.constants.shadow_random) [[static_branch]] {
    rotation = srd.util_tx.fetch(frag_co, UTIL_BLUE_NOISE_LAYER).xy;
    if (!srd.constants.shadow_static_noise) [[static_branch]] {
      rotation = fract(rotation + srd.sampling.rng_2D_get(SAMPLING_SHADOW_X));
    }
  }
  if (contact && radius > 0.0f) {
    bool variance_search = method == int(NodeShadowFilter::VSSM);
    float search_radius = min(radius, directional ? source * max(r.depth, 0.0f) :
                                  source * max(r.depth, 0.0f) / max(intBitsToFloat(light.clip_near), 1e-6f));
    if (search_radius > 0.0f) {
      NodeFilterSamples b = node_filter_kernel(srd, light, r, search_radius, search_radius * safe_rcp(texel), min(taps, 16),
          rotation, rectangle, true, variance_search, false, false, false, 0u);
      if (b.blocked + b.sat_occlusion <= 0.0f) {
        return float4(1, 0, 0, 0);
      }
      float gap = (b.blocker_separation + b.sat_blocker_sum) / (b.blocked + b.sat_occlusion);
      if (variance_search) {
        /* With unblocked samples approximated by the receiver depth, E[z] = -(1-p)*gap.
         * Use the measured blockers when that approximation has no positive solution. */
        float occ = node_sample_occlusion(b, false) + b.sat_occlusion;
        float estimate = (b.sat_blocker_sum - b.moments.x * b.depth_scale) / max(occ, 1e-8f);
        if (estimate > 0.0f && occ > 1e-5f) {
          gap = estimate;
        }
      }
      radius = min(radius, directional ? source * gap : source * gap / max(r.depth - gap, 1e-6f));
    }
    else {
      radius = 0.0f;
    }
  }
  bool moments = method >= int(NodeShadowFilter::VSM) && !chs && radius > 0.0f;
  bool four = method == int(NodeShadowFilter::MSM);
  NodeFilterSamples s = node_filter_kernel(srd, light, r, radius, radius * safe_rcp(texel), radius > 0.0f ? taps : 1,
      fract(rotation + float2(0.37f, 0.71f)), rectangle, false, moments, four, chs,
      classify && bool(uni.uniform_buf.shadow.use_caster_atlas), receiver_id);
  float count = s.count + s.sat_count;
  if (count == 0.0f) {
    return float4(1, 0, 0, 0);
  }
  float occlusion = moments ? node_sample_occlusion(s, four) : s.blocked;
  float lit = 1.0f - (occlusion + s.sat_occlusion) / count;
  return float4(lit, (s.ownership * (occlusion * safe_rcp(s.blocked)) + s.sat_ownership) / count, 0);
}

}  // namespace eevee
