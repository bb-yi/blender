/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Material tessellation of meshes (#MAT_GEOM_MESH_TESS).
 *
 * Each triangle of the mesh batch is expanded into `max_level^2` triangles using primitive
 * expansion. The vertex shader reads the original mesh data from storage buffers and places the
 * vertices on a triangular lattice.
 *
 * Every edge has its own subdivision level which only depends on its two end points (taken in a
 * canonical order) and on the main camera. This makes both triangles sharing an edge generate the
 * exact same vertices along the edge (no cracks), and makes every pass (including shadows and
 * light-probes) generate the same geometry.
 *
 * The lattice of a triangle uses the highest level of its 3 edges. The vertices on an edge with a
 * lower level are snapped to the edge own lattice, producing degenerate triangles. Unused
 * sub-triangles are degenerate.
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
  /* Near distance or target edge length in pixels, depending on the mode. */
  float param_a;
  /* Far distance. */
  float param_b;
  float phong;
};

/** Unpack the settings. Must match #node_shader_tessellation_settings_pack. */
TessSettings tess_settings_unpack(float4 encoded)
{
  int bits = int(encoded.x + 0.5f);
  TessSettings settings;
  settings.max_level = clamp(bits & 15, 1, 15);
  settings.min_level = clamp((bits >> 4) & 15, 1, settings.max_level);
  settings.mode = (bits >> 8) & 3;
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
 * Number of segments of an edge. The end points must be given in canonical order.
 * \param camera_position: World space position of the main camera.
 * \param camera_persmat: View projection matrix of the main camera.
 * \param viewport_size: Render size in pixels.
 */
int tess_edge_level(TessSettings settings,
                    float3 P_min,
                    float3 P_max,
                    float3 camera_position,
                    float4x4 camera_persmat,
                    float2 viewport_size)
{
  if (settings.mode == TESS_MODE_FIXED || settings.min_level == settings.max_level) {
    return settings.max_level;
  }

  if (settings.mode == TESS_MODE_DISTANCE) {
    float dist = distance(camera_position, (P_min + P_max) * 0.5f);
    float range = max(settings.param_b - settings.param_a, 1e-6f);
    float fac = saturate((dist - settings.param_a) / range);
    int level = int(round(mix(float(settings.max_level), float(settings.min_level), fac)));
    return clamp(level, settings.min_level, settings.max_level);
  }

  /* Screen edge length. */
  float4 hs_min = camera_persmat * float4(P_min, 1.0f);
  float4 hs_max = camera_persmat * float4(P_max, 1.0f);
  if (hs_min.w <= 1e-6f && hs_max.w <= 1e-6f) {
    /* Behind the camera. */
    return settings.min_level;
  }
  if (hs_min.w <= 1e-6f || hs_max.w <= 1e-6f) {
    /* Crossing the camera plane. The projected length is unbounded. */
    return settings.max_level;
  }
  float2 ss_min = (hs_min.xy / hs_min.w) * 0.5f * viewport_size;
  float2 ss_max = (hs_max.xy / hs_max.w) * 0.5f * viewport_size;
  float edge_pixels = distance(ss_min, ss_max);
  int level = int(ceil(edge_pixels / max(settings.param_a, 1.0f)));
  return clamp(level, settings.min_level, settings.max_level);
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
 * Compute the tessellated vertex for the given vertex index of the expanded draw-call.
 * \param model: Object to world matrix. Used to compute the edge levels in world space.
 */
TessVertex tess_vertex_get(TessSettings settings,
                           int vert_id,
                           float4x4 model,
                           float3 camera_position,
                           float4x4 camera_persmat,
                           float2 viewport_size)
{
  TessVertex tv;
  const int max_level = settings.max_level;
  const int expanded_vert_len = 3 * max_level * max_level;
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
  tv.N0 = tess_nor_load(tv.indices.x);
  tv.N1 = tess_nor_load(tv.indices.y);
  tv.N2 = tess_nor_load(tv.indices.z);

  const float3 wP0 = (model * float4(tv.P0, 1.0f)).xyz;
  const float3 wP1 = (model * float4(tv.P1, 1.0f)).xyz;
  const float3 wP2 = (model * float4(tv.P2, 1.0f)).xyz;

  /* Edge `i` is opposite to corner `i`. Levels are computed using the canonical edge order. */
  int3 edge_level;
  for (int i = 0; i < 3; i++) {
    float3 wPa = tess_corner_get(wP0, wP1, wP2, (i + 1) % 3);
    float3 wPb = tess_corner_get(wP0, wP1, wP2, (i + 2) % 3);
    bool is_ordered = tess_point_less(wPa, wPb);
    edge_level[i] = tess_edge_level(settings,
                                    is_ordered ? wPa : wPb,
                                    is_ordered ? wPb : wPa,
                                    camera_position,
                                    camera_persmat,
                                    viewport_size);
  }
  const int level = max(edge_level.x, max(edge_level.y, edge_level.z));
  tv.is_degenerate = sub_triangle >= level * level;
  if (tv.is_degenerate) {
    tv.weights = float3(1.0f, 0.0f, 0.0f);
    tv.lP = tv.P0;
    return tv;
  }

  int3 lattice = tess_lattice_corner(level, sub_triangle, corner);

  int zero_count = int(lattice.x == 0) + int(lattice.y == 0) + int(lattice.z == 0);
  if (zero_count == 0) {
    /* Interior vertex. */
    tv.weights = float3(lattice) / float(level);
    tv.lP = tv.weights.x * tv.P0 + tv.weights.y * tv.P1 + tv.weights.z * tv.P2;
    return tv;
  }

  /* Vertex on an edge (or a corner). Snap it to the edge own lattice and compute it from the
   * canonically ordered edge end points so that both adjacent triangles match exactly. */
  int opposite = (lattice.x == 0) ? 0 : ((lattice.y == 0) ? 1 : 2);
  int corner_a = (opposite + 1) % 3;
  int corner_b = (opposite + 2) % 3;
  int edge_len = edge_level[opposite];
  /* Position along the edge from `corner_a` in edge lattice units (rounded). */
  int steps = (2 * lattice[corner_b] * edge_len + level) / (2 * level);

  float3 wPa = tess_corner_get(wP0, wP1, wP2, corner_a);
  float3 wPb = tess_corner_get(wP0, wP1, wP2, corner_b);
  bool is_ordered = tess_point_less(wPa, wPb);
  int corner_min = is_ordered ? corner_a : corner_b;
  int corner_max = is_ordered ? corner_b : corner_a;
  int steps_from_min = is_ordered ? steps : (edge_len - steps);
  float fac = float(steps_from_min) / float(edge_len);

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
