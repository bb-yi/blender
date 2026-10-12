/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_gsplat_lib.bsl.hh"
#include "eevee_nodetree_lib.bsl.hh"
#include "eevee_shadow_shared.hh"
#include "eevee_uniform.bsl.hh"
#include "gpu_shader_math_base.bsl.hh"
#include "gpu_shader_math_vector_safe.bsl.hh"

#include "eevee_reverse_z_lib.bsl.hh"

#define MAT_SURFACE_CULL_NONE 0
#define MAT_SURFACE_CULL_BACK 1
#define MAT_SURFACE_CULL_FRONT 2

struct MaterialSurfaceCull {
  [[push_constant]] int surface_cull_mode;
};

void material_surface_cull_discard(bool front_face)
{
#if defined(GPU_FRAGMENT_SHADER)
  [[resource_table]] const MaterialSurfaceCull &srt = resource_table_get(MaterialSurfaceCull);
  if ((srt.surface_cull_mode == MAT_SURFACE_CULL_BACK && !front_face) ||
      (srt.surface_cull_mode == MAT_SURFACE_CULL_FRONT && front_face))
  {
    gpu_discard_fragment();
  }
#endif
}

namespace eevee {

/* Common interface */
struct VertOutCommon {
  /* World Position. */
  [[smooth]] float3 P;
  [[smooth]] float3 P_base;
  /* World Normal. */
  [[smooth]] float3 N;
  /* Resource ID. */
  [[flat]] uint resource_id_raw;
};

struct VertOutShadow {
  [[flat]] int shadow_view_id;
};

struct VertOutBake {
  [[flat]] float3 Ng;
  [[flat]] int primitive_id;
};

struct VertOutShadowClipping {
  [[smooth]] float3 position;
  [[smooth]] float3 vector;
};

struct VertOutCurves {
  [[smooth]] float3 tangent;
  [[smooth]] float3 binormal;
  [[smooth]] float time;
  [[smooth]] float time_width;
  [[smooth]] float radius;
  [[smooth]] float point_id; /* Smooth to be used for barycentric. */
  [[flat]] int strand_id;
};

struct VertOutPointcloud {
  [[smooth]] float radius;
  [[smooth]] float3 position;
  [[flat]] int id;
};

struct VertOutGSplat {
  [[smooth]] float2 billboard_co;
  [[flat]] float opacity;
};

struct VertOutClipPlane {
  [[smooth]] float clip_distance;
};

struct ClipPlane {
  [[uniform(CLIP_PLANE_BUF)]] ClipPlaneData &clip_plane;
};

struct GeomShadow {
  [[storage(SHADOW_RENDER_VIEW_BUF_SLOT,
            read)]] const ShadowRenderView (&render_view_buf)[SHADOW_VIEW_MAX];
};

void init_globals_mesh(const VertOutCommon &interp, ShadingData &sd, float3 barycentric_co)
{
  float wp_delta = 0.0f;
  float bc_delta = 0.0f;
#if defined(GPU_FRAGMENT_SHADER) || defined(GLSL_CPP_STUBS)
  wp_delta = length(gpu_dfdx(interp.P)) + length(gpu_dfdy(interp.P));
  bc_delta = length(gpu_dfdx(barycentric_co)) + length(gpu_dfdy(barycentric_co));
#endif
  float rate_of_change = wp_delta * safe_rcp(bc_delta);
  sd.barycentric_dists = rate_of_change * (1.0f - barycentric_co);
  sd.barycentric_coords = barycentric_co.xy;
}

void init_globals_curves(const VertOutCommon &interp,
                         const VertOutCurves &curve_interp,
                         ShadingData &sd,
                         const ViewMatrices view)
{
  /* Shade as a cylinder. */
  float cos_theta = curve_interp.time_width / curve_interp.radius;
  float sin_theta = sin_from_cos(cos_theta);
  sd.N = sd.Ni = normalize(interp.N * sin_theta + curve_interp.binormal * cos_theta);

  /* Costly, but follows cycles per pixel tangent space (not following curve shape). */
  float3 V = view.world_incident_vector(sd.P);
  sd.curve_T = -curve_interp.tangent;
  sd.curve_B = cross(V, sd.curve_T);
  sd.curve_N = safe_normalize(cross(sd.curve_T, sd.curve_B));

  sd.is_strand = true;
  sd.hair_diameter = curve_interp.radius * 2.0;
  sd.hair_strand_id = curve_interp.strand_id;
  sd.barycentric_coords.y = fract(curve_interp.point_id);
  sd.barycentric_coords.x = 1.0 - sd.barycentric_coords.y;
}

void init_globals_pointcloud(const VertOutPointcloud &ptcloud_interp, ShadingData &sd)
{
  sd.point_position = ptcloud_interp.position;
  sd.point_radius = ptcloud_interp.radius;
  sd.point_id = ptcloud_interp.id;
}

[[nodiscard]] ShadingData init_globals(const eevee::PipelineConstants &pipe,
                                       const eevee::Uniform &uni,
                                       const VertOutCommon &interp,
                                       const ViewMatrices view,
                                       bool front_face,
                                       float4 fragment_co)
{
  ShadingData sd = {};
  sd.npr_diffusion_weight = 1.0f;
  sd.displacement_base = interp.P_base;
  /* Default values. */
  sd.frag_co = fragment_co;
  sd.front_facing = front_face;
  sd.P = interp.P;
  sd.forward_lighting_P = sd.P;
  sd.Ni = interp.N;
  sd.N = safe_normalize(interp.N);
  sd.Ng = sd.N;
  sd.is_strand = false;
  sd.hair_diameter = 0.0f;
  sd.hair_strand_id = 0;
  sd.point_position = float3(0.0f);
  sd.point_radius = 0.0f;
  sd.point_id = 0;
  if (pipe.is_shadow_pipe) [[static_branch]] {
    sd.ray_type = RAY_TYPE_SHADOW;
  }
  else if (pipe.is_capture_pipe) [[static_branch]] {
    sd.ray_type = RAY_TYPE_DIFFUSE;
  }
  else {
    sd.ray_type = uni.pipeline_buf.ray_type;
  }
  sd.ray_depth = 0.0f;
  sd.ray_length = distance(sd.P, view.position());
  sd.barycentric_coords = float2(0.0f);
  sd.barycentric_dists = float3(0.0f);
  sd.thickness = Thickness::zero();
  sd.resource_id_raw = interp.resource_id_raw;

  sd.N = (front_face) ? sd.N : -sd.N;
  sd.Ni = (front_face) ? sd.Ni : -sd.Ni;
#if defined(GPU_FRAGMENT_SHADER) || defined(GLSL_CPP_STUBS)
  sd.Ng = safe_normalize(cross(gpu_dfdx(sd.P), gpu_dfdy(sd.P)));
  if (uni.pipeline_buf.is_main_view_inverted) {
    sd.Ng = -sd.Ng;
  }
#endif
  return sd;
}

/* Avoid some compiler issue with non set interface parameters. */
void init_interface(VertOutCommon &interp, uint resource_id_raw)
{
  interp.P = float3(0.0f);
  interp.P_base = float3(0.0f);
  interp.N = float3(0.0f);
  interp.resource_id_raw = resource_id_raw;
}

float3 gsplat_amend_transmittance([[maybe_unused]] const VertOutGSplat &gsplat_interp,
                                  float3 transmittance)
{
  float alpha = draw::gsplat::evaluate_gaussian(gsplat_interp.billboard_co, gsplat_interp.opacity);
  alpha = saturate(alpha * (256.0f / 255.0f));
  return float3(1.0f) - alpha * saturate(float3(1.0f) - transmittance);
}

}  // namespace eevee

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

