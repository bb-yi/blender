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
#include "eevee_reverse_z_lib.glsl"
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
void nodetree_filter_outputs();

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

TextureHandle filter_graph_input_resolve(TextureHandle tex)
{
  if (tex.type != TEX_HANDLE_FILTER_GRAPH_INPUT) {
    return tex;
  }
  if (tex.index < 0 || tex.index >= FILTER_GRAPH_INPUT_MAX) {
    return TEXTURE_HANDLE_DEFAULT;
  }

  TextureHandle resolved = TextureHandle(filter_graph_input_buf[tex.index].type,
                                         filter_graph_input_buf[tex.index].index);
  return (resolved.type != TEX_HANDLE_FILTER_GRAPH_INPUT) ? resolved : TEXTURE_HANDLE_DEFAULT;
}

int TextureHandle_alpha_mode(TextureHandle tex)
{
  if (tex.type == TEX_HANDLE_FILTER_GRAPH_INPUT) {
    if (tex.index < 0 || tex.index >= FILTER_GRAPH_INPUT_MAX) {
      return FILTER_GRAPH_ALPHA_MODE_OPACITY;
    }
    return filter_graph_input_buf[tex.index].alpha_mode;
  }

  tex = filter_graph_input_resolve(tex);
  if (tex.type == TEX_HANDLE_SCENE && tex.index == 0) {
    return FILTER_GRAPH_ALPHA_MODE_TRANSMITTANCE;
  }
  if (tex.type == TEX_HANDLE_SCENE && tex.index == 1) {
    return FILTER_GRAPH_ALPHA_MODE_DEPTH;
  }
  return FILTER_GRAPH_ALPHA_MODE_OPACITY;
}

bool TextureHandle_stores_transmittance_alpha(TextureHandle tex)
{
  return TextureHandle_alpha_mode(tex) == FILTER_GRAPH_ALPHA_MODE_TRANSMITTANCE;
}

bool TextureHandle_is_scene_depth(TextureHandle tex)
{
  return TextureHandle_alpha_mode(tex) == FILTER_GRAPH_ALPHA_MODE_DEPTH;
}

float filter_scene_depth_value(float2 uv)
{
  return reverse_z::read(texture(depth_tx, uv).r);
}

float filter_scene_depth_linear(float2 uv)
{
  return -drw_depth_screen_to_view(filter_scene_depth_value(uv));
}

float4 filter_scene_depth_color(float2 uv)
{
  float depth = filter_scene_depth_linear(uv);
  return float4(depth.xxx, 1.0f);
}

float4 filter_scene_normal_color(int2 texel, float2 uv)
{
  if (uniform_buf.render_pass.normal_id >= 0) {
    return float4(texelFetch(rp_color_tx, int3(texel, uniform_buf.render_pass.normal_id), 0).rgb,
                  1.0f);
  }

  float depth = filter_scene_depth_value(uv);
  if (depth >= 1.0f) {
    return float4(0.0f);
  }

  float3 position = drw_point_screen_to_world(float3(uv, depth));
  float3 normal = normalize(cross(gpu_dfdx(position), gpu_dfdy(position)));
  return float4(normal, 1.0f);
}

float4 filter_scene_position_color(int2 texel, float2 uv)
{
  if (uniform_buf.render_pass.position_id >= 0) {
    return float4(texelFetch(rp_color_tx, int3(texel, uniform_buf.render_pass.position_id), 0).rgb,
                  1.0f);
  }

  float depth = filter_scene_depth_value(uv);
  if (depth >= 1.0f) {
    return float4(0.0f);
  }

  return float4(drw_point_screen_to_world(float3(uv, depth)), 1.0f);
}

float4 TextureHandle_eval(TextureHandle tex, float2 offset, bool texel_offset)
{
  tex = filter_graph_input_resolve(tex);
  if (tex.type == TEX_HANDLE_NULL) {
    return float4(0.0f);
  }

  int2 extent = (tex.type == TEX_HANDLE_FILTER_GRAPH_TEXTURE) ?
                    int2(textureSize(filter_graph_input_tx, 0).xy) :
                    textureSize(scene_color_tx, 0);
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
    case TEX_HANDLE_FILTER_GRAPH_TEXTURE:
      return texelFetch(filter_graph_input_tx, int3(texel, int(tex.index)), 0);
    case TEX_HANDLE_SCENE:
      if (tex.index == 0) {
        /* Return raw scene color. Alpha (transmittance) inversion is handled
         * by the Image Sample node, matching the original Scene Color behavior. */
        return texelFetch(scene_color_tx, texel, 0);
      }
      if (tex.index == 1) {
        float2 uv = (float2(texel) + 0.5f) / float2(extent);
        return filter_scene_depth_color(uv);
      }
      if (tex.index == 2) {
        float2 uv = (float2(texel) + 0.5f) / float2(extent);
        return filter_scene_normal_color(texel, uv);
      }
      if (tex.index == 4) {
        float2 uv = (float2(texel) + 0.5f) / float2(extent);
        return filter_scene_position_color(texel, uv);
      }
      return float4(0.0f);
    default:
      return float4(0.0f);
  }
}

float4 TextureHandle_eval(TextureHandle tex)
{
  return TextureHandle_eval(tex, float2(0.0f), true);
}

/* Absolute UV sampling: use the provided uv directly (0-1 range), ignoring
 * gl_FragCoord. Enables procedural coordinate sampling (Voronoi, screen
 * coordinate nodes, etc.), matching the old Scene Color Vector input. */
float4 TextureHandle_eval_uv(TextureHandle tex, float2 uv)
{
  tex = filter_graph_input_resolve(tex);
  if (tex.type == TEX_HANDLE_NULL) {
    return float4(0.0f);
  }

  int2 extent = (tex.type == TEX_HANDLE_FILTER_GRAPH_TEXTURE) ?
                    int2(textureSize(filter_graph_input_tx, 0).xy) :
                    textureSize(scene_color_tx, 0);
  uv = clamp(uv, float2(0.0f), float2(1.0f));
  int2 texel = clamp(int2(uv * float2(extent)), int2(0), extent - int2(1));
  switch (tex.type) {
    case TEX_HANDLE_RP_COLOR:
      return texelFetch(rp_color_tx, int3(texel, int(tex.index)), 0);
    case TEX_HANDLE_RP_VALUE:
      return float4(texelFetch(rp_value_tx, int3(texel, int(tex.index)), 0).rrr, 1.0f);
    case TEX_HANDLE_FILTER_GRAPH_TEXTURE:
      return texelFetch(filter_graph_input_tx, int3(texel, int(tex.index)), 0);
    case TEX_HANDLE_SCENE:
      if (tex.index == 0) {
        return texelFetch(scene_color_tx, texel, 0);
      }
      if (tex.index == 1) {
        return filter_scene_depth_color(uv);
      }
      if (tex.index == 2) {
        return filter_scene_normal_color(texel, uv);
      }
      if (tex.index == 4) {
        return filter_scene_position_color(texel, uv);
      }
      return float4(0.0f);
    default:
      return float4(0.0f);
  }
}

void main()
{
  init_globals();
  g_data.N = drw_normal_view_to_world(drw_view_incident_vector(interp.P));
  g_data.Ng = g_data.N;
  g_data.P = -g_data.N;
  attrib_load(WorldPoint{0});

  nodetree_filter_outputs();
}
