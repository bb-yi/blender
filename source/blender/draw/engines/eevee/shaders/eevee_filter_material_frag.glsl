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

int TextureHandle_source_kind(TextureHandle tex)
{
  if (tex.type == TEX_HANDLE_FILTER_GRAPH_INPUT) {
    if (tex.index < 0 || tex.index >= FILTER_GRAPH_INPUT_MAX) {
      return FILTER_GRAPH_SOURCE_COLOR;
    }
    return filter_graph_input_buf[tex.index].source_kind;
  }

  tex = filter_graph_input_resolve(tex);
  if (tex.type == TEX_HANDLE_SCENE) {
    if (tex.index == 0) {
      return FILTER_GRAPH_SOURCE_COLOR;
    }
    if (tex.index == 1) {
      return FILTER_GRAPH_SOURCE_DEPTH;
    }
    return FILTER_GRAPH_SOURCE_DATA;
  }
  if (tex.type == TEX_HANDLE_RP_VALUE) {
    return FILTER_GRAPH_SOURCE_VALUE;
  }
  if (tex.type == TEX_HANDLE_FILTER_GRAPH_TEXTURE) {
    return FILTER_GRAPH_SOURCE_INTERMEDIATE;
  }
  return FILTER_GRAPH_SOURCE_COLOR;
}

bool filter_graph_use_linear_resample(int source_kind)
{
  return source_kind == FILTER_GRAPH_SOURCE_COLOR ||
         source_kind == FILTER_GRAPH_SOURCE_INTERMEDIATE;
}

int2 filter_graph_output_extent()
{
  return int2(imageSize(filter_graph_output_img).xy);
}

int2 filter_graph_source_texel(float2 uv, int2 source_extent)
{
  return clamp(int2(uv * float2(source_extent)), int2(0), source_extent - int2(1));
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
  int source_kind = TextureHandle_source_kind(tex);
  tex = filter_graph_input_resolve(tex);
  if (tex.type == TEX_HANDLE_NULL) {
    return float4(0.0f);
  }

  int2 output_extent = filter_graph_output_extent();
  float2 uv = gl_FragCoord.xy / float2(output_extent);
  if (texel_offset) {
    uv += offset / float2(output_extent);
  }
  else {
    uv += offset;
  }
  uv = clamp(uv, float2(0.0f), float2(1.0f));

  switch (tex.type) {
    case TEX_HANDLE_RP_COLOR: {
      int2 extent = int2(textureSize(rp_color_tx, 0).xy);
      if (filter_graph_use_linear_resample(source_kind)) {
        return texture(rp_color_tx, float3(uv, float(tex.index)));
      }
      return texelFetch(rp_color_tx, int3(filter_graph_source_texel(uv, extent), int(tex.index)), 0);
    }
    case TEX_HANDLE_RP_VALUE: {
      int2 extent = int2(textureSize(rp_value_tx, 0).xy);
      return float4(texelFetch(rp_value_tx,
                               int3(filter_graph_source_texel(uv, extent), int(tex.index)),
                               0)
                        .rrr,
                    1.0f);
    }
    case TEX_HANDLE_FILTER_GRAPH_TEXTURE: {
      int2 extent = int2(textureSize(filter_graph_input_tx, 0).xy);
      if (filter_graph_use_linear_resample(source_kind)) {
        return texture(filter_graph_input_tx, float3(uv, float(tex.index)));
      }
      return texelFetch(filter_graph_input_tx,
                        int3(filter_graph_source_texel(uv, extent), int(tex.index)),
                        0);
    }
    case TEX_HANDLE_SCENE:
      if (tex.index == 0) {
        /* Return raw scene color. Alpha (transmittance) inversion is handled
         * by the Image Sample node, matching the original Scene Color behavior. */
        int2 extent = textureSize(scene_color_tx, 0);
        if (filter_graph_use_linear_resample(source_kind)) {
          return texture(scene_color_tx, uv);
        }
        int2 texel = filter_graph_source_texel(uv, extent);
        return texelFetch(scene_color_tx, texel, 0);
      }
      if (tex.index == 1) {
        return filter_scene_depth_color(uv);
      }
      if (tex.index == 2) {
        int2 texel = filter_graph_source_texel(uv, int2(textureSize(rp_color_tx, 0).xy));
        return filter_scene_normal_color(texel, uv);
      }
      if (tex.index == 4) {
        int2 texel = filter_graph_source_texel(uv, int2(textureSize(rp_color_tx, 0).xy));
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
  int source_kind = TextureHandle_source_kind(tex);
  tex = filter_graph_input_resolve(tex);
  if (tex.type == TEX_HANDLE_NULL) {
    return float4(0.0f);
  }

  uv = clamp(uv, float2(0.0f), float2(1.0f));
  switch (tex.type) {
    case TEX_HANDLE_RP_COLOR: {
      int2 extent = int2(textureSize(rp_color_tx, 0).xy);
      if (filter_graph_use_linear_resample(source_kind)) {
        return texture(rp_color_tx, float3(uv, float(tex.index)));
      }
      return texelFetch(rp_color_tx, int3(filter_graph_source_texel(uv, extent), int(tex.index)), 0);
    }
    case TEX_HANDLE_RP_VALUE: {
      int2 extent = int2(textureSize(rp_value_tx, 0).xy);
      return float4(texelFetch(rp_value_tx,
                               int3(filter_graph_source_texel(uv, extent), int(tex.index)),
                               0)
                        .rrr,
                    1.0f);
    }
    case TEX_HANDLE_FILTER_GRAPH_TEXTURE: {
      int2 extent = int2(textureSize(filter_graph_input_tx, 0).xy);
      if (filter_graph_use_linear_resample(source_kind)) {
        return texture(filter_graph_input_tx, float3(uv, float(tex.index)));
      }
      return texelFetch(filter_graph_input_tx,
                        int3(filter_graph_source_texel(uv, extent), int(tex.index)),
                        0);
    }
    case TEX_HANDLE_SCENE:
      if (tex.index == 0) {
        int2 extent = textureSize(scene_color_tx, 0);
        if (filter_graph_use_linear_resample(source_kind)) {
          return texture(scene_color_tx, uv);
        }
        int2 texel = filter_graph_source_texel(uv, extent);
        return texelFetch(scene_color_tx, texel, 0);
      }
      if (tex.index == 1) {
        return filter_scene_depth_color(uv);
      }
      if (tex.index == 2) {
        int2 texel = filter_graph_source_texel(uv, int2(textureSize(rp_color_tx, 0).xy));
        return filter_scene_normal_color(texel, uv);
      }
      if (tex.index == 4) {
        int2 texel = filter_graph_source_texel(uv, int2(textureSize(rp_color_tx, 0).xy));
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
  const ViewMatrices view = view_matrices_get();
  g_data.N = view.normal_view_to_world(view.view_incident_vector(interp.P));
  g_data.Ni = g_data.N;
  g_data.Ng = g_data.N;
  g_data.P = -g_data.N;
  attrib_load(WorldPoint{g_data.P});

  nodetree_filter_outputs();
}
