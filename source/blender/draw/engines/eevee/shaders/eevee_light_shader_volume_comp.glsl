/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Evaluate an Eevee light data-block node tree at volume froxel positions.
 */

#include "infos/eevee_light_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_light_shader_volume)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.glsl"
#include "eevee_light_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_volume_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

float4 nodetree_light_shader();

void init_light_shader_globals()
{
  g_data.is_strand = false;
  g_data.hair_diameter = 0.0f;
  g_data.hair_strand_id = 0;
  g_data.ray_type = uniform_buf.pipeline.ray_type;
  g_data.ray_depth = 0.0f;
  g_data.barycentric_coords = float2(0.0f);
  g_data.barycentric_dists = float3(0.0f);
  g_data.curve_T = float3(0.0f);
  g_data.curve_B = float3(0.0f);
  g_data.curve_N = float3(0.0f);
}

void main()
{
  int3 froxel = int3(gl_GlobalInvocationID);

  if (any(greaterThanEqual(froxel, uniform_buf.volumes.tex_size))) {
    return;
  }

  init_light_shader_globals();

  float offset = sampling_rng_1D_get(SAMPLING_VOLUME_W);
  float jitter = volume_froxel_jitter(froxel.xy, offset);
  float3 uvw = (float3(froxel) + float3(0.5f, 0.5f, 0.5f - jitter)) *
               uniform_buf.volumes.inv_tex_size;
  float3 vP = volume_jitter_to_view(uvw);
  float3 P = drw_point_view_to_world(vP);

  g_data.P = P;
  g_data.N = g_data.Ni = drw_world_incident_vector(P);
  g_data.Ng = g_data.N;
  g_data.ray_length = distance(g_data.P, drw_view_position());

  attrib_load(WorldPoint{0});

  float4 result = nodetree_light_shader();
  int layer = light_index * uniform_buf.volumes.tex_size.z + froxel.z;
  imageStoreFast(out_light_shader_img,
                 int3(froxel.xy, layer),
                 float4(max(result.rgb, float3(0.0f)), max(result.a, 0.0f)));
}
