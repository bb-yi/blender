/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

[[node]]
void valtorgb_opti_constant(
    float fac, float edge, float4 color1, float4 color2, float4 &outcol, float &outalpha)
{
  outcol = (fac > edge) ? color2 : color1;
  outalpha = outcol.a;
}

float oklab_signed_cbrt(float value)
{
  return sign(value) * pow(abs(value) + 1e-10f, 1.0f / 3.0f);
}

float3 linear_rgb_to_oklab(float3 color)
{
  float l = 0.4122214708f * color.r + 0.5363325363f * color.g + 0.0514459929f * color.b;
  float m = 0.2119034982f * color.r + 0.6806995451f * color.g + 0.1073969566f * color.b;
  float s = 0.0883024619f * color.r + 0.2817188376f * color.g + 0.6299787005f * color.b;

  float l_root = oklab_signed_cbrt(l);
  float m_root = oklab_signed_cbrt(m);
  float s_root = oklab_signed_cbrt(s);

  return float3(0.2104542553f * l_root + 0.7936177850f * m_root - 0.0040720468f * s_root,
                1.9779984951f * l_root - 2.4285922050f * m_root + 0.4505937099f * s_root,
                0.0259040371f * l_root + 0.7827717662f * m_root - 0.8086757660f * s_root);
}

float3 oklab_to_linear_rgb(float3 color)
{
  float l_root = color.x + 0.3963377774f * color.y + 0.2158037573f * color.z;
  float m_root = color.x - 0.1055613458f * color.y - 0.0638541728f * color.z;
  float s_root = color.x - 0.0894841775f * color.y - 1.2914855480f * color.z;

  float l = l_root * l_root * l_root;
  float m = m_root * m_root * m_root;
  float s = s_root * s_root * s_root;

  float3 linear_rgb = float3(+4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
                             -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
                             -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s);

  float max_component = max(max(linear_rgb.r, linear_rgb.g), linear_rgb.b);
  if (max_component > 1.0f) {
    linear_rgb /= max_component;
  }

  return max(float3(0.0f), linear_rgb);
}

float4 oklab_mix(float4 color1, float4 color2, float factor)
{
  factor = clamp(factor, 0.0f, 1.0f);
  float3 oklab1 = linear_rgb_to_oklab(color1.rgb);
  float3 oklab2 = linear_rgb_to_oklab(color2.rgb);
  float3 linear_mixed = oklab_to_linear_rgb(mix(oklab1, oklab2, factor));
  float alpha_mixed = mix(color1.a, color2.a, factor);
  return float4(clamp(linear_mixed, 0.0f, 1.0f), alpha_mixed);
}

[[node]]
void oklab_valtorgb_opti_linear(
    float fac, float2 mulbias, float4 color1, float4 color2, float4 &outcol, float &outalpha)
{
  fac = clamp(fac * mulbias.x + mulbias.y, 0.0f, 1.0f);
  outcol = oklab_mix(color1, color2, fac);
  outalpha = outcol.a;
}

[[node]]
void valtorgb_opti_linear(
    float fac, float2 mulbias, float4 color1, float4 color2, float4 &outcol, float &outalpha)
{
  fac = clamp(fac * mulbias.x + mulbias.y, 0.0f, 1.0f);
  outcol = mix(color1, color2, fac);
  outalpha = outcol.a;
}

[[node]]
void valtorgb_opti_ease(
    float fac, float2 mulbias, float4 color1, float4 color2, float4 &outcol, float &outalpha)
{
  fac = clamp(fac * mulbias.x + mulbias.y, 0.0f, 1.0f);
  fac = fac * fac * (3.0f - 2.0f * fac);
  outcol = mix(color1, color2, fac);
  outalpha = outcol.a;
}

[[node]]
void oklab_valtorgb_opti_constant(
    float fac, float edge, float4 color1, float4 color2, float4 &outcol, float &outalpha)
{
  outcol = (fac > edge) ? color2 : color1;
  outalpha = outcol.a;
}

[[node]]
void oklab_valtorgb_opti_ease(
    float fac, float2 mulbias, float4 color1, float4 color2, float4 &outcol, float &outalpha)
{
  fac = clamp(fac * mulbias.x + mulbias.y, 0.0f, 1.0f);
  fac = fac * fac * (3.0f - 2.0f * fac);
  outcol = oklab_mix(color1, color2, fac);
  outalpha = outcol.a;
}

/* Color maps are stored in texture samplers, so ensure that the coordinate evaluates the sampler
 * at the center of the pixels, because samplers are evaluated using linear interpolation. Given
 * the coordinate in the [0, 1] range. */
float compute_color_map_coordinate(float coordinate)
{
  /* Color maps have a fixed width of 257. We offset by the equivalent of half a pixel and scale
   * down such that the normalized coordinate 1.0 corresponds to the center of the last pixel. */
  constexpr float sampler_resolution = 257.0f;
  constexpr float sampler_offset = 0.5f / sampler_resolution;
  constexpr float sampler_scale = 1.0f - (1.0f / sampler_resolution);
  return coordinate * sampler_scale + sampler_offset;
}

[[node]]
void oklab_valtorgb(float fac, sampler1DArray colormap, float layer, float4 &outcol, float &outalpha)
{
  fac = clamp(fac, 0.0f, 1.0f);
  outcol = texture(colormap, float2(compute_color_map_coordinate(fac), layer));
  outalpha = outcol.a;
}

[[node]]
void valtorgb(float fac, sampler1DArray colormap, float layer, float4 &outcol, float &outalpha)
{
  outcol = texture(colormap, float2(compute_color_map_coordinate(fac), layer));
  outalpha = outcol.a;
}

[[node]]
void valtorgb_nearest(
    float fac, sampler1DArray colormap, float layer, float4 &outcol, float &outalpha)
{
  fac = clamp(fac, 0.0f, 1.0f);
  outcol = texelFetch(colormap, int2(fac * (textureSize(colormap, 0).x - 1), layer), 0);
  outalpha = outcol.a;
}
