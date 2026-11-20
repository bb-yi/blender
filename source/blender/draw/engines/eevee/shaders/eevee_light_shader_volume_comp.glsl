/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Evaluate an Eevee light data-block node tree at volume froxel positions.
 */

#include "infos/eevee_light_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_light_shader_volume)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.bsl.hh"
#include "eevee_light_lib.glsl"
#include "eevee_light_shader_common_lib.glsl"
#include "eevee_nodetree_frag_lib.bsl.hh"
#include "eevee_volume_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

float4 nodetree_light_shader(KernelGlobals kg, ShadingData &sd);

void main()
{
  int3 froxel = int3(gl_GlobalInvocationID);

  if (any(greaterThanEqual(froxel, uniform_buf.volumes.tex_size))) {
    return;
  }

  [[resource_table]] KernelGlobals &kg = resource_table_get(KernelGlobals);
  ShadingData sd;
  light_shader_globals_init(sd);

  float offset = eevee_sampling_rng_1D_get(SAMPLING_VOLUME_W);
  float jitter = volume_froxel_jitter(froxel.xy, offset);
  float3 uvw = (float3(froxel) + float3(0.5f, 0.5f, 0.5f - jitter)) *
               uniform_buf.volumes.inv_tex_size;
  [[resource_table]] const eevee::Uniform &uni = resource_table_get(eevee::Uniform);
  float3 vP = volume_jitter_to_view(uni, drw_view(), uvw);
  float3 P = drw_point_view_to_world(vP);

  sd.P = P;
  sd.N = sd.Ni = drw_world_incident_vector(P);
  sd.Ng = sd.N;
  sd.ray_length = distance(P, drw_view_position());

  attrib_load(WorldPoint{float3(0.0f)});

  int layer = light_index * uniform_buf.volumes.tex_size.z + froxel.z;
  imageStoreFast(out_light_shader_img,
                 int3(froxel.xy, layer),
                 light_shader_result_clamp(nodetree_light_shader(kg, sd)));
}
