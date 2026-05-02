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

}  // namespace blender::draw
