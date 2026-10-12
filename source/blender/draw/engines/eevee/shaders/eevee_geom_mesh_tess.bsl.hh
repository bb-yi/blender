/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Vertex shader of tessellated meshes (#MAT_GEOM_MESH_TESS). See `eevee_geom_mesh_tess_lib.glsl`.
 * Must be drawn using primitive expansion of `max_level^2` triangles per mesh triangle.
 * Keep the outputs in sync with `eevee::geom_mesh`.
 */

#pragma once

#include "infos/eevee_geom_infos.hh"


#include "draw_model.bsl.hh"
#include "eevee_attributes_mesh_lib.bsl.hh"
#include "eevee_geom_mesh_tess_lib.glsl"
#include "eevee_nodetree_vert_lib.bsl.hh"
#include "eevee_pipeline.bsl.hh"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"
#include "eevee_velocity.bsl.hh"
#include "gpu_shader_math_matrix_transform.bsl.hh"

namespace eevee {

struct GeomMeshTess {
  [[legacy_info]] ShaderCreateInfo eevee_geom_mesh_tess_data;
};

[[vertex]] [[clip_control]] void geom_mesh_tess(
    [[resource_table]] KernelGlobals &kg,
    [[resource_table]] const PipelineConstants &pipe,
    [[resource_table]] const GeomMeshTess & /*srt*/,
    [[resource_table]] const Uniform &uni,
    [[instance_index]] const int inst_index,
    [[resource_table]] const draw::View &views,
    [[resource_table]] const draw::Model &models,
    [[resource_table]] const draw::Infos &infos,
    [[resource_table]] const draw::Resource &res_id,
    [[resource_table, condition(is_shadow_pipe)]] const GeomShadow &shadow,
    [[resource_table, condition(use_clip_plane)]] const ClipPlane &clip_srt,
    [[resource_table, condition(use_velocity)]] const GeometryVelocity &geo_vel,
    [[out]] VertOutCommon &interp,
    [[out, condition(is_shadow_pipe)]] VertOutShadow &shadow_iface,
    [[out, condition(is_shadow_pipe)]] VertOutShadowClipping &shadow_clip,
    [[out, condition(use_clip_plane)]] VertOutClipPlane &clip_interp,
    [[out, condition(use_velocity)]] VertOutVelocity &motion,
    [[vertex_id]] const int vert_id,
    [[position]] float4 &out_position,
    [[viewport_index, condition(is_shadow_pipe &&use_multi_viewport)]] int &out_viewport)
{
  draw::ID id = res_id.get(inst_index);
  uint view_id = 0;
  uint resource_id = id.resource_id<1>();
  if (pipe.is_shadow_pipe) [[static_branch]] {
    view_id = id.view_id<64>();
    resource_id = id.resource_id<64>();
  }

  const ViewMatrices view = views.get(view_id);
  const ObjectMatrices obj = models.get(resource_id);

  if (pipe.is_shadow_pipe) [[static_branch]] {
    shadow_iface.shadow_view_id = int(view_id);
    if (pipe.use_multi_viewport) [[static_branch]] {
      out_viewport = int(shadow.render_view_buf[view_id].viewport_index);
    }
  }

  init_interface(interp, id.raw_id);
  ShadingData sd = init_globals(pipe, uni, interp, view, true, float4(0.0f));

  /* The subdivision levels only depend on the main camera, so that every view (including shadows
   * and light-probes) renders the same surface. The culling uses the view being rendered. */
  const TessSettings settings = tess_settings_unpack(nodetree_tessellation(kg, sd));
  const CameraData camera = uni.uniform_buf.camera;
  /* BSL currently accepts matrix-vector products but not matrix-matrix products. */
  const float4x4 view_projection = float4x4(view.winmat * view.viewmat[0],
                                          view.winmat * view.viewmat[1],
                                          view.winmat * view.viewmat[2],
                                          view.winmat * view.viewmat[3]);
  const TessVertex tv = tess_vertex_get(settings,
                                        vert_id,
                                        obj.model,
                                        camera.viewinv[3].xyz,
                                        camera.persmat,
                                        float2(uni.uniform_buf.film.render_extent),
                                        view_projection,
                                        view.position(),
                                        view.forward(),
                                        !view.is_perspective(),
                                        pipe.is_shadow_pipe);
  if (tv.is_degenerate) {
    /* All the vertices of the sub-triangle are at the same position. Nothing is rasterized. */
    out_position = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  ObjectInfos ob_infos = infos.get(resource_id);

  float3 lP = tess_phong(tv, tv.lP, tv.weights, settings.phong);
  interp.P = obj.point_object_to_world(lP);
  interp.N = safe_normalize(obj.normal_object_to_world(tess_normal_interpolate(tv, tv.weights)));
  /* Used by #MAT_DISPLACEMENT_GEOM_NORMAL to evaluate the displacement in the fragment shader. */
  interp.P_base = interp.P;

  /* Compute Original Coordinate (ORCO). */
  float3 lP_orco = lP * ob_infos.orco_mul + ob_infos.orco_add;

  sd = init_globals(pipe, uni, interp, view, true, float4(0.0f));
  init_globals_mesh(interp, sd, float3(0.0f));
  tess_attr_interp(tv.indices, tv.weights);
  attrib_load(MeshVertex{lP, to_float3x3(obj.model_inverse), lP_orco});

  interp.P += nodetree_displacement(kg, sd);

  if (pipe.use_velocity) [[static_branch]] {
    float3 prv0, nxt0, prv1, nxt1, prv2, nxt2;
    geo_vel.local_position_deltas(tv.P0, int(tv.indices.x), prv0, nxt0, resource_id);
    geo_vel.local_position_deltas(tv.P1, int(tv.indices.y), prv1, nxt1, resource_id);
    geo_vel.local_position_deltas(tv.P2, int(tv.indices.z), prv2, nxt2, resource_id);
    /* Like `eevee::geom_mesh`, evaluated before displacement. The smoothing is considered rigid. */
    float3 smoothing_offset = lP - tv.lP;
    float3 prv = tv.weights.x * prv0 + tv.weights.y * prv1 + tv.weights.z * prv2 +
                 smoothing_offset;
    float3 nxt = tv.weights.x * nxt0 + tv.weights.y * nxt1 + tv.weights.z * nxt2 +
                 smoothing_offset;
    geo_vel.vertex_velocity(prv, lP, nxt, motion.prev, motion.next, resource_id, obj.model);
  }

  if (pipe.use_clip_plane) [[static_branch]] {
    clip_interp.clip_distance = dot(clip_srt.clip_plane.plane, float4(interp.P, 1.0f));
  }

  float3 vs_P = view.point_world_to_view(interp.P);

  if (pipe.is_shadow_pipe) [[static_branch]] {
    ShadowRenderView view = shadow.render_view_buf[view_id];
    shadow_clip.position = shadow_position_vector_get(vs_P, view);
    shadow_clip.vector = shadow_clip_vector_get(vs_P, view.clip_distance_inv);
  }

  out_position = reverse_z::transform(view.point_view_to_homogenous(vs_P));
}

}  // namespace eevee
