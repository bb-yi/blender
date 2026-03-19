/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_filter_material_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)
FRAGMENT_SHADER_CREATE_INFO(eevee_geom_world)
FRAGMENT_SHADER_CREATE_INFO(eevee_filter_material)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

#define TEX_HANDLE_NULL 0u
#define TEX_HANDLE_RP_COLOR 1u
#define TEX_HANDLE_RP_VALUE 2u

float4 closure_to_rgba(Closure cl)
{
  UNUSED_VARS(cl);
  return float4(0.0f);
}

float4 nodetree_filter();

#define TEXTURE_HANDLE_EVAL_DEFINED

void input_aov_impl(uint hash, out TextureHandle color, out TextureHandle value)
{
  int color_index = aov_color_index(hash);
  int value_index = aov_value_index(hash);

  color = (color_index >= 0) ? TextureHandle(TEX_HANDLE_RP_COLOR,
                                             int(uniform_buf.render_pass.color_len) + color_index) :
                               TEXTURE_HANDLE_DEFAULT;
  value = (value_index >= 0) ? TextureHandle(TEX_HANDLE_RP_VALUE,
                                             int(uniform_buf.render_pass.value_len) + value_index) :
                               TEXTURE_HANDLE_DEFAULT;
}

float4 TextureHandle_eval(TextureHandle tex, float2 offset, bool texel_offset)
{
  if (tex.type == TEX_HANDLE_NULL) {
    return float4(0.0f);
  }

  int2 extent = textureSize(scene_color_tx, 0);
  int2 texel = int2(gl_FragCoord.xy);
  if (texel_offset) {
    texel += int2(offset);
  }
  else {
    float2 uv = clamp((gl_FragCoord.xy / float2(extent)) + offset,
                      float2(0.0f),
                      float2(1.0f));
    texel = int2(uv * float2(extent));
  }

  texel = clamp(texel, int2(0), extent - int2(1));

  switch (tex.type) {
    case TEX_HANDLE_RP_COLOR:
      return texelFetch(rp_color_tx, int3(texel, int(tex.index)), 0);
    case TEX_HANDLE_RP_VALUE:
      return float4(texelFetch(rp_value_tx, int3(texel, int(tex.index)), 0).rrr, 1.0f);
    default:
      return float4(0.0f);
  }
}

float4 TextureHandle_eval(TextureHandle tex)
{
  return TextureHandle_eval(tex, float2(0.0f), true);
}

void main()
{
  init_globals();
  g_data.N = drw_normal_view_to_world(drw_view_incident_vector(interp.P));
  g_data.Ng = g_data.N;
  g_data.P = -g_data.N;
  attrib_load(WorldPoint{0});

  float4 filter_result = nodetree_filter();
  out_color = float4(filter_result.rgb, saturate(1.0f - filter_result.a));
}
