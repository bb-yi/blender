/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include <algorithm>

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"

#include "GPU_index_buffer.hh"

#include "draw_subdivision.hh"
#include "extract_mesh.hh"

namespace blender::draw {

static gpu::IndexBufPtr build_freestyle_lines_ibo(const Span<Vector<uint2>> lines_by_material,
                                                  const int max_index,
                                                  MutableSpan<gpu::IndexBufPtr> ibos_per_material)
{
  int lines_num = 0;
  for (const Vector<uint2> &lines : lines_by_material) {
    lines_num += lines.size();
  }
  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_LINES, lines_num, max_index);
  MutableSpan<uint2> data = GPU_indexbuf_get_data(&builder).cast<uint2>();
  int start = 0;
  for (const Vector<uint2> &lines : lines_by_material) {
    data.slice(start, lines.size()).copy_from(lines);
    start += lines.size();
  }
  gpu::IndexBufPtr ibo(GPU_indexbuf_build_ex(&builder, 0, max_index, false));
  start = 0;
  for (const int material : ibos_per_material.index_range()) {
    const int size = lines_by_material[material].size() * 2;
    ibos_per_material[material] = gpu::IndexBufPtr(
        GPU_indexbuf_create_subrange(ibo.get(), start, size));
    start += size;
  }
  return ibo;
}

static int face_material(const MeshRenderData &mr, const int face)
{
  const int material = mr.extract_type == MeshExtractType::BMesh ?
                           BM_face_at_index(mr.bm, face)->mat_nr :
                           (mr.material_indices.is_empty() ? 0 : mr.material_indices[face]);
  return std::clamp(material, 0, mr.materials_num - 1);
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

static void extract_freestyle_lines_mesh(const MeshRenderData &mr,
                                         MutableSpan<Vector<uint2>> lines_by_material)
{
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const bke::AttributeReader<bool> freestyle_edge_attr = attributes.lookup<bool>(
      "freestyle_edge", bke::AttrDomain::Edge);
  if (!freestyle_edge_attr) {
    return;
  }

  const VArraySpan<bool> freestyle_edges(freestyle_edge_attr.varray);

  Set<int2> used;
  const OffsetIndices faces = mr.faces;
  const Span<int> corner_edges = mr.corner_edges;
  for (const int face_index : faces.index_range()) {
    if (!mr.hide_poly.is_empty() && mr.hide_poly[face_index]) {
      continue;
    }
    const int material = face_material(mr, face_index);
    const IndexRange face = faces[face_index];
    for (const int corner : face) {
      const int edge = corner_edges[corner];
      if (mesh_edge_visible(mr, edge, true) && freestyle_edges[edge] &&
          used.add(int2(edge, material)))
      {
        lines_by_material[material].append(edge_from_corners(face, corner));
      }
    }
  }

  Array<int, 64> loose_edges(mr.loose_edges.size());
  mr.loose_edges.to_indices(loose_edges.as_mutable_span());
  for (const int loose_edge_index : loose_edges.index_range()) {
    const int edge = loose_edges[loose_edge_index];
    if (mesh_edge_visible(mr, edge, false) && freestyle_edges[edge]) {
      const uint corner_a = mr.corners_num + loose_edge_index * 2;
      const uint corner_b = mr.corners_num + loose_edge_index * 2 + 1;
      /* Loose edges have no face material. Match the surface fallback to the first slot. */
      lines_by_material[0].append(uint2(corner_a, corner_b));
    }
  }
}

static void extract_freestyle_lines_bm(const MeshRenderData &mr,
                                       MutableSpan<Vector<uint2>> lines_by_material)
{
  BMesh *bm = mr.bm;
  const int freestyle_edge_ofs = mr.freestyle_edge_ofs;
  if (freestyle_edge_ofs == -1) {
    return;
  }

  Array<int> loose_edge_map(bm->totedge, -1);
  for (const int loose_edge_index : mr.loose_edges.index_range()) {
    loose_edge_map[mr.loose_edges[loose_edge_index]] = loose_edge_index;
  }

  Set<int2> used;
  BMIter face_iter;
  BMFace *face;
  BM_ITER_MESH (face, &face_iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test_bool(face, BM_ELEM_HIDDEN)) {
      continue;
    }
    const int material = std::clamp(int(face->mat_nr), 0, mr.materials_num - 1);
    BMIter loop_iter;
    BMLoop *loop;
    BM_ITER_ELEM (loop, &loop_iter, face, BM_LOOPS_OF_FACE) {
      if (bmesh_edge_visible(*loop->e) && BM_ELEM_CD_GET_BOOL(loop->e, freestyle_edge_ofs) &&
          used.add(int2(BM_elem_index_get(loop->e), material)))
      {
        lines_by_material[material].append(
            uint2(BM_elem_index_get(loop), BM_elem_index_get(loop->next)));
      }
    }
  }

  BMIter iter;
  BMEdge *eed;
  BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
    if (!eed->l && bmesh_edge_visible(*eed) && BM_ELEM_CD_GET_BOOL(eed, freestyle_edge_ofs)) {
      const int edge = BM_elem_index_get(eed);
      const int loose_edge_index = loose_edge_map[edge];
      if (loose_edge_index != -1) {
        const uint corner_a = mr.corners_num + loose_edge_index * 2;
        const uint corner_b = mr.corners_num + loose_edge_index * 2 + 1;
        lines_by_material[0].append(uint2(corner_a, corner_b));
      }
    }
  }
}

