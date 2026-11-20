/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_nodetree_frag_lib.bsl.hh"
#include "eevee_outline_lib.glsl"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"

float4 closure_to_rgba_depth(KernelGlobals &kg, ShadingData &sd, Closure /*cl*/)
{
  const float4 value = float4(sd.emission, saturate(1.0f - average(sd.transmittance)));
  closure_weights_reset(kg, sd, 0.0f);
  return value;
}

namespace eevee {

struct OutlineFreestyleData {
  [[sampler(OUTLINE_DEPTH_TEX_SLOT)]] sampler2D depth_tx;
  [[sampler(OUTLINE_COLOR_TEX_SLOT)]] sampler2D outline_color_tx;
  [[sampler(OUTLINE_INFO_TEX_SLOT)]] usampler2DArray outline_info_tx;
  [[push_constant]] int2 outline_extent;
};

struct OutlineFreestyleFragOut {
  [[frag_color(0)]] float4 out_outline_seed;
};

[[fragment]]
void outline_freestyle([[resource_table]] KernelGlobals &kg,
                      [[resource_table]] const OutlineFreestyleData &srt,
                      [[frag_coord]] const float4 frag_co,
                      [[in]] const VertOutCommon &interp,
                      [[front_facing]] const bool front_face,
                      [[out]] OutlineFreestyleFragOut &frag_out)
{
  const int2 extent = textureSize(srt.depth_tx, 0);
  const bool reduced = any(notEqual(srt.outline_extent, extent));
  int2 texel = int2(frag_co.xy);
  if (reduced) {
    texel = outline_source_texel(texel, extent, srt.outline_extent);
  }
  float line_depth = reverse_z::read(frag_co.z);
#ifdef MAT_DEPTH_OFFSET
  const ViewMatrices view = kg.view.get(0);
  ShadingData sd = init_globals(kg.pipe, kg.uniforms, interp, view, front_face, frag_co);
  init_globals_mesh(interp, sd, float3(0.0f));
  line_depth = material_depth_offset_screen_depth(kg, sd, nodetree_depth_offset(kg, sd));
#endif
  if (line_depth > reverse_z::read(texelFetch(srt.depth_tx, texel, 0).r) + 1e-5f) {
    gpu_discard_fragment();
    return;
  }
  const uint4 info = texelFetch(srt.outline_info_tx, int3(texel, 0), 0);
  const float4 color = texelFetch(srt.outline_color_tx, texel, 0);
  const FilmData &film = kg.uniforms.uniform_buf.film;
  float width_scale = film.sr_active ? sqrt(film.sr_render_ratio.x * film.sr_render_ratio.y) :
                                      1.0f;
  if (reduced) {
    const float2 ratio = float2(srt.outline_extent) / float2(extent);
    width_scale *= sqrt(ratio.x * ratio.y);
  }
  const float width = outline_width_unpack(info.r) * width_scale;
  if (width <= 0.0f || color.a <= 0.0f || !outline_freestyle_edge_unpack(info.b)) {
    gpu_discard_fragment();
    return;
  }
  frag_out.out_outline_seed = float4(1.0f, 0.0f, 0.0f, width);
}

}  // namespace eevee
