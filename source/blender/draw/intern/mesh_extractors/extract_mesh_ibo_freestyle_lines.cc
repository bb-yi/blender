/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "BLI_array.hh"
#include "BLI_math_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"

#include "GPU_index_buffer.hh"

#include "draw_subdivision.hh"
#include "extract_mesh.hh"

namespace blender::draw {

static gpu::IndexBufPtr build_freestyle_lines_ibo(const Span<uint2> lines, const int max_index)
{
  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_LINES, lines.size(), max_index);
  if (!lines.is_empty()) {
    MutableSpan<uint2> data = GPU_indexbuf_get_data(&builder).cast<uint2>();
    data.copy_from(lines);
  }
  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(&builder, 0, max_index, false));
}

static bool mesh_edge_visible(const MeshRenderData &mr,
                              const int edge,
                              const bool use_subsurf_optimal_display)
{
  if (use_subsurf_optimal_display &&
      !mr.mesh->runtime->subsurf_optimal_display_edges.is_empty() &&
      !mr.mesh->runtime->subsurf_optimal_display_edges[edge])
  {
    return false;
  }
  if (!mr.hide_edge.is_empty() && mr.hide_edge[edge]) {
    return false;
  }
  if (mr.hide_unmapped_edges && mr.orig_index_edge != nullptr &&
      mr.orig_index_edge[edge] == ORIGINDEX_NONE)
  {
    return false;
  }
  return true;
}

static bool bmesh_edge_visible(const BMEdge &edge)
{
  return !BM_elem_flag_test_bool(&edge, BM_ELEM_HIDDEN);
}

static void extract_freestyle_lines_mesh(const MeshRenderData &mr, gpu::IndexBufPtr &ibo)
{
  const int max_index = mr.corners_num + mr.loose_edges.size() * 2;
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const bke::AttributeReader<bool> freestyle_edge_attr = attributes.lookup<bool>(
      "freestyle_edge", bke::AttrDomain::Edge);
  if (!freestyle_edge_attr) {
    ibo = build_freestyle_lines_ibo(Span<uint2>(), max_index);
    return;
  }

  const VArraySpan<bool> freestyle_edges(freestyle_edge_attr.varray);

  Vector<uint2> lines;
  Array<bool> used(mr.edges_num, false);
  const OffsetIndices faces = mr.faces;
  const Span<int> corner_edges = mr.corner_edges;
  for (const int face_index : faces.index_range()) {
    const IndexRange face = faces[face_index];
    for (const int corner : face) {
      const int edge = corner_edges[corner];
      if (!used[edge] && mesh_edge_visible(mr, edge, true) && freestyle_edges[edge]) {
        used[edge] = true;
        lines.append(edge_from_corners(face, corner));
      }
    }
  }

  Array<int, 64> loose_edges(mr.loose_edges.size());
  mr.loose_edges.to_indices(loose_edges.as_mutable_span());
  for (const int loose_edge_index : loose_edges.index_range()) {
    const int edge = loose_edges[loose_edge_index];
    if (!used[edge] && mesh_edge_visible(mr, edge, false) && freestyle_edges[edge]) {
      const uint corner_a = mr.corners_num + loose_edge_index * 2;
      const uint corner_b = mr.corners_num + loose_edge_index * 2 + 1;
      lines.append(uint2(corner_a, corner_b));
    }
  }

  ibo = build_freestyle_lines_ibo(lines, max_index);
}

static void extract_freestyle_lines_bm(const MeshRenderData &mr, gpu::IndexBufPtr &ibo)
{
  BMesh *bm = mr.bm;
  const int freestyle_edge_ofs = mr.freestyle_edge_ofs;
  if (freestyle_edge_ofs == -1) {
    ibo = build_freestyle_lines_ibo(Span<uint2>(), mr.corners_num + mr.loose_edges.size() * 2);
    return;
  }

  const int max_index = mr.corners_num + mr.loose_edges.size() * 2;
  Array<int> loose_edge_map(bm->totedge, -1);
  for (const int loose_edge_index : mr.loose_edges.index_range()) {
    loose_edge_map[mr.loose_edges[loose_edge_index]] = loose_edge_index;
  }

  Vector<uint2> lines;
  BMIter iter;
  BMEdge *eed;
  BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
    if (bmesh_edge_visible(*eed) && BM_ELEM_CD_GET_BOOL(eed, freestyle_edge_ofs))
    {
      if (eed->l) {
        lines.append(uint2(BM_elem_index_get(eed->l), BM_elem_index_get(eed->l->next)));
      }
      else {
        const int edge = BM_elem_index_get(eed);
        const int loose_edge_index = loose_edge_map[edge];
        if (loose_edge_index != -1) {
          const uint corner_a = mr.corners_num + loose_edge_index * 2;
          const uint corner_b = mr.corners_num + loose_edge_index * 2 + 1;
          lines.append(uint2(corner_a, corner_b));
        }
      }
    }
  }

  ibo = build_freestyle_lines_ibo(lines, max_index);
}