#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_DEPTH_OFFSET)
bool material_depth_offset_is_zero(KernelGlobals &kg, ShadingData &sd, float depth_offset)
{
  return abs(depth_offset) <= 1.0e-8f;
}

float material_depth_offset_view_z(KernelGlobals &kg, ShadingData &sd, float depth_offset)
{
  const ViewMatrices view = kg.view_matrices_get(sd);
  float vP_z = view.depth_screen_to_view(reverse_z::read(sd.frag_co.z));
  return vP_z + depth_offset;
}

bool material_depth_offset_is_clipped(KernelGlobals &kg, ShadingData &sd, float depth_offset)
{
  if (material_depth_offset_is_zero(kg, sd, depth_offset)) {
    return false;
  }
  const ViewMatrices view = kg.view_matrices_get(sd);
  float vP_z = material_depth_offset_view_z(kg, sd, depth_offset);
  return vP_z > view.near() || vP_z < view.far();
}

float material_depth_offset_frag_depth(KernelGlobals &kg, ShadingData &sd, float depth_offset)
{
  if (material_depth_offset_is_zero(kg, sd, depth_offset)) {
    return sd.frag_co.z;
  }
  const ViewMatrices view = kg.view_matrices_get(sd);
  float vP_z = material_depth_offset_view_z(kg, sd, depth_offset);
  return saturate(reverse_z::read(view.depth_view_to_screen(vP_z)));
}

