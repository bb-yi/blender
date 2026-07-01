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

float4 filter_graph_debug_color(float3 color)
{
  /* Resolve output goes straight to the stage framebuffer; alpha must be opaque
   * for depth/normal/position debug visualization. */
  return float4(color, 1.0f);
}

float4 filter_graph_visualize_scene_depth(int2 texel, int2 extent)
{
  float depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
  bool has_surface = depth < 1.0f;
  float linear_depth = has_surface ? -drw_depth_screen_to_view(depth) : 0.0f;
  if (!has_surface || linear_depth <= 0.0f) {
    return filter_graph_debug_color(float3(0.0f));
  }
  float visible_depth = linear_depth / max(drw_view_far(), 1e-5f);
  return filter_graph_debug_color(float3(saturate(visible_depth)));
}

float4 filter_graph_visualize_handle(TextureHandle tex)
{
  int2 extent = (tex.type == TEX_HANDLE_FILTER_GRAPH_TEXTURE) ?
                    int2(textureSize(filter_graph_input_tx, 0).xy) :
                    textureSize(scene_color_tx, 0);
  int2 texel = clamp(int2(gl_FragCoord.xy), int2(0), extent - int2(1));

  switch (tex.type) {
    case TEX_HANDLE_RP_COLOR: {
      float4 color = texelFetch(rp_color_tx, int3(texel, int(tex.index)), 0);
      return filter_graph_debug_color(color.rgb);
    }
    case TEX_HANDLE_RP_VALUE: {
      float value = texelFetch(rp_value_tx, int3(texel, int(tex.index)), 0).r;
      return filter_graph_debug_color(value.xxx);
    }
    case TEX_HANDLE_FILTER_GRAPH_TEXTURE:
      return texelFetch(filter_graph_input_tx, int3(texel, int(tex.index)), 0);
    case TEX_HANDLE_SCENE:
      if (tex.index == 0) {
        return texelFetch(scene_color_tx, texel, 0);
      }
      if (tex.index == 1) {
        return filter_graph_visualize_scene_depth(texel, extent);
      }
      if (tex.index == 2 && uniform_buf.render_pass.normal_id >= 0) {
        float3 normal = texelFetch(
                            rp_color_tx, int3(texel, uniform_buf.render_pass.normal_id), 0)
                            .rgb;
        return filter_graph_debug_color(normal * 0.5f + 0.5f);
      }
      if (tex.index == 2) {
        return filter_graph_debug_color(float3(1.0f, 0.0f, 1.0f));
      }
      if (tex.index == 4 && uniform_buf.render_pass.position_id >= 0) {
        float3 position = texelFetch(
                              rp_color_tx, int3(texel, uniform_buf.render_pass.position_id), 0)
                              .rgb;
        return filter_graph_debug_color(fract(abs(position) * 0.1f));
      }
      if (tex.index == 4) {
        return filter_graph_debug_color(float3(0.0f, 1.0f, 1.0f));
      }
      break;
  }

  return filter_graph_debug_color(float3(0.0f));
}

void main()
{
  TextureHandle tex = TextureHandle(filter_graph_input_buf[0].type, filter_graph_input_buf[0].index);
  out_color = filter_graph_visualize_handle(tex);
}
