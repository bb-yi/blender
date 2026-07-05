/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "infos/eevee_geom_infos.hh"

#include "draw_model.bsl.hh"
#include "draw_view.bsl.hh"
#include "eevee_lightprobe_shared.hh" /* IWYU pragma: export: Needed for resource declaration. */
#include "eevee_pipeline.bsl.hh"
#include "eevee_sampling_shared.hh"   /* IWYU pragma: export: Needed for resource declaration. */
#include "eevee_shadow_shared.hh"
#include "eevee_uniform.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_vector_safe_lib.glsl"
#include "eevee_reverse_z_lib.bsl.hh"

#define MAT_SURFACE_CULL_NONE 0
#define MAT_SURFACE_CULL_BACK 1
#define MAT_SURFACE_CULL_FRONT 2

void material_surface_cull_discard()
{
#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_SURFACE_CULL)
  if ((surface_cull_mode == MAT_SURFACE_CULL_BACK && !gl_FrontFacing) ||
      (surface_cull_mode == MAT_SURFACE_CULL_FRONT && gl_FrontFacing))
  {
    gpu_discard_fragment();
  }
#endif
}

namespace eevee {

struct GeomShadow {
  [[storage(SHADOW_RENDER_VIEW_BUF_SLOT,
            read)]] const ShadowRenderView (&render_view_buf)[SHADOW_VIEW_MAX];
};

}  // namespace eevee

ViewMatrices surface_view_matrices_get()
{
  [[resource_table]] const eevee::PipelineConstants &pipe = resource_table_get(
      eevee::PipelineConstants);
  [[resource_table]] const draw::View &views = resource_table_get(draw::View);
  auto &interp_flat = interface_get(eevee_geom_iface_info, interp_flat);
  draw::ID id{interp_flat.resource_id_raw};
  if (pipe.is_shadow_pipe) {
    return views.get(id.view_id<64>());
  }
  return views.get(id.view_id<1>());
}

#if defined(USE_BARYCENTRICS) && defined(GPU_FRAGMENT_SHADER) && defined(MAT_GEOM_MESH)
float3 barycentric_distances_get()
{
  float wp_delta = length(gpu_dfdx(interp.P)) + length(gpu_dfdy(interp.P));
  float bc_delta = length(gpu_dfdx(gpu_BaryCoord)) + length(gpu_dfdy(gpu_BaryCoord));
  float rate_of_change = wp_delta / bc_delta;
  return rate_of_change * (1.0f - gpu_BaryCoord);
}
#endif

void init_globals_mesh()
{
#if defined(USE_BARYCENTRICS) && defined(GPU_FRAGMENT_SHADER) && defined(MAT_GEOM_MESH)
  g_data.barycentric_coords = gpu_BaryCoord.xy;
  g_data.barycentric_dists = barycentric_distances_get();
#endif
}

void init_globals_curves(const ViewMatrices view)
{
  auto &interp = interface_get(eevee_geom_iface_info, interp);
  auto &curve_interp = interface_get(eevee_geom_curves_iface_info, curve_interp);
  auto &curve_interp_flat = interface_get(eevee_geom_curves_iface_info, curve_interp_flat);
  /* Shade as a cylinder. */
  float cos_theta = curve_interp.time_width / curve_interp.radius;
  float sin_theta = sin_from_cos(cos_theta);
  g_data.N = g_data.Ni = normalize(interp.N * sin_theta + curve_interp.binormal * cos_theta);

  /* Costly, but follows cycles per pixel tangent space (not following curve shape). */
  float3 V = view.world_incident_vector(g_data.P);
  g_data.curve_T = -curve_interp.tangent;
  g_data.curve_B = cross(V, g_data.curve_T);
  g_data.curve_N = safe_normalize(cross(g_data.curve_T, g_data.curve_B));

  g_data.is_strand = true;
  g_data.hair_diameter = curve_interp.radius * 2.0;
  g_data.hair_strand_id = curve_interp_flat.strand_id;
#if defined(USE_BARYCENTRICS) && defined(GPU_FRAGMENT_SHADER)
  g_data.barycentric_coords.y = fract(curve_interp.point_id);
  g_data.barycentric_coords.x = 1.0 - g_data.barycentric_coords.y;
#endif
}

void init_globals([[resource_table]] const eevee::Uniform &uni,
                  const ViewMatrices view,
                  bool front_face)
{
  auto &interp = interface_get(eevee_geom_iface_info, interp);
  /* Default values. */
  g_data.P = interp.P;
  g_data.Ni = interp.N;
  g_data.N = safe_normalize(interp.N);
  g_data.Ng = g_data.N;
  g_data.is_strand = false;
  g_data.hair_diameter = 0.0f;
  g_data.hair_strand_id = 0;
#if defined(MAT_SHADOW)
  g_data.ray_type = RAY_TYPE_SHADOW;
#elif defined(MAT_CAPTURE)
  g_data.ray_type = RAY_TYPE_DIFFUSE;
#else
  g_data.ray_type = uni.pipeline_buf.ray_type;
#endif
  g_data.ray_depth = 0.0f;
  g_data.ray_length = distance(g_data.P, view.position());
  g_data.barycentric_coords = float2(0.0f);
  g_data.barycentric_dists = float3(0.0f);

  g_data.N = (front_face) ? g_data.N : -g_data.N;
  g_data.Ni = (front_face) ? g_data.Ni : -g_data.Ni;
#ifdef GPU_FRAGMENT_SHADER
  g_data.Ng = safe_normalize(cross(gpu_dfdx(g_data.P), gpu_dfdy(g_data.P)));
  if (uni.pipeline_buf.is_main_view_inverted) {
    g_data.Ng = -g_data.Ng;
  }
#endif

#if defined(MAT_GEOM_MESH)
  init_globals_mesh();
#elif defined(MAT_GEOM_CURVES)
  init_globals_curves(view);
#endif
}

