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
  uint total_len = uniform_buf.render_pass.aovs.color_len + uniform_buf.render_pass.aovs.value_len;
  uint hash_index;
  for (hash_index = 0u; hash_index < AOV_MAX && hash_index < total_len; hash_index += 4u) {
    bool4 cmp_mask = equal(uniform_buf.render_pass.aovs.hash[hash_index >> 2u], uint4(hash));
    if (any(cmp_mask)) {
      hash_index += (cmp_mask[0] ? 0u : (cmp_mask[1] ? 1u : (cmp_mask[2] ? 2u : 3u)));
      break;
    }
  }

  if (hash_index < total_len) {
    bool is_value = hash_index >= uint(uniform_buf.render_pass.aovs.color_len);
    uint aov_index = hash_index - (is_value ? uint(uniform_buf.render_pass.aovs.color_len) : 0u);
    int render_pass_index = (is_value ? uniform_buf.render_pass.value_len :
                                        uniform_buf.render_pass.color_len) +
                            int(aov_index);
    color = is_value ? TEXTURE_HANDLE_DEFAULT :
                       TextureHandle(TEX_HANDLE_RP_COLOR, render_pass_index);
    value = is_value ? TextureHandle(TEX_HANDLE_RP_VALUE, render_pass_index) :
                       TEXTURE_HANDLE_DEFAULT;
    return;
  }

  color = TEXTURE_HANDLE_DEFAULT;
  value = TEXTURE_HANDLE_DEFAULT;
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
  float3 world_N = drw_normal_view_to_world(drw_view_incident_vector(interp.P));
  g_data.N = world_N;
  g_data.Ni = world_N;
  g_data.Ng = world_N;
  g_data.P = -world_N;
  attrib_load(WorldPoint{g_data.P});

  float4 filter_result = nodetree_filter();
  out_color = float4(filter_result.rgb, saturate(1.0f - filter_result.a));
}
