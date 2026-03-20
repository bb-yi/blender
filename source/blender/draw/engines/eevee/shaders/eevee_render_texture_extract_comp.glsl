/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_render_texture_infos.hh"

#include "draw_view_lib.glsl"
#include "eevee_reverse_z_lib.glsl"

/* Keep in sync with #SceneEEVEERenderTextureSource. */
#define RENDER_TEXTURE_SOURCE_COLOR 0
#define RENDER_TEXTURE_SOURCE_GRAYSCALE 1
#define RENDER_TEXTURE_SOURCE_DEPTH 2
#define RENDER_TEXTURE_SOURCE_NORMAL 3

float3 render_texture_gbuffer_normal_unpack(float2 normal_packed)
{
  normal_packed = normal_packed * 2.0f - 1.0f;
  float3 normal = float3(
      normal_packed.x, normal_packed.y, 1.0f - abs(normal_packed.x) - abs(normal_packed.y));
  float fold = clamp(-normal.z, 0.0f, 1.0f);
  normal.x += (normal.x >= 0.0f) ? -fold : fold;
  normal.y += (normal.y >= 0.0f) ? -fold : fold;
  return normalize(normal);
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, output_extent))) {
    return;
  }

  float4 combined = texelFetch(combined_tx, texel, 0);
  float depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
  bool has_surface = depth < 1.0f;
  bool has_gbuffer = texelFetch(gbuf_header_tx, int3(texel, 0), 0).r != 0u;

  float4 out_color;

  switch (output_type) {
    case RENDER_TEXTURE_SOURCE_GRAYSCALE: {
      float luminance = dot(combined.rgb, float3(0.2126f, 0.7152f, 0.0722f));
      out_color = float4(float3(luminance), combined.a);
      break;
    }
    case RENDER_TEXTURE_SOURCE_DEPTH: {
      float depth_value = has_surface ? -drw_depth_screen_to_view(depth) : 0.0f;
      out_color = float4(float3(depth_value), has_surface ? 1.0f : 0.0f);
      break;
    }
    case RENDER_TEXTURE_SOURCE_NORMAL: {
      float3 normal = has_gbuffer ? render_texture_gbuffer_normal_unpack(
                                        texelFetch(gbuf_normal_tx, int3(texel, 0), 0).rg) :
                                    float3(0.0f);
      out_color = float4(normal * 0.5f + 0.5f, has_gbuffer ? 1.0f : 0.0f);
      break;
    }
    case RENDER_TEXTURE_SOURCE_COLOR:
    default:
      out_color = combined;
      break;
  }

  imageStore(output_img, texel, out_color);
}
