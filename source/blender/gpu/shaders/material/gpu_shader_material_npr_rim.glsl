/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_material_npr_rim_lib.glsl"

[[node]]
void node_npr_rim(float4 color, float strength, float3 normal,
                  float angle, float arc_length, float arc_falloff,
                  float thickness, float thickness_falloff,
                  float width_px, float threshold, float softness, float samples,
                  float light_bias, float light_factor, float mask, float alpha,
                  float mode, float supported, float4 &result, float &factor)
{
  float shape = 0.0f;
  if (mode < 0.5f) {
    float3 N = npr_v2_safe_normalize(normal, g_data.N);
    float3 V = npr_v2_safe_normalize(coordinate_incoming(g_data.P), N);
    shape = npr_v2_rim_shape(N, V, float4(angle, arc_length, arc_falloff, thickness),
                             thickness_falloff);
  }
  else if (supported > 0.5f && alpha >= 1.0f) {
    shape = npr_v2_depth_rim(width_px, threshold, softness, angle,
                            arc_length, arc_falloff, samples);
  }
  float bias = clamp(light_bias, -1.0f, 1.0f);
  float lighting = saturate(light_factor);
  factor = shape * saturate(mask) * (bias >= 0.0f ? mix(1.0f, lighting, bias) :
                                                     mix(1.0f, 1.0f - lighting, -bias));
  result = float4(max(color.rgb, float3(0.0f)) * max(strength, 0.0f) * factor, 1.0f);
}
