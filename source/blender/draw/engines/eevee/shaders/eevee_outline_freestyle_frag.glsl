/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_freestyle)
FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)

#include "draw_view_lib.glsl"
#include "eevee_outline_lib.glsl"
#include "eevee_reverse_z_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_surf_common.bsl.hh"

float4 closure_to_rgba_depth(Closure /*cl*/)
{
  return float4(g_emission, saturate(1.0f - average(g_transmittance)));
}

float4 outline_source_color_fetch(int2 texel)
{
  return texelFetch(outline_color_tx, texel, 0);
}

uint4 outline_source_info_fetch(int2 texel)
{
  return texelFetch(outline_info_tx, int3(texel, 0), 0);
}

float outline_depth_fetch(int2 texel)
{
  return reverse_z::read(texelFetch(depth_tx, texel, 0).r);
}

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = textureSize(depth_tx, 0);
  if (any(lessThan(texel, int2(0))) || any(greaterThanEqual(texel, extent))) {
    discard;
  }

  float line_depth = reverse_z::read(gl_FragCoord.z);
#ifdef MAT_DEPTH_OFFSET
  [[resource_table]] const eevee::Uniform &uni = resource_table_get(eevee::Uniform);
  init_globals(uni, view_matrices_get(), true);
  line_depth = material_depth_offset_screen_depth(nodetree_depth_offset());
#endif
  const float scene_depth = outline_depth_fetch(texel);
  if (line_depth > scene_depth + 1.0e-5f) {
    discard;
  }

  const float4 outline_color = outline_source_color_fetch(texel);
  const uint4 outline_info = outline_source_info_fetch(texel);
  const float sr_width_scale = uniform_buf.film.sr_active ?
                                   sqrt(uniform_buf.film.sr_render_ratio.x *
                                        uniform_buf.film.sr_render_ratio.y) :
                                   1.0f;
  const float line_width = outline_width_unpack(outline_info.r) * sr_width_scale;
  if (line_width <= 0.0f || outline_color.a <= 0.0f ||
      !outline_freestyle_edge_unpack(outline_info.b))
  {
    discard;
  }

  /* Match the detect pass seed layout: .a = full line width (for JFA coverage), .r = width
   * modulation factor (1.0 = no modulation; Freestyle edges are not strength-modulated).
   * Color is read from outline_color_tx in resolve, so seed.gb are unused. */
  out_outline_seed = float4(1.0f, 0.0f, 0.0f, line_width);
}