gpu::IndexBufPtr extract_freestyle_lines(const MeshRenderData &mr,
                                         MutableSpan<gpu::IndexBufPtr> ibos_per_material)
{
  Array<Vector<uint2>> lines_by_material(mr.materials_num);
  if (mr.extract_type == MeshExtractType::Mesh) {
    extract_freestyle_lines_mesh(mr, lines_by_material);
  }
  else {
    extract_freestyle_lines_bm(mr, lines_by_material);
  }
  return build_freestyle_lines_ibo(
      lines_by_material, mr.corners_num + mr.loose_edges.size() * 2, ibos_per_material);
}

gpu::IndexBufPtr extract_freestyle_lines_subdiv(const DRWSubdivCache &subdiv_cache,
                                                const MeshRenderData &mr,
                                                MutableSpan<gpu::IndexBufPtr> ibos_per_material)
{
  const int max_index = subdiv_full_vbo_size(mr, subdiv_cache);
  Array<Vector<uint2>> lines_by_material(mr.materials_num);

  Array<bool> marked_edges(mr.edges_num, false);
  bool has_marked_edge = false;
  if (mr.extract_type == MeshExtractType::Mesh) {
    const bke::AttributeAccessor attributes = mr.mesh->attributes();
    const bke::AttributeReader<bool> freestyle_edge_attr = attributes.lookup<bool>(
        "freestyle_edge", bke::AttrDomain::Edge);
    if (!freestyle_edge_attr) {
      return build_freestyle_lines_ibo(lines_by_material, max_index, ibos_per_material);
    }
    const VArraySpan<bool> freestyle_edges(freestyle_edge_attr.varray);
    for (const int edge : IndexRange(mr.edges_num)) {
      marked_edges[edge] = mesh_edge_visible(mr, edge, false) && freestyle_edges[edge];
      has_marked_edge = has_marked_edge || marked_edges[edge];
    }
  }
  else {
    if (mr.freestyle_edge_ofs == -1) {
      return build_freestyle_lines_ibo(lines_by_material, max_index, ibos_per_material);
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
    return build_freestyle_lines_ibo(lines_by_material, max_index, ibos_per_material);
  }

  const Span<int> subdiv_loop_edge_index = subdiv_cache.edges_orig_index->data<int>();
  const Span<int> subdiv_loop_edge_draw_flag = subdiv_cache.edges_draw_flag->data<int>();
  const Span<int> subdiv_loop_subdiv_edge_index(subdiv_cache.subdiv_loop_subdiv_edge_index,
                                                subdiv_cache.num_subdiv_loops);
  Set<int2> used_subdiv_edges;

  const int quads_num = subdiv_cache.num_subdiv_quads;
  for (const int subdiv_quad_index : IndexRange(quads_num)) {
    const int coarse_face = subdiv_cache.subdiv_loop_face_index[subdiv_quad_index * 4];
    const int material = face_material(mr, coarse_face);
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
      if (used_subdiv_edges.add(int2(subdiv_edge_index, material))) {
        lines_by_material[material].append(edge_from_corners(subdiv_face, corner));
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
      lines_by_material[0].append(uint2(vertex_start + i * 2, vertex_start + i * 2 + 1));
    }
  }

  return build_freestyle_lines_ibo(lines_by_material, max_index, ibos_per_material);
}

}  // namespace blender::draw