float material_depth_offset_screen_depth(KernelGlobals &kg, ShadingData &sd, float depth_offset)
{
  return reverse_z::read(material_depth_offset_frag_depth(kg, sd, depth_offset));
}

#  if defined(MAT_DEFERRED)
bool material_depth_offset_fragment_matches_prepass(KernelGlobals &kg, ShadingData &sd, float depth_offset)
{
  float fragment_depth = reverse_z::read(material_depth_offset_frag_depth(kg, sd, depth_offset));
  if (kg.pipe.use_ambient_occlusion || kg.pipe.use_hiz_data) [[static_branch]] {
    float prepass_depth = texelFetch(kg.hiz.hiz_tx, int2(sd.frag_co.xy), 0).r;
    return abs(fragment_depth - prepass_depth) <= 1.0e-6f;
  }
  return true;
}
#  endif

float3 material_depth_offset_world_position_from_depth(KernelGlobals &kg, ShadingData &sd, float screen_depth)
{
  const eevee::Uniform &uni = kg.uniforms;
  const ViewMatrices view = kg.view_matrices_get(sd);
  float2 screen_uv = sd.frag_co.xy * uni.uniform_buf.volumes.main_view_extent_inv;
  return view.point_screen_to_world(float3(screen_uv, screen_depth));
}

float3 material_depth_offset_world_position(KernelGlobals &kg, ShadingData &sd, float depth_offset)
{
  return material_depth_offset_world_position_from_depth(kg, sd,
      material_depth_offset_screen_depth(kg, sd, depth_offset));
}

float3 material_depth_offset_apply_nodetree_position(KernelGlobals &kg, ShadingData &sd, float depth_offset)
{
  if (!material_depth_offset_is_zero(kg, sd, depth_offset)) {
    sd.P = material_depth_offset_world_position(kg, sd, depth_offset);
    sd.ray_length = distance(sd.P, kg.view_matrices_get(sd).position());
  }
  return sd.P;
}

float material_depth_offset_frag_depth(KernelGlobals &kg, ShadingData &sd)
{
  return material_depth_offset_frag_depth(kg, sd, nodetree_depth_offset(kg, sd));
}

#  if !defined(MAT_SHADOW) || defined(SHADOW_UPDATE_TBDR)
void material_depth_offset_write(KernelGlobals &kg, ShadingData &sd, float depth_offset, float &out_depth)
{
  if (material_depth_offset_is_clipped(kg, sd, depth_offset)) {
    gpu_discard_fragment();
    return;
  }
  out_depth = material_depth_offset_frag_depth(kg, sd, depth_offset);
}

void material_depth_offset_write(KernelGlobals &kg, ShadingData &sd, float &out_depth)
{
  material_depth_offset_write(kg, sd, nodetree_depth_offset(kg, sd), out_depth);
}
#  endif
#endif

