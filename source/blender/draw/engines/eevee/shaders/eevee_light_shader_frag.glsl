/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Evaluate an Eevee light data-block node tree as a screen-space direct-light modifier.
 */

#include "infos/eevee_light_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_light_shader)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.bsl.hh"
#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_light_lib.glsl"
#include "eevee_light_shader_common_lib.glsl"
#include "eevee_nodetree_frag_lib.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"

float4 nodetree_light_shader(KernelGlobals kg, ShadingData &sd);

void main()
{
  [[resource_table]] KernelGlobals &kg = resource_table_get(KernelGlobals);
  ShadingData sd;
  light_shader_globals_init(sd);
  sd.frag_co = gl_FragCoord;

  const int2 texel = int2(gl_FragCoord.xy);
  const float depth = texelFetch(hiz_tx, texel, 0).r;
  const gbuffer::Layers gbuf = gbuffer::read_layers(texel);

  if (gbuf.header.is_empty()) {
    out_light_shader = float4(1.0f);
    return;
  }

  const float surface_depth = gbuffer::read_surface_depth(gbuf.header, texel, depth);
  const float3 P = drw_point_screen_to_world(float3(screen_uv, surface_depth));

  sd.P = P;
  sd.N = sd.Ni = gbuf.surface_N();
  sd.Ng = gbuf.header.geometry_normal(sd.N);
  sd.ray_length = distance(P, drw_view_position());

  attrib_load(WorldPoint{float3(0.0f)});

  out_light_shader = light_shader_result_clamp(nodetree_light_shader(kg, sd));
}
