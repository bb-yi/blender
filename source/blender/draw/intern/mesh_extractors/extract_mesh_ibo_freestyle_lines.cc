/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

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
  const auto edge_visible = [&](const int edge) {
    return mr.hide_edge.is_empty() || !mr.hide_edge[edge];
  };

  Vector<uint2> lines;
  Array<bool> used(mr.edges_num, false);
  const OffsetIndices faces = mr.faces;
  const Span<int> corner_edges = mr.corner_edges;
  for (const int face_index : faces.index_range()) {
    const IndexRange face = faces[face_index];
    for (const int corner : face) {
      const int edge = corner_edges[corner];
      if (!used[edge] && edge_visible(edge) && freestyle_edges[edge]) {
        used[edge] = true;
        lines.append(edge_from_corners(face, corner));
      }
    }
  }

  const Span<int> loose_edges = mr.loose_edges;
  for (const int loose_edge_index : loose_edges.index_range()) {
    const int edge = loose_edges[loose_edge_index];
    if (!used[edge] && edge_visible(edge) && freestyle_edges[edge]) {
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
    if (!BM_elem_flag_test_bool(eed, BM_ELEM_HIDDEN) &&
        BM_ELEM_CD_GET_BOOL(eed, freestyle_edge_ofs))
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
  const int loose_edges_num = subdiv_loose_edges_num(mr, subdiv_cache);
  const int max_index = subdiv_full_vbo_size(mr, subdiv_cache);
  const int lines_num = subdiv_cache.num_subdiv_edges + loose_edges_num;

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_LINES, lines_num, max_index);
  MutableSpan<uint2> data = GPU_indexbuf_get_data(&builder).cast<uint2>();
  data.fill(uint2(gpu::RESTART_INDEX));

  auto edge_visible = [&](const int edge) {
    return mr.hide_edge.is_empty() || !mr.hide_edge[edge];
  };

  auto edge_marked = [&](const int edge) {
    if (edge < 0 || edge >= mr.edges_num || !edge_visible(edge)) {
      return false;
    }
    if (mr.extract_type == MeshExtractType::Mesh) {
      const bke::AttributeAccessor attributes = mr.mesh->attributes();
      const bke::AttributeReader<bool> freestyle_edge_attr = attributes.lookup<bool>(
          "freestyle_edge", bke::AttrDomain::Edge);
      if (!freestyle_edge_attr) {
        return false;
      }
      const VArraySpan<bool> freestyle_edges(freestyle_edge_attr.varray);
      return freestyle_edges[edge];
    }

    if (mr.freestyle_edge_ofs == -1) {
      return false;
    }
    BMesh *bm = mr.bm;
    const BMEdge *eed = BM_edge_at_index(bm, edge);
    return !BM_elem_flag_test_bool(eed, BM_ELEM_HIDDEN) &&
           BM_ELEM_CD_GET_BOOL(eed, mr.freestyle_edge_ofs);
  };

  const Span<int> subdiv_loop_edge_index = subdiv_cache.edges_orig_index->data<int>();
  const Span<int> subdiv_loop_subdiv_edge_index(subdiv_cache.subdiv_loop_subdiv_edge_index,
                                                subdiv_cache.num_subdiv_loops);
  const int quads_num = subdiv_cache.num_subdiv_quads;
  for (const int subdiv_quad_index : IndexRange(quads_num)) {
    const IndexRange subdiv_face(subdiv_quad_index * 4, 4);
    for (const int corner : subdiv_face) {
      const int coarse_edge_index = subdiv_loop_edge_index[corner];
      if (!edge_marked(coarse_edge_index)) {
        continue;
      }
      const int subdiv_edge_index = subdiv_loop_subdiv_edge_index[corner];
      data[subdiv_edge_index] = edge_from_corners(subdiv_face, corner);
    }
  }

  const int edges_per_edge = subdiv_edges_per_coarse_edge(subdiv_cache);
  const int loose_start = subdiv_cache.num_subdiv_loops;
  const Span<int> loose_edges = mr.loose_edges;
  for (const int loose_edge_index : loose_edges.index_range()) {
    const int edge = loose_edges[loose_edge_index];
    if (!edge_marked(edge)) {
      continue;
    }
    const int line_start = subdiv_cache.num_subdiv_edges + loose_edge_index * edges_per_edge;
    const int vertex_start = loose_start + loose_edge_index * edges_per_edge * 2;
    for (const int i : IndexRange(edges_per_edge)) {
      data[line_start + i] = uint2(vertex_start + i * 2, vertex_start + i * 2 + 1);
    }
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(&builder, 0, max_index, true));
}

}  // namespace blender::draw
