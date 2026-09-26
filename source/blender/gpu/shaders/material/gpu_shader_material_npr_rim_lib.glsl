/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_principled_npr_v2_math.glsl"
#include "gpu_shader_material_principled_npr_depth_rim.glsl"

float npr_v2_rim_shape(float3 N, float3 V, float4 shape, float falloff)
{
  float3 camera_x = float3(1.0f, 0.0f, 0.0f);
  float3 camera_y = float3(0.0f, 1.0f, 0.0f);
#if defined(GPU_FRAGMENT_SHADER)
  camera_x = npr_v2_safe_normalize(drw_view().viewinv[0].xyz, camera_x);
  camera_y = npr_v2_safe_normalize(drw_view().viewinv[1].xyz, camera_y);
#endif
  float2 projected = float2(dot(N, camera_x), dot(N, camera_y));
  float len = length(projected);
  if (len <= 1e-6f || shape.y <= 1e-6f || shape.w <= 1e-6f) {
    return 0.0f;
  }
  projected /= len;
  float turns = atan(projected.y, projected.x) / (2.0f * M_PI);
  float centered = abs(fract(turns - shape.x / 360.0f + 0.5f) - 0.5f);
  float half_length = saturate(shape.y) * 0.5f;
  float arc_width = max(shape.z, 0.0f) * 0.5f;
  float arc = arc_width <= 1e-6f ? 1.0f - step(half_length, centered) :
      1.0f - smoothstep(max(half_length - arc_width, 0.0f), half_length, centered);
  float facing = saturate(1.0f - dot(N, V));
  float edge = 1.0f - saturate(shape.w);
  float width = max(falloff, 0.0f);
  return arc * (width <= 1e-6f ? step(edge, facing) :
                 smoothstep(edge - 0.5f * width, edge + 0.5f * width, facing));
}
