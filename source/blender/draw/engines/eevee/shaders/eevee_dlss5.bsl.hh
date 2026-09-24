/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_velocity.bsl.hh"
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
  [[push_constant]] int guide_overscan;
  [[push_constant]] int guide_scale;
};

struct FragOut {
  [[frag_color(0)]] float4 color;
};

struct VelocityResources {
  [[sampler(0)]] sampler2D velocity_tx;
  [[sampler(1)]] sampler2DDepth depth_tx;
  [[resource_table]] srt_t<CameraVelocity> camera;
  [[push_constant]] int guide_overscan;
  [[push_constant]] int guide_scale;
};

struct VelocityFragOut {
  [[frag_color(0)]] float2 velocity;
};

struct ColorResources {
  [[sampler(0)]] sampler2D color_tx;
  [[push_constant]] bool inverse;
};

struct HdrReconstructResources {
  [[push_constant]] float resolve_intensity;
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

float3 clamp_ap1(float3 color)
{
  const float3 ap1 = float3(
      dot(float3(0.613097f, 0.339523f, 0.047379f), color),
      dot(float3(0.070194f, 0.916354f, 0.013452f), color),
      dot(float3(0.020616f, 0.109570f, 0.869815f), color));
  return float3(
      dot(float3(1.705051f, -0.621792f, -0.083259f), max(ap1, float3(0.0f))),
      dot(float3(-0.130256f, 1.140805f, -0.010548f), max(ap1, float3(0.0f))),
      dot(float3(-0.024003f, -0.128969f, 1.152972f), max(ap1, float3(0.0f))));
}

float3 cbrt_signed(float3 value)
{
  return sign(value) * pow(abs(value), float3(1.0f / 3.0f));
}

float3 to_oklab(float3 color)
{
  const float3 lms = float3(
      dot(float3(0.4122214708f, 0.5363325363f, 0.0514459929f), color),
      dot(float3(0.2119034985f, 0.6806995451f, 0.1073969566f), color),
      dot(float3(0.0883024619f, 0.2817188376f, 0.6299787005f), color));
  const float3 root = cbrt_signed(lms);
  return float3(
      dot(float3(0.2104542553f, 0.7936177850f, -0.0040720468f), root),
      dot(float3(1.9779984951f, -2.4285922050f, 0.4505937099f), root),
      dot(float3(0.0259040371f, 0.7827717662f, -0.8086757660f), root));
}

float3 from_oklab(float3 lab)
{
  const float3 lms = float3(
      lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z,
      lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z,
      lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z);
  const float3 cubed = lms * lms * lms;
  return float3(
      dot(float3(4.0767416621f, -3.3077115913f, 0.2309699292f), cubed),
      dot(float3(-1.2684380046f, 2.6097574011f, -0.3413193965f), cubed),
      dot(float3(-0.0041960863f, -0.7034186147f, 1.7076147010f), cubed));
}

float3 hue_oklab(float3 incorrect, float3 correct)
{
  float3 incorrect_lab = to_oklab(incorrect);
  const float3 correct_lab = to_oklab(correct);
  const float incorrect_chroma = length(incorrect_lab.yz);
  const float correct_chroma = length(correct_lab.yz);
  incorrect_lab.yz = correct_lab.yz *
                     (correct_chroma == 0.0f ? 1.0f : incorrect_chroma / correct_chroma);
  return clamp_ap1(from_oklab(incorrect_lab));
}

[[fragment]]
void color_convert_frag([[resource_table]] const ColorResources &resources,
                        [[frag_coord]] const float4 frag_co,
                        [[out]] FragOut &frag_out)
{
  const int2 texel = int2(frag_co.xy);
  float4 color = texelFetch(resources.color_tx, texel, 0);
  float3 display = color.rgb;
  if (!resources.inverse) {
    display = max(display, float3(0.0f));
    const float display_luma = dot(display, float3(0.2126f, 0.7152f, 0.0722f));
    if (display_luma > 0.75f) {
      const float rolled = 0.75f +
                           0.25f * (1.0f - exp(-(display_luma - 0.75f) / 0.25f));
      display *= rolled / display_luma;
    }
  }
  color.rgb = clamp(color_transfer(display, resources.inverse),
                    float3(0.0f), float3(1.0f));
  frag_out.color = color;
}

[[fragment]]
void hdr_reconstruct_frag([[resource_table]] const HdrReconstructResources &resources,
                           [[frag_coord]] const float4 frag_co,
                           [[out]] FragOut &frag_out)
{
  const int2 texel = int2(frag_co.xy);
  const float2 output_extent = float2(textureSize(resources.color_tx, 0));
  const float2 uv = (float2(texel) + 0.5f) / output_extent;
  const float4 color = textureLod(resources.color_tx, uv, 0.0f);
  const float4 input_color = textureLod(resources.source_tx, uv, 0.0f);
  const float4 original = textureLod(resources.original_tx, uv, 0.0f);
  /* The referenced feature-18 implementation uses paper white 1.0 and a soft
   * knee. Keep the original HDR frame as the authoritative luminance source. */
  const float norm_scale = 1.0f;
  const float3 input_linear = color_transfer(
      clamp(input_color.rgb, float3(0.0f), float3(1.0f)), true);
  const float3 dlss_linear = color_transfer(
      clamp(color.rgb, float3(0.0f), float3(1.0f)), true);
  const float3 original_normalized = max(original.rgb, float3(0.0f)) / norm_scale;
  const float3 negative = min(original.rgb, float3(0.0f));
  const float original_luma = dot(original_normalized, float3(0.2126f, 0.7152f, 0.0722f));
  const float input_luma = dot(input_linear, float3(0.2126f, 0.7152f, 0.0722f));
  const float model_luma = dot(dlss_linear, float3(0.2126f, 0.7152f, 0.0722f));
  float3 upgraded = original_normalized;
  if (model_luma > 1e-5f) {
    const float ratio = original_luma < input_luma ?
                            original_luma / max(input_luma, 1e-6f) :
                            (model_luma + max(0.0f, original_luma - input_luma)) / model_luma;
    const float transfer = clamp(resources.resolve_intensity, 0.0f, 1.0f);
    upgraded = mix(original_normalized, hue_oklab(dlss_linear * ratio, dlss_linear), transfer);
  }
  const float upgraded_luma = dot(upgraded, float3(0.2126f, 0.7152f, 0.0722f));
  const float luma_ratio = original_luma > 1e-6f ?
                               clamp(upgraded_luma / original_luma, 0.0f, 2.0f) :
                               1.0f;
  const float3 reconstructed = max(
      mix(original_normalized * luma_ratio, upgraded, 1.0f) * norm_scale, float3(0.0f));
  frag_out.color = float4(reconstructed + negative, original.a);
}

[[fragment]]
void depth_convert_frag([[resource_table]] const Resources &resources,
                        [[frag_coord]] const float4 frag_co,
                        [[out]] FragOut &frag_out)
{
  const int2 texel = int2(frag_co.xy);
  const int2 guide_texel = texel / resources.guide_scale + resources.guide_overscan;
  frag_out.color = float4(texelFetch(resources.depth_tx, guide_texel, 0).x);
}

[[fragment]]
void velocity_convert_frag([[resource_table]] const VelocityResources &resources,
                           [[resource_table]] const draw::View &views,
                           [[frag_coord]] const float4 frag_co,
                           [[out]] VelocityFragOut &frag_out)
{
  const int2 texel = int2(frag_co.xy);
  const int2 guide_texel = texel / resources.guide_scale + resources.guide_overscan;
  const float depth = reverse_z::read(texelFetch(resources.depth_tx, guide_texel, 0).r);
  [[resource_table]] const CameraVelocity &camera = resources.camera;
  const float2 motion = camera.resolve(views, resources.velocity_tx, guide_texel, depth).xy;
  /* EEVEE resolves previous-current UV motion. NR uses current-previous pixels.
   * Include overscan in the UV-to-pixel scale, then undo the EEVEE render scale. */
  frag_out.velocity = -motion * float2(textureSize(resources.velocity_tx, 0)) *
                      float(resources.guide_scale);
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
