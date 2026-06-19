/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_nodetree_infos.hh"
#include "infos/eevee_surf_forward_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)
FRAGMENT_SHADER_CREATE_INFO(eevee_geom_mesh)
FRAGMENT_SHADER_CREATE_INFO(eevee_surf_outline_shell)

#include "draw_view_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

float4 closure_to_rgba(Closure cl_unused)
{
  return float4(0.0f);
}

void main()
{
  outline_output_reset();
  init_globals();

  float noise = utility_tx_fetch(utility_tx, gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling_rng_1D_get(SAMPLING_CLOSURE));
  nodetree_surface(closure_rand);

  if (g_outline_staged_color.a <= 0.0f || g_outline_staged_info.r <= 0.0f) {
    gpu_discard_fragment();
    return;
  }

  float shell_alpha = g_outline_staged_color.a;
  out_outline_shell = float4(g_outline_staged_color.rgb * shell_alpha, shell_alpha);
}
