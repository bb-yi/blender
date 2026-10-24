/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Material tessellation of meshes (#MAT_GEOM_MESH_TESS).
 *
 * Each triangle of the mesh batch is expanded into `max_expand^2` triangles using primitive
 * expansion. The vertex shader reads the original mesh data from storage buffers and places the
 * vertices on a triangular lattice.
 *
 * Every edge has its own subdivision factor which only depends on its two end points (taken in a
 * canonical order) and on the main camera. This makes both triangles sharing an edge generate the
 * exact same vertices along the edge (no cracks), and makes every pass (including shadows and
 * light-probes) generate the same geometry.
 *
 * View dependent modes use continuous factors with fractional-odd partitioning, like the Unity /
 * Direct3D tessellator: the lattice resolution of an edge is the smallest odd number not below
 * its factor, and the two segments adjacent to the center segment grow smoothly from zero length
 * as the factor increases between two odd numbers. Vertices slide continuously instead of popping
 * when the subdivision level changes.
 *
 * The lattice of a triangle uses the highest resolution of its 3 edges, warped by the matching
 * factor. The vertices on an edge with a lower factor are snapped to the edge own (warped)
 * lattice, producing degenerate triangles. Unused sub-triangles are degenerate.
 */

#pragma once

#include "infos/eevee_geom_infos.hh"

SHADER_LIBRARY_CREATE_INFO(eevee_geom_mesh_tess_data)

#include "gpu_shader_attribute_fetch_lib.glsl"
#include "gpu_shader_index_load_lib.glsl"
#include "gpu_shader_math_vector_safe_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

/* Must match #NodeShaderTessellationMode. */
#define TESS_MODE_FIXED 0
#define TESS_MODE_DISTANCE 1
#define TESS_MODE_SCREEN_EDGE 2

struct TessSettings {
  int mode;
  int min_level;
  int max_level;
  /* Primitive expansion budget, bounded on the CPU using the evaluated mesh and all instances.
   * The edge factors still use the original min/max levels for identical geometry in all passes. */
  int max_expand;
  /* Bound on how far vertices can move (vertex displacement plus a margin for smoothing), used
   * to cull conservatively. Negative when the material displaces vertices without a declared
   * bound: culling is then disabled. */
  float cull_margin;
  /* Whether the material culls back-facing triangles. */
  bool backface_cull;
  /* Near distance or target edge length in pixels, depending on the mode. */
  float param_a;
  /* Far distance. */
  float param_b;
  float phong;
};

/** Unpack the settings. Must match #node_shader_tessellation_settings_pack. */
TessSettings tess_settings_unpack(float4 encoded)
{
  /* The packed value can be 2^24 - 1, which the +0.5 rounding would turn into 2^24. */
  int bits = min(int(encoded.x + 0.5f), 16777215);
  TessSettings settings;
  settings.max_level = clamp(bits & 15, 1, 15);
  settings.min_level = clamp((bits >> 4) & 15, 1, settings.max_level);
  settings.mode = (bits >> 8) & 3;
  settings.max_expand = clamp(tessellation_expand, 1, max((bits >> 10) & 15, 1));
  /* Quantized `Material::max_vertex_displacement` (1/32 unit steps, rounded up). 511 marks
   * unbounded displacement: culling is disabled. */
  settings.cull_margin =
      ((bits >> 14) & 511) == 511 ? -1.0f : float((bits >> 14) & 511) * (1.0f / 32.0f);
  settings.backface_cull = ((bits >> 23) & 1) != 0;
  settings.param_a = encoded.y;
  settings.param_b = encoded.z;
  settings.phong = encoded.w;
  return settings;
}

/* -------------------------------------------------------------------- */
/** \name Mesh Data
 * \{ */

uint tess_index_load(uint element_index)
{
  return gpu_index_load(element_index);
}

float3 tess_pos_load(uint vertex_index)
{
  return gpu_attr_fetch_float4(vbo_pos, gpu_attr_17, gpu_attr_17_meta, vertex_index).xyz;
}

float3 tess_nor_load(uint vertex_index)
{
  return gpu_attr_fetch_float4(vbo_nor, gpu_attr_18, gpu_attr_18_meta, vertex_index).xyz;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge Levels
 * \{ */

/** Strict ordering of positions, independent of the triangle the edge is part of. */
bool tess_point_less(float3 a, float3 b)
{
  if (a.x != b.x) {
    return a.x < b.x;
  }
  if (a.y != b.y) {
    return a.y < b.y;
  }
  return a.z < b.z;
}

/**
 * Subdivision factor of an edge, in number of segments. The end points must be given in
 * canonical order. View dependent modes return a continuous factor, which is partitioned with
 * #tess_edge_warp so that the lattice deforms smoothly instead of jumping between levels.
 * \param camera_position: World space position of the main camera.
 * \param camera_persmat: View projection matrix of the main camera.
 * \param viewport_size: Render size in pixels.
 */
float tess_edge_factor(TessSettings settings,
                       float3 P_min,
                       float3 P_max,
                       float3 camera_position,
                       float4x4 camera_persmat,
                       float2 viewport_size)
{
  if (settings.mode == TESS_MODE_FIXED || settings.min_level == settings.max_level) {
    return float(settings.max_level);
  }

  if (settings.mode == TESS_MODE_DISTANCE) {
    float dist = distance(camera_position, (P_min + P_max) * 0.5f);
    float range = max(settings.param_b - settings.param_a, 1e-6f);
    float fac = saturate((dist - settings.param_a) / range);
    return mix(float(settings.max_level), float(settings.min_level), fac);
  }

  /* Screen edge length. */
  float4 hs_min = camera_persmat * float4(P_min, 1.0f);
  float4 hs_max = camera_persmat * float4(P_max, 1.0f);
  if (hs_min.w <= 1e-6f && hs_max.w <= 1e-6f) {
    /* Behind the camera. */
    return float(settings.min_level);
  }
  if (hs_min.w <= 1e-6f || hs_max.w <= 1e-6f) {
    /* Crossing the camera plane. The projected length is unbounded. */
    return float(settings.max_level);
  }
  float2 ss_min = (hs_min.xy / hs_min.w) * 0.5f * viewport_size;
  float2 ss_max = (hs_max.xy / hs_max.w) * 0.5f * viewport_size;
  float edge_pixels = distance(ss_min, ss_max);
  return clamp(edge_pixels / max(settings.param_a, 1.0f),
               float(settings.min_level),
               float(settings.max_level));
}

/**
 * Smallest odd number not below the factor `f`: the lattice resolution used by fractional-odd
 * partitioning.
 */
int tess_odd_ceil(float f)
{
  return int(ceil(f)) | 1;
}

/**
 * Position along an edge of lattice point `m` (0 <= m <= `n`), where `n` is the odd lattice
 * resolution of the factor `f`. The two segments adjacent to the center one grow linearly from
 * zero length as `f` increases from `n - 2` to `n`, so the lattice points slide smoothly and meet
 * pairwise at `f == n - 2`, reproducing the uniform `n - 2` partitioning.
 */
float tess_edge_warp(int m, int n, float f)
{
  if (n < 3) {
    /* Single segment (f == 1). */
    return float(m);
  }
  const int center = (n - 1) >> 1;
  const float shrink = 1.0f - 0.5f * (f - float(n - 2));
  float w = float(m);
  if (m >= center) {
    w -= shrink;
  }
  if (m >= center + 2) {
    w -= shrink;
  }
  return w / f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lattice
 * \{ */

/**
 * Corner of a sub-triangle of a triangular lattice, as integer barycentric coordinates
 * (weights of the original triangle corners, summing to `level`).
 * The sub-triangles have the same winding as the original triangle.
 */
int3 tess_lattice_corner(int level, int sub_triangle, int corner)
{
  /* Row `r` (starting at the first corner) contains `2r + 1` sub-triangles. */
  int row = int(sqrt(float(sub_triangle)));
  /* Fix floating point imprecision. */
  row -= int(row * row > sub_triangle);
  row += int((row + 1) * (row + 1) <= sub_triangle);
  int k = sub_triangle - row * row;
  int t = k >> 1;

  int2 lattice; /* Row and column. */
  if ((k & 1) == 0) {
    /* Triangle pointing toward the first corner. */
    lattice = (corner == 0) ? int2(row, t) : (corner == 1) ? int2(row + 1, t) : int2(row + 1, t + 1);
  }
  else {
    lattice = (corner == 0) ? int2(row, t) :
              (corner == 1) ? int2(row + 1, t + 1) :
                              int2(row, t + 1);
  }
  return int3(level - lattice.x, lattice.x - lattice.y, lattice.y);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tessellated Vertex
 * \{ */

struct TessVertex {
  /* Vertex of a discarded sub-triangle. */
  bool is_degenerate;
  /* Vertex indices of the original triangle. */
  uint3 indices;
  /* Barycentric weights of the original triangle corners. Exactly zero for the corner opposite
   * to the edge the vertex lies on. */
  float3 weights;
  /* Object space position on the flat original triangle. */
  float3 lP;
  /* Object space corners of the original triangle. */
  float3 P0, P1, P2;
  float3 N0, N1, N2;
};

float3 tess_corner_get(float3 v0, float3 v1, float3 v2, int i)
{
  return (i == 0) ? v0 : ((i == 1) ? v1 : v2);
}

/**
 * True if the (possibly displaced and smoothed) triangle covers no pixel of the current view:
 * all its corners are beyond the same frustum plane by more than `margin`, or it faces away
 * and the material culls back-faces. `margin` bounds how far the vertices can still move, so
 * culled triangles cannot be displaced or smoothed back into view.
 *
 * The tests use the view being rendered, not the main camera, so shadow and probe views still
 * receive the triangles they need. Back-facing triangles are not culled in shadow views:
 * shadows need both faces regardless of the surface cull method.
 */
bool tess_triangle_culled(TessSettings settings,
                          float3 wP0,
                          float3 wP1,
                          float3 wP2,
                          float3 face_nor,
                          float4x4 view_persmat,
                          float3 view_position,
                          float3 view_forward,
                          bool view_ortho,
                          bool shadow_view)
{
  if (settings.cull_margin < 0.0f) {
    /* The material displaces vertices without a declared bound. Nothing can be culled. */
    return false;
  }
  float edge01 = 0.0f, edge02 = 0.0f, edge12 = 0.0f;
  if (settings.phong > 0.0f || (settings.backface_cull && !shadow_view)) {
    edge01 = distance(wP0, wP1);
    edge02 = distance(wP0, wP2);
    edge12 = distance(wP1, wP2);
  }
  float margin = settings.cull_margin + settings.phong * max(edge01, max(edge02, edge12)) +
                 1e-4f;

  /* Frustum planes from the view projection matrix rows (inside: `dot(plane, hP) >= 0`). The
   * near plane `z + w >= 0` is also valid (weaker) for the [0, w] clip range convention. */
  float4x4 pm = transpose(view_persmat);
  float4 planes[6];
  planes[0] = pm[3] + pm[0];
  planes[1] = pm[3] - pm[0];
  planes[2] = pm[3] + pm[1];
  planes[3] = pm[3] - pm[1];
  planes[4] = pm[3] + pm[2];
  planes[5] = pm[3] - pm[2];
  for (int i = 0; i < 6; i++) {
    float bound = -margin * length(planes[i].xyz);
    if (dot(planes[i], float4(wP0, 1.0f)) < bound &&
        dot(planes[i], float4(wP1, 1.0f)) < bound && dot(planes[i], float4(wP2, 1.0f)) < bound)
    {
      return true;
    }
  }

  if (settings.backface_cull && !shadow_view) {
    float3 V = view_ortho ? view_forward :
                            (view_position - (wP0 + wP1 + wP2) * (1.0f / 3.0f));
    /* Moving a corner by `margin` can tilt the triangle by at most `2*margin/min_edge`. */
    float tilt = min(2.0f * margin / max(min(edge01, min(edge02, edge12)), 1e-6f), 1.0f);
    if (dot(face_nor, V) < -tilt * length(face_nor) * length(V)) {
      return true;
    }
  }
  return false;
}

/**
 * Compute the tessellated vertex for the given vertex index of the expanded draw-call.
 * \param model: Object to world matrix. Used to compute the edge levels in world space.
 * \param view_*: Clip matrix, origin and forward of the view being rendered (not the main
 * camera) used for conservative per-view culling.
 */
TessVertex tess_vertex_get(TessSettings settings,
                           int vert_id,
                           float4x4 model,
                           float3 camera_position,
                           float4x4 camera_persmat,
                           float2 viewport_size,
                           float4x4 view_persmat,
                           float3 view_position,
                           float3 view_forward,
                           bool view_ortho,
                           bool shadow_view)
{
  TessVertex tv;
  const int max_expand = settings.max_expand;
  const int expanded_vert_len = 3 * max_expand * max_expand;
  const int triangle = vert_id / expanded_vert_len;
  const int local_vert = vert_id - triangle * expanded_vert_len;
  const int sub_triangle = local_vert / 3;
  const int corner = local_vert - sub_triangle * 3;

  tv.indices = uint3(tess_index_load(uint(triangle) * 3u + 0u),
                     tess_index_load(uint(triangle) * 3u + 1u),
                     tess_index_load(uint(triangle) * 3u + 2u));
  tv.P0 = tess_pos_load(tv.indices.x);
  tv.P1 = tess_pos_load(tv.indices.y);
  tv.P2 = tess_pos_load(tv.indices.z);

  const float3 wP0 = (model * float4(tv.P0, 1.0f)).xyz;
  const float3 wP1 = (model * float4(tv.P1, 1.0f)).xyz;
  const float3 wP2 = (model * float4(tv.P2, 1.0f)).xyz;

  /* Mirrored object transforms flip the winding; keep the face normal front-facing. */
  float3 face_nor = float3(0.0f);
  if (settings.backface_cull && !shadow_view && settings.cull_margin >= 0.0f) {
    face_nor = cross(wP1 - wP0, wP2 - wP0);
    if (dot(cross(model[0].xyz, model[1].xyz), model[2].xyz) < 0.0f) {
      face_nor = -face_nor;
    }
  }
  if (tess_triangle_culled(settings,
                           wP0,
                           wP1,
                           wP2,
                           face_nor,
                           view_persmat,
                           view_position,
                           view_forward,
                           view_ortho,
                           shadow_view))
  {
    tv.is_degenerate = true;
    tv.weights = float3(1.0f, 0.0f, 0.0f);
    tv.lP = tv.P0;
    return tv;
  }

  /* Edge `i` is opposite to corner `i`. Factors are computed using the canonical edge order. */
  const bool fractional = settings.mode != TESS_MODE_FIXED &&
                          settings.min_level != settings.max_level;
  float3 edge_fac = float3(settings.max_level);
  if (fractional) {
    for (int i = 0; i < 3; i++) {
      float3 wPa = tess_corner_get(wP0, wP1, wP2, (i + 1) % 3);
      float3 wPb = tess_corner_get(wP0, wP1, wP2, (i + 2) % 3);
      bool is_ordered = tess_point_less(wPa, wPb);
      edge_fac[i] = tess_edge_factor(settings,
                                     is_ordered ? wPa : wPb,
                                     is_ordered ? wPb : wPa,
                                     camera_position,
                                     camera_persmat,
                                     viewport_size);
    }
  }
  /* View dependent modes use fractional-odd partitioning for smooth level transitions. */
  const int3 edge_len = fractional ? int3(tess_odd_ceil(edge_fac.x),
                                          tess_odd_ceil(edge_fac.y),
                                          tess_odd_ceil(edge_fac.z)) :
                                     int3(settings.max_level);
  const int level = max(edge_len.x, max(edge_len.y, edge_len.z));
  const float level_fac = fractional ? max(edge_fac.x, max(edge_fac.y, edge_fac.z)) : float(level);
  tv.is_degenerate = sub_triangle >= level * level;
  if (tv.is_degenerate) {
    tv.weights = float3(1.0f, 0.0f, 0.0f);
    tv.lP = tv.P0;
    return tv;
  }

  /* Discarded vertices need neither normals nor material attributes/displacement. */
  tv.N0 = tess_nor_load(tv.indices.x);
  tv.N1 = tess_nor_load(tv.indices.y);
  tv.N2 = tess_nor_load(tv.indices.z);

  int3 lattice = tess_lattice_corner(level, sub_triangle, corner);

  int zero_count = int(lattice.x == 0) + int(lattice.y == 0) + int(lattice.z == 0);
  if (zero_count == 0) {
    /* Interior vertex. */
    if (fractional) {
      /* Warp the integer lattice to the fractional partitioning of `level_fac`. The third
       * weight is implied so that the warped lattice still covers the whole triangle. */
      tv.weights.x = tess_edge_warp(lattice.x, level, level_fac);
      tv.weights.y = tess_edge_warp(lattice.y, level, level_fac);
      tv.weights.z = 1.0f - tv.weights.x - tv.weights.y;
    }
    else {
      tv.weights = float3(lattice) / float(level);
    }
    tv.lP = tv.weights.x * tv.P0 + tv.weights.y * tv.P1 + tv.weights.z * tv.P2;
    return tv;
  }

  /* Vertex on an edge (or a corner). Snap it to the edge own lattice and compute it from the
   * canonically ordered edge end points so that both adjacent triangles match exactly. */
  int opposite = (lattice.x == 0) ? 0 : ((lattice.y == 0) ? 1 : 2);
  int corner_a = (opposite + 1) % 3;
  int corner_b = (opposite + 2) % 3;
  int edge_n = edge_len[opposite];
  /* Position along the edge from `corner_a` in edge lattice units (rounded). */
  int steps = (2 * lattice[corner_b] * edge_n + level) / (2 * level);

  float3 wPa = tess_corner_get(wP0, wP1, wP2, corner_a);
  float3 wPb = tess_corner_get(wP0, wP1, wP2, corner_b);
  bool is_ordered = tess_point_less(wPa, wPb);
  int corner_min = is_ordered ? corner_a : corner_b;
  int corner_max = is_ordered ? corner_b : corner_a;
  int steps_from_min = is_ordered ? steps : (edge_n - steps);
  float fac = fractional ? tess_edge_warp(steps_from_min, edge_n, edge_fac[opposite]) :
                           float(steps_from_min) / float(edge_n);

  tv.weights = float3(0.0f);
  tv.weights[corner_min] = 1.0f - fac;
  tv.weights[corner_max] = fac;
  tv.lP = mix(tess_corner_get(tv.P0, tv.P1, tv.P2, corner_min),
              tess_corner_get(tv.P0, tv.P1, tv.P2, corner_max),
              fac);
  return tv;
}

/** Interpolated object space normal (not normalized). */
float3 tess_normal_interpolate(TessVertex tv, float3 weights)
{
  return weights.x * tv.N0 + weights.y * tv.N1 + weights.z * tv.N2;
}

/**
 * Phong tessellation (Boubekeur & Alexa 2008): blend the projections of the flat position onto
 * the tangent planes of the corners.
 */
float3 tess_phong(TessVertex tv, float3 lP, float3 weights, float factor)
{
  if (factor <= 0.0f) {
    return lP;
  }
  float3 N0 = safe_normalize(tv.N0);
  float3 N1 = safe_normalize(tv.N1);
  float3 N2 = safe_normalize(tv.N2);
  float3 proj0 = lP - dot(lP - tv.P0, N0) * N0;
  float3 proj1 = lP - dot(lP - tv.P1, N1) * N1;
  float3 proj2 = lP - dot(lP - tv.P2, N2) * N2;
  float3 phong_P = weights.x * proj0 + weights.y * proj1 + weights.z * proj2;
  return mix(lP, phong_P, factor);
}

/** \} */
