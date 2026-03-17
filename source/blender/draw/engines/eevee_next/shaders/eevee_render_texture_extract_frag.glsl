/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_render_texture_info.hh"

/* Keep in sync with #SceneEEVEERenderTextureSource. */
#define RENDER_TEXTURE_SOURCE_COLOR 0
#define RENDER_TEXTURE_SOURCE_GRAYSCALE 1
#define RENDER_TEXTURE_SOURCE_DEPTH 2
#define RENDER_TEXTURE_SOURCE_NORMAL 3

vec3 render_texture_gbuffer_normal_unpack(vec2 normal_packed)
{
  normal_packed = normal_packed * 2.0 - 1.0;
  vec3 normal = vec3(
      normal_packed.x, normal_packed.y, 1.0 - abs(normal_packed.x) - abs(normal_packed.y));
  float fold = clamp(-normal.z, 0.0, 1.0);
  normal.x += (normal.x >= 0.0) ? -fold : fold;
  normal.y += (normal.y >= 0.0) ? -fold : fold;
  return normalize(normal);
}

void main()
{
  ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, output_extent))) {
    return;
  }
  vec4 combined = texelFetch(combined_tx, texel, 0);
  float depth = texelFetch(depth_tx, texel, 0).r;
  bool has_surface = depth < 1.0;
  bool has_gbuffer = texelFetch(gbuf_header_tx, texel, 0).r != 0u;

  vec4 out_color;

  switch (output_type) {
    case RENDER_TEXTURE_SOURCE_GRAYSCALE: {
      float luminance = dot(combined.rgb, vec3(0.2126, 0.7152, 0.0722));
      out_color = vec4(vec3(luminance), combined.a);
      break;
    }
    case RENDER_TEXTURE_SOURCE_DEPTH: {
      float depth_value = has_surface ? depth : 0.0;
      out_color = vec4(vec3(depth_value), has_surface ? 1.0 : 0.0);
      break;
    }
    case RENDER_TEXTURE_SOURCE_NORMAL: {
      vec3 normal = has_gbuffer ? render_texture_gbuffer_normal_unpack(
                                      texelFetch(gbuf_normal_tx, ivec3(texel, 0), 0).rg) :
                                  vec3(0.0);
      out_color = vec4(normal * 0.5 + 0.5, has_gbuffer ? 1.0 : 0.0);
      break;
    }
    case RENDER_TEXTURE_SOURCE_COLOR:
    default:
      out_color = combined;
      break;
  }

  imageStore(output_img, texel, out_color);
}
