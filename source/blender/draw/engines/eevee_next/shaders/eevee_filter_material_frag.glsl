/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_filter_material_info.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_geom_world)
FRAGMENT_SHADER_CREATE_INFO(eevee_filter_material)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.glsl"
#include "eevee_nodetree_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

vec4 closure_to_rgba(Closure cl)
{
  UNUSED_VARS(cl);
  return vec4(0.0);
}

vec4 nodetree_filter();

void main()
{
  init_globals();
  /* Reuse the world-style fullscreen setup so regular shader node helpers still work. */
  g_data.N = drw_normal_view_to_world(drw_view_incident_vector(interp.P));
  g_data.Ng = g_data.N;
  g_data.P = -g_data.N;
  attrib_load();

  vec2 uv = clamp(gl_FragCoord.xy / vec2(textureSize(scene_color_tx, 0)), vec2(0.0), vec2(1.0));
  vec4 scene_color = texture(scene_color_tx, uv);
  vec4 filtered_color = nodetree_filter();

  out_color = vec4(mix(scene_color.rgb, filtered_color.rgb, filtered_color.a), scene_color.a);
}
