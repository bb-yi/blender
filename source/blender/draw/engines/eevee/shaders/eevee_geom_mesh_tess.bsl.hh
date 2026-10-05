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
#include "infos/eevee_nodetree_infos.hh"

VERTEX_SHADER_CREATE_INFO(eevee_nodetree)
VERTEX_SHADER_CREATE_INFO(eevee_clip_plane)
VERTEX_SHADER_CREATE_INFO(eevee_geom_mesh_tess_data)

#include "draw_model.bsl.hh"
#include "eevee_attributes_mesh_lib.glsl"
#include "eevee_geom_mesh_tess_lib.glsl"
#include "eevee_nodetree_vert_lib.glsl"
#include "eevee_pipeline.bsl.hh"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"
#include "eevee_velocity.bsl.hh"

namespace eevee {

struct GeomMeshTess {
  [[legacy_info]] ShaderCreateInfo eevee_geom_iface_info;
  [[legacy_info]] ShaderCreateInfo eevee_geom_mesh_tess_data;
  /* WORKAROUND: Until we get condition support for interfaces. */
  [[legacy_info]] ShaderCreateInfo eevee_geom_mesh_tess_iface_info;
};

[[vertex]] [[clip_control]] void geom_mesh_tess(
    [[resource_table]] const PipelineConstants &pipe,
    [[resource_table]] const GeomMeshTess & /*srt*/,
    [[resource_table]] const Uniform &uni,
    [[instance_index]] const int inst_index,
    [[resource_table]] const draw::View &views,
    [[resource_table]] const draw::Model &models,
    [[resource_table]] const draw::Infos &infos,
    [[resource_table]] const draw::Resource &res_id,
    [[resource_table, condition(is_shadow_pipe)]] const GeomShadow &shadow,
    [[instance_id]] const int /*inst_id*/,     /* Used by model_lib. */
    [[base_instance]] const int /*base_inst*/, /* Used by model_lib. */
    [[vertex_id]] const int vert_id,
    [[position]] float4 &out_position,
    /* Note: Removed manually if not needed. Otherwise, can generate geometry shader fallback. */
    [[viewport_index]] int &out_viewport)
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

  auto &interp = interface_get(eevee_geom_iface_info, interp);

  if (pipe.is_shadow_pipe) [[static_branch]] {
    auto &shadow_iface = interface_get(eevee_shadow_iface_info, shadow_iface);

    shadow_iface.shadow_view_id = int(view_id);
    out_viewport = int(shadow.render_view_buf[view_id].viewport_index);
  }

  init_interface(id.raw_id);

  /* The subdivision levels only depend on the main camera, so that every view (including shadows
   * and light-probes) renders the same surface. */
  const TessSettings settings = tess_settings_unpack(nodetree_tessellation());
  const CameraData camera = uni.uniform_buf.camera;
  const TessVertex tv = tess_vertex_get(settings,
                                        vert_id,
                                        obj.model,
                                        camera.viewinv[3].xyz,
                                        camera.persmat,
                                        float2(uni.uniform_buf.film.render_extent));
  if (tv.is_degenerate) {
    /* All the vertices of the sub-triangle are at the same position. Nothing is rasterized. */
    out_position = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  ObjectInfos ob_infos = infos.get(resource_id);

  float3 lP = tess_phong(tv, tv.lP, tv.weights, settings.phong);
  interp.P = obj.point_object_to_world(lP);
  interp.N = safe_normalize(obj.normal_object_to_world(tess_normal_interpolate(tv, tv.weights)));
  /* Used by #TESS_NORMAL_GEOMETRY to evaluate the displacement in the fragment shader. */
  auto &tess_interp = interface_get(eevee_geom_mesh_tess_iface_info, tess_interp);
  tess_interp.P_base = interp.P;

  /* Compute Original Coordinate (ORCO). */
  float3 lP_orco = lP * ob_infos.orco_mul + ob_infos.orco_add;

  init_globals(uni, view, true);
  tess_attr_interp(tv.indices, tv.weights);
  attrib_load(MeshVertex{lP, to_float3x3(obj.model_inverse), lP_orco});

  interp.P += nodetree_displacement();

  if (pipe.use_velocity) [[static_branch]] {
    /* clang-format off */ /* Multi-line define messes up line index. */
    [[resource_table]] const GeometryVelocity &geo_vel = resource_table_get(eevee::GeometryVelocity);
    /* clang-format on */
    auto &motion = interface_get(eevee_velocity_iface_info, motion);
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
    auto &clip_interp = interface_get(eevee_clip_plane, clip_interp);
    const auto &clip_plane = buffer_get(eevee_clip_plane, clip_plane);
    clip_interp.clip_distance = dot(clip_plane.plane, float4(interp.P, 1.0f));
  }

  float3 vs_P = view.point_world_to_view(interp.P);

  if (pipe.is_shadow_pipe) [[static_branch]] {
    auto &shadow_clip = interface_get(eevee_shadow_iface_info, shadow_clip);
    ShadowRenderView view = shadow.render_view_buf[view_id];
    shadow_clip.position = shadow_position_vector_get(vs_P, view);
    shadow_clip.vector = shadow_clip_vector_get(vs_P, view.clip_distance_inv);
  }

  out_position = reverse_z::transform(view.point_view_to_homogenous(vs_P));
}

}  // namespace eevee
