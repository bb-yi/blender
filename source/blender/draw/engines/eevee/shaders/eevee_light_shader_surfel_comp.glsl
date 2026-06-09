/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Evaluate an Eevee light data-block node tree at Volume Probe bake surfel positions.
 */

#include "infos/eevee_light_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_light_shader_surfel)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.glsl"
#include "eevee_light_lib.glsl"
#include "eevee_light_shader_common_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

float4 nodetree_light_shader();

void main()
{
  int index = int(gl_GlobalInvocationID.x);
  if (index >= int(capture_info_buf.surfel_len)) {
    return;
  }

  Surfel surfel = surfel_buf[index];

  light_shader_globals_init();

  float3 P = surfel.position;
  g_data.P = P;
  g_data.N = g_data.Ni = surfel.normal;
  g_data.Ng = surfel.normal;
  g_data.ray_length = distance(P, drw_view_position());

  attrib_load(WorldPoint{float3(0.0f)});

  out_light_shader_buf[light_index * int(capture_info_buf.surfel_len) + index] =
      light_shader_result_clamp(nodetree_light_shader());
}
