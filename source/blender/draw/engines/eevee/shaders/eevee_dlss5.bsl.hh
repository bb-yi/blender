/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_reverse_z_lib.bsl.hh"
#include "gpu_shader_fullscreen_lib.glsl"

namespace eevee::dlss5 {

struct VertOut {
  [[smooth]] float2 screen_uv;
};

[[vertex]]
void fullscreen_vert([[vertex_id]] const int vert_id,
                     [[position]] float4 &out_position,
                     [[out]] VertOut &v_out)
{
  fullscreen_vertex(vert_id, out_position, v_out.screen_uv);
  out_position = reverse_z::transform(out_position);
}

struct Resources {
  [[sampler(0)]] sampler2DDepth depth_tx;
};

struct FragOut {
  [[frag_color(0)]] float4 color;
};

struct VelocityResources {
  [[sampler(0)]] sampler2D velocity_tx;
  [[push_constant]] float2 scale;
};

struct VelocityFragOut {
  [[frag_color(0)]] float2 velocity;
};

struct ColorResources {
  [[sampler(0)]] sampler2D color_tx;
  [[push_constant]] bool inverse;
};

struct HdrReconstructResources {
  [[sampler(0)]] sampler2D color_tx;
  [[sampler(1)]] sampler2D source_tx;
  [[sampler(2)]] sampler2D original_tx;
};

float srgb_from_linear(float value)
{
  value = max(value, 0.0f);
  return (value <= 0.0031308f) ? value * 12.92f :
                                  1.055f * pow(value, 1.0f / 2.4f) - 0.055f;
}

float linear_from_srgb(float value)
{
  value = max(value, 0.0f);
  return (value <= 0.04045f) ? value / 12.92f :
                                pow((value + 0.055f) / 1.055f, 2.4f);
}

float3 color_transfer(float3 color, bool inverse)
{
  if (inverse) {
    return float3(linear_from_srgb(color.r),
                  linear_from_srgb(color.g),
                  linear_from_srgb(color.b));
  }
  return float3(srgb_from_linear(color.r),
                srgb_from_linear(color.g),
                srgb_from_linear(color.b));
}

[[fragment]]
void color_convert_frag([[resource_table]] const ColorResources &resources,
                        [[frag_coord]] const float4 frag_co,
                        [[out]] FragOut &frag_out)
{
  const int2 texel = int2(frag_co.xy);
  float4 color = texelFetch(resources.color_tx, texel, 0);
  color.rgb = clamp(color_transfer(color.rgb, resources.inverse),
                    float3(0.0f),
                    float3(1.0f));
  frag_out.color = color;
}

[[fragment]]
void hdr_reconstruct_frag([[resource_table]] const HdrReconstructResources &resources,
                           [[frag_coord]] const float4 frag_co,
                           [[out]] FragOut &frag_out)
{
  const int2 texel = int2(frag_co.xy);
  const float4 color = texelFetch(resources.color_tx, texel, 0);
  const float4 input_color = texelFetch(resources.source_tx, texel, 0);
  const float4 original = texelFetch(resources.original_tx, texel, 0);
  const float3 input_linear = color_transfer(
      clamp(input_color.rgb, float3(0.0f), float3(1.0f)), true);
  const float3 dlss_linear = color_transfer(
      clamp(color.rgb, float3(0.0f), float3(1.0f)), true);
  frag_out.color = float4(original.rgb + dlss_linear - input_linear, original.a);
}

[[fragment]]
void depth_convert_frag([[resource_table]] const Resources &resources,
                        [[frag_coord]] const float4 frag_co,
                        [[out]] FragOut &frag_out)
{
  const int2 texel = int2(frag_co.xy);
  frag_out.color = float4(texelFetch(resources.depth_tx, texel, 0).x);
}

[[fragment]]
void velocity_convert_frag([[resource_table]] const VelocityResources &resources,
                           [[frag_coord]] const float4 frag_co,
                           [[out]] VelocityFragOut &frag_out)
{
  const int2 texel = int2(frag_co.xy);
  /* EEVEE viewport motion is (previous - current) in UV units. NGX consumes
   * current-to-previous motion in full-resolution pixels. */
  frag_out.velocity = texelFetch(resources.velocity_tx, texel, 0).xy * resources.scale;
}

}  // namespace eevee::dlss5

PipelineGraphic eevee_dlss5_color_convert(eevee::dlss5::fullscreen_vert,
                                           eevee::dlss5::color_convert_frag);
PipelineGraphic eevee_dlss5_hdr_reconstruct(eevee::dlss5::fullscreen_vert,
                                             eevee::dlss5::hdr_reconstruct_frag);
PipelineGraphic eevee_dlss5_depth_convert(eevee::dlss5::fullscreen_vert,
                                           eevee::dlss5::depth_convert_frag);
PipelineGraphic eevee_dlss5_velocity_convert(eevee::dlss5::fullscreen_vert,
                                              eevee::dlss5::velocity_convert_frag);