/* Avoid some compiler issue with non set interface parameters. */
void init_interface([[maybe_unused]] uint resource_id_raw)
{
#ifdef GPU_VERTEX_SHADER
  auto &interp = interface_get(eevee_geom_iface_info, interp);
  auto &interp_flat = interface_get(eevee_geom_iface_info, interp_flat);
  interp.P = float3(0.0f);
  interp.N = float3(0.0f);
  interp_flat.resource_id_raw = resource_id_raw;
#endif
}

#if defined(GPU_VERTEX_SHADER) && defined(DRAW_MODELMAT_CREATE_INFO) && \
    defined(DRW_MODEL_LIB_FUNCTIONS_DEFINED)
void init_interface()
{
  init_interface(drw_resource_id_raw());
}
#endif

#if defined(GPU_FRAGMENT_SHADER) || defined(GPU_VERTEX_SHADER)
void init_globals()
{
  [[resource_table]] const eevee::Uniform &uni = resource_table_get(eevee::Uniform);
  const ViewMatrices view = surface_view_matrices_get();
#  if defined(GPU_FRAGMENT_SHADER)
  init_globals(uni, view, gl_FrontFacing);
#  else
  init_globals(uni, view, true);
#  endif
}
#endif

#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_DEPTH_OFFSET)
bool material_depth_offset_is_zero(float depth_offset)
{
  return abs(depth_offset) <= 1.0e-8f;
}

float material_depth_offset_view_z(float depth_offset)
{
  const ViewMatrices view = surface_view_matrices_get();
  float vP_z = view.depth_screen_to_view(reverse_z::read(gl_FragCoord.z));
  return vP_z + depth_offset;
}

bool material_depth_offset_is_clipped(float depth_offset)
{
  if (material_depth_offset_is_zero(depth_offset)) {
    return false;
  }
  const ViewMatrices view = surface_view_matrices_get();
  float vP_z = material_depth_offset_view_z(depth_offset);
  return vP_z > view.near() || vP_z < view.far();
}

float material_depth_offset_frag_depth(float depth_offset)
{
  if (material_depth_offset_is_zero(depth_offset)) {
    return gl_FragCoord.z;
  }
  const ViewMatrices view = surface_view_matrices_get();
  float vP_z = material_depth_offset_view_z(depth_offset);
  return saturate(reverse_z::read(view.depth_view_to_screen(vP_z)));
}

float material_depth_offset_screen_depth(float depth_offset)
{
  return reverse_z::read(material_depth_offset_frag_depth(depth_offset));
}

float3 material_depth_offset_world_position_from_depth(float screen_depth)
{
  [[resource_table]] const eevee::Uniform &uni = resource_table_get(eevee::Uniform);
  const ViewMatrices view = surface_view_matrices_get();
  float2 screen_uv = gl_FragCoord.xy * uni.uniform_buf.volumes.main_view_extent_inv;
  return view.point_screen_to_world(float3(screen_uv, screen_depth));
}

float3 material_depth_offset_world_position(float depth_offset)
{
  return material_depth_offset_world_position_from_depth(
      material_depth_offset_screen_depth(depth_offset));
}

float3 material_depth_offset_apply_nodetree_position(float depth_offset)
{
  if (!material_depth_offset_is_zero(depth_offset)) {
    g_data.P = material_depth_offset_world_position(depth_offset);
    g_data.ray_length = distance(g_data.P, surface_view_matrices_get().position());
  }
  return g_data.P;
}

float material_depth_offset_frag_depth()
{
  return material_depth_offset_frag_depth(nodetree_depth_offset());
}

#  if !defined(MAT_SHADOW) || defined(SHADOW_UPDATE_TBDR)
void material_depth_offset_write(float depth_offset)
{
  if (material_depth_offset_is_clipped(depth_offset)) {
    gpu_discard_fragment();
    return;
  }
  gl_FragDepth = material_depth_offset_frag_depth(depth_offset);
}

void material_depth_offset_write()
{
  material_depth_offset_write(nodetree_depth_offset());
}
#  endif
#endif

float3 shadow_position_vector_get(float3 view_position, ShadowRenderView view)
{
  if (view.is_directional) {
    return float3(0.0f, 0.0f, -view_position.z - view.clip_near);
  }
  return view_position;
}

/* In order to support physical clipping, we pass a vector to the fragment shader that then clips
 * each fragment using a unit sphere test. This allows to support both point light and area light
 * clipping at the same time. */
float3 shadow_clip_vector_get(float3 view_position, float clip_distance_inv)
{
  if (clip_distance_inv == 0.0f) {
    /* No clipping. */
    return float3(2.0f);
  }
  /* Punctual shadow case. */
  return view_position * clip_distance_inv;
}
