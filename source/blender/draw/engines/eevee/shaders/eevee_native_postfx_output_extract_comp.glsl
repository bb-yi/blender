/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_native_postfx_output_infos.hh"

#include "eevee_reverse_z_lib.glsl"

#define NATIVE_POSTFX_EXTRACT_DEPTH 0
#define NATIVE_POSTFX_EXTRACT_VECTOR 1
#define NATIVE_POSTFX_EXTRACT_COLOR_PASS 2
#define NATIVE_POSTFX_EXTRACT_VALUE_PASS 3
#define NATIVE_POSTFX_EXTRACT_OUTLINE 4

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, imageSize(out_color_img)))) {
    return;
  }

  float4 out_color = float4(0.0f);
  if (source_kind == NATIVE_POSTFX_EXTRACT_DEPTH) {
    float depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
    bool has_surface = depth < 1.0f;
    out_color = float4(float3(has_surface ? depth : 0.0f), has_surface ? 1.0f : 0.0f);
  }
  else if (source_kind == NATIVE_POSTFX_EXTRACT_VECTOR) {
    out_color = texelFetch(vector_tx, texel, 0);
  }
  else if (source_kind == NATIVE_POSTFX_EXTRACT_OUTLINE) {
    out_color = texelFetch(outline_tx, texel, 0);
  }
  else if (source_layer >= 0) {
    if (source_kind == NATIVE_POSTFX_EXTRACT_VALUE_PASS) {
      float value = texelFetch(rp_value_tx, int3(texel, source_layer), 0).r;
      out_color = float4(float3(value), 1.0f);
    }
    else {
      out_color = texelFetch(rp_color_tx, int3(texel, source_layer), 0);
    }
  }

  imageStore(out_color_img, texel, out_color);
}
