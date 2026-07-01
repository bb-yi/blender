/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_filter_material_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_filter_graph_resolve)

#include "draw_view_lib.glsl"
#include "eevee_reverse_z_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

#define TEX_HANDLE_NULL 0u
#define TEX_HANDLE_RP_COLOR 1u
#define TEX_HANDLE_RP_VALUE 2u

float filter_graph_scene_depth_value(float2 uv)
{
  return reverse_z::read(texture(depth_tx, uv).r);
}

float filter_graph_scene_depth_linear(float2 uv)
{
  return -drw_depth_screen_to_view(filter_graph_scene_depth_value(uv));
}

float4 filter_graph_scene_depth_color(float2 uv)
{
  float depth = filter_graph_scene_depth_linear(uv);
  return float4(depth.xxx, 1.0f);
}

float4 filter_graph_scene_normal_color(int2 texel, float2 uv)
{
  if (uniform_buf.render_pass.normal_id >= 0) {
    return float4(texelFetch(rp_color_tx, int3(texel, uniform_buf.render_pass.normal_id), 0).rgb,
                  1.0f);
  }

  float depth = filter_graph_scene_depth_value(uv);
  if (depth >= 1.0f) {
    return float4(0.0f);
  }

  float3 position = drw_point_screen_to_world(float3(uv, depth));
  float3 normal = normalize(cross(gpu_dfdx(position), gpu_dfdy(position)));
  return float4(normal, 1.0f);
}

float4 filter_graph_scene_position_color(int2 texel, float2 uv)
{
  if (uniform_buf.render_pass.position_id >= 0) {
    return float4(texelFetch(rp_color_tx, int3(texel, uniform_buf.render_pass.position_id), 0).rgb,
                  1.0f);
  }

  float depth = filter_graph_scene_depth_value(uv);
  if (depth >= 1.0f) {
    return float4(0.0f);
  }

  return float4(drw_point_screen_to_world(float3(uv, depth)), 1.0f);
}

float4 filter_graph_eval_handle(TextureHandle tex)
{
  int2 extent = (tex.type == TEX_HANDLE_FILTER_GRAPH_TEXTURE) ?
                    int2(textureSize(filter_graph_input_tx, 0).xy) :
                    textureSize(scene_color_tx, 0);
  int2 texel = clamp(int2(gl_FragCoord.xy), int2(0), extent - int2(1));

  switch (tex.type) {
    case TEX_HANDLE_RP_COLOR: {
      return texelFetch(rp_color_tx, int3(texel, int(tex.index)), 0);
    }
    case TEX_HANDLE_RP_VALUE: {
      float value = texelFetch(rp_value_tx, int3(texel, int(tex.index)), 0).r;
      return float4(value.xxx, 1.0f);
    }
    case TEX_HANDLE_FILTER_GRAPH_TEXTURE:
      return texelFetch(filter_graph_input_tx, int3(texel, int(tex.index)), 0);
    case TEX_HANDLE_SCENE:
      if (tex.index == 0) {
        return texelFetch(scene_color_tx, texel, 0);
      }
      if (tex.index == 1) {
        float2 uv = (float2(texel) + 0.5f) / float2(extent);
        return filter_graph_scene_depth_color(uv);
      }
      if (tex.index == 2) {
        float2 uv = (float2(texel) + 0.5f) / float2(extent);
        return filter_graph_scene_normal_color(texel, uv);
      }
      if (tex.index == 4) {
        float2 uv = (float2(texel) + 0.5f) / float2(extent);
        return filter_graph_scene_position_color(texel, uv);
      }
      break;
  }

  return float4(0.0f);
}

float4 filter_graph_resolve_stage_output(TextureHandle tex)
{
  float4 color = filter_graph_eval_handle(tex);

  if (tex.type == TEX_HANDLE_FILTER_GRAPH_TEXTURE) {
    return color;
  }

  float opacity = color.a;
  if (tex.type == TEX_HANDLE_SCENE && tex.index == 0) {
    opacity = saturate(1.0f - color.a);
  }
  else if (tex.type == TEX_HANDLE_SCENE && tex.index == 1) {
    opacity = color.r;
  }

  return float4(color.rgb, saturate(1.0f - opacity));
}

void main()
{
  TextureHandle tex = TextureHandle(filter_graph_input_buf[0].type, filter_graph_input_buf[0].index);
  out_color = filter_graph_resolve_stage_output(tex);
}
