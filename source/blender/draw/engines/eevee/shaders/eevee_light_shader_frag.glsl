/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Evaluate an Eevee light data-block node tree as a screen-space direct-light modifier.
 */

#include "infos/eevee_light_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_light_shader)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.glsl"
#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_light_lib.glsl"
#include "eevee_light_shader_common_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

float4 nodetree_light_shader();

void main()
{
  light_shader_globals_init();

  const int2 texel = int2(gl_FragCoord.xy);
  const float depth = texelFetch(hiz_tx, texel, 0).r;
  const gbuffer::Layers gbuf = gbuffer::read_layers(texel);

  if (gbuf.header.is_empty()) {
    out_light_shader = float4(1.0f);
    return;
  }

  const float surface_depth = gbuffer::read_surface_depth(gbuf.header, texel, depth);
  const float3 P = drw_point_screen_to_world(float3(screen_uv, surface_depth));

  g_data.P = P;
  g_data.N = g_data.Ni = gbuf.surface_N();
  g_data.Ng = gbuf.header.geometry_normal(g_data.N);
  g_data.ray_length = distance(g_data.P, drw_view_position());

  attrib_load(WorldPoint{0});

  out_light_shader = light_shader_result_clamp(nodetree_light_shader());
}