gpu::IndexBufPtr extract_freestyle_lines(const MeshRenderData &mr)
{
  gpu::IndexBufPtr ibo;
  if (mr.extract_type == MeshExtractType::Mesh) {
    extract_freestyle_lines_mesh(mr, ibo);
  }
  else {
    extract_freestyle_lines_bm(mr, ibo);
  }
  return ibo;
}

gpu::IndexBufPtr extract_freestyle_lines_subdiv(const DRWSubdivCache &subdiv_cache,
                                                const MeshRenderData &mr)
{
  const int max_index = subdiv_full_vbo_size(mr, subdiv_cache);

  Array<bool> marked_edges(mr.edges_num, false);
  bool has_marked_edge = false;
  if (mr.extract_type == MeshExtractType::Mesh) {
    const bke::AttributeAccessor attributes = mr.mesh->attributes();
    const bke::AttributeReader<bool> freestyle_edge_attr = attributes.lookup<bool>(
        "freestyle_edge", bke::AttrDomain::Edge);
    if (!freestyle_edge_attr) {
      return build_freestyle_lines_ibo(Span<uint2>(), max_index);
    }
    const VArraySpan<bool> freestyle_edges(freestyle_edge_attr.varray);
    for (const int edge : IndexRange(mr.edges_num)) {
      marked_edges[edge] = mesh_edge_visible(mr, edge, false) && freestyle_edges[edge];
      has_marked_edge = has_marked_edge || marked_edges[edge];
    }
  }
  else {
    if (mr.freestyle_edge_ofs == -1) {
      return build_freestyle_lines_ibo(Span<uint2>(), max_index);
    }
    BMIter iter;
    BMEdge *eed;
    BM_ITER_MESH (eed, &iter, mr.bm, BM_EDGES_OF_MESH) {
      const int edge = BM_elem_index_get(eed);
      if (edge >= 0 && edge < mr.edges_num) {
        marked_edges[edge] = bmesh_edge_visible(*eed) &&
                             BM_ELEM_CD_GET_BOOL(eed, mr.freestyle_edge_ofs);
        has_marked_edge = has_marked_edge || marked_edges[edge];
      }
    }
  }
  if (!has_marked_edge) {
    return build_freestyle_lines_ibo(Span<uint2>(), max_index);
  }

  const Span<int> subdiv_loop_edge_index = subdiv_cache.edges_orig_index->data<int>();
  const Span<int> subdiv_loop_edge_draw_flag = subdiv_cache.edges_draw_flag->data<int>();
  const Span<int> subdiv_loop_subdiv_edge_index(subdiv_cache.subdiv_loop_subdiv_edge_index,
                                                subdiv_cache.num_subdiv_loops);
  Vector<uint2> lines;
  Array<bool> used_subdiv_edges(subdiv_cache.num_subdiv_edges, false);

  const int quads_num = subdiv_cache.num_subdiv_quads;
  for (const int subdiv_quad_index : IndexRange(quads_num)) {
    const IndexRange subdiv_face(subdiv_quad_index * 4, 4);
    for (const int corner : subdiv_face) {
      if (subdiv_loop_edge_draw_flag[corner] == 0) {
        continue;
      }
      const int coarse_edge_index = subdiv_loop_edge_index[corner];
      if (coarse_edge_index < 0 || coarse_edge_index >= mr.edges_num ||
          !marked_edges[coarse_edge_index])
      {
        continue;
      }
      const int subdiv_edge_index = subdiv_loop_subdiv_edge_index[corner];
      if (subdiv_edge_index < 0 || subdiv_edge_index >= subdiv_cache.num_subdiv_edges) {
        continue;
      }
      if (!used_subdiv_edges[subdiv_edge_index]) {
        used_subdiv_edges[subdiv_edge_index] = true;
        lines.append(edge_from_corners(subdiv_face, corner));
      }
    }
  }

  const int edges_per_edge = subdiv_edges_per_coarse_edge(subdiv_cache);
  const int loose_start = subdiv_cache.num_subdiv_loops;
  Array<int, 64> loose_edges(mr.loose_edges.size());
  mr.loose_edges.to_indices(loose_edges.as_mutable_span());
  for (const int loose_edge_index : loose_edges.index_range()) {
    const int edge = loose_edges[loose_edge_index];
    if (edge < 0 || edge >= mr.edges_num || !marked_edges[edge]) {
      continue;
    }
    const int vertex_start = loose_start + loose_edge_index * edges_per_edge * 2;
    for (const int i : IndexRange(edges_per_edge)) {
      lines.append(uint2(vertex_start + i * 2, vertex_start + i * 2 + 1));
    }
  }

  return build_freestyle_lines_ibo(lines, max_index);
}

}  // namespace blender::draw
