/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared deferred light combine helpers used by both the regular combine pass and the NPR pass.
 */

#pragma once

#include "eevee_colorspace_lib.glsl"
#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_renderpass_lib.glsl"
#include "gpu_shader_shared_exponent_lib.glsl"

float3 load_radiance_direct(int2 texel, uchar i)
{
  uint data = 0u;
  switch (i) {
    case 0:
      data = texelFetch(direct_radiance_1_tx, texel, 0).r;
      break;
    case 1:
      data = texelFetch(direct_radiance_2_tx, texel, 0).r;
      break;
    case 2:
      data = texelFetch(direct_radiance_3_tx, texel, 0).r;
      break;
    default:
      break;
  }
  return rgb9e5_decode(data);
}

float3 load_radiance_indirect(int2 texel, uchar i)
{
  switch (i) {
    case 0:
      return texelFetch(indirect_radiance_1_tx, texel, 0).rgb;
    case 1:
      return texelFetch(indirect_radiance_2_tx, texel, 0).rgb;
    case 2:
      return texelFetch(indirect_radiance_3_tx, texel, 0).rgb;
    default:
      return float3(0.0f);
  }
  return float3(0.0f);
}

struct DeferredCombine {
  float3 diffuse_color;
  float3 diffuse_direct;
  float3 diffuse_indirect;
  float3 specular_color;
  float3 specular_direct;
  float3 specular_indirect;
  float3 out_direct;
  float3 out_indirect;
  float3 average_normal;
};

DeferredCombine deferred_combine(int2 texel)
{
  const gbuffer::Layers gbuf = gbuffer::read_layers(texel);
  const uchar closure_count = gbuf.header.closure_len();
  const uint3 bin_indices = gbuf.header.bin_index_per_layer();

  DeferredCombine dc;
  dc.diffuse_color = float3(0.0f);
  dc.diffuse_direct = float3(0.0f);
  dc.diffuse_indirect = float3(0.0f);
  dc.specular_color = float3(0.0f);
  dc.specular_direct = float3(0.0f);
  dc.specular_indirect = float3(0.0f);
  dc.out_direct = float3(0.0f);
  dc.out_indirect = float3(0.0f);
  dc.average_normal = float3(0.0f);

  for (uchar i = 0; i < GBUFFER_LAYER_MAX && i < closure_count; i++) {
    ClosureUndetermined cl = gbuf.layer_get(i);
    if (cl.type == CLOSURE_NONE_ID) {
      continue;
    }

    uchar layer_index = bin_indices[i];
    float3 closure_direct_light = load_radiance_direct(texel, layer_index);
    float3 closure_indirect_light = use_split_radiance ? load_radiance_indirect(texel, layer_index) :
                                                        float3(0.0f);

    dc.average_normal += cl.N * reduce_add(cl.color);

    switch (cl.type) {
      case CLOSURE_BSDF_TRANSLUCENT_ID:
      case CLOSURE_BSSRDF_BURLEY_ID:
      case CLOSURE_BSDF_DIFFUSE_ID:
        dc.diffuse_color += cl.color;
        dc.diffuse_direct += closure_direct_light;
        dc.diffuse_indirect += closure_indirect_light;
        break;
      case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
        dc.specular_color += cl.color;
        dc.specular_direct += closure_direct_light;
        dc.specular_indirect += closure_indirect_light;
        break;
      case CLOSURE_NONE_ID:
        break;
    }

    if ((cl.type == CLOSURE_BSDF_TRANSLUCENT_ID ||
         cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) &&
        (gbuffer::read_thickness(gbuf.header, texel).value() != 0.0f))
    {
      /* We model two transmission events, so the surface color needs to be applied twice. */
      cl.color *= cl.color;
    }

    dc.out_direct += closure_direct_light * cl.color;
    dc.out_indirect += closure_indirect_light * cl.color;
  }

  float normal_len = length(dc.average_normal);
  dc.average_normal = (normal_len < 1e-5f) ? gbuf.surface_N() : (dc.average_normal / normal_len);

  return dc;
}

void deferred_combine_clamp(inout DeferredCombine dc)
{
  float clamp_direct = uniform_buf.clamp.surface_direct;
  float clamp_indirect = uniform_buf.clamp.surface_indirect;

  dc.out_direct = colorspace_brightness_clamp_max(dc.out_direct, clamp_direct);
  dc.out_indirect = colorspace_brightness_clamp_max(dc.out_indirect, clamp_indirect);

  /* Keep the port's post-clamp scaling identical to the main deferred combine shader. */
  dc.out_direct *= uniform_buf.clamp.direct_scale;
  dc.out_indirect *= uniform_buf.clamp.indirect_scale;

  dc.diffuse_direct = colorspace_brightness_clamp_max(dc.diffuse_direct, clamp_direct);
  dc.diffuse_indirect = colorspace_brightness_clamp_max(dc.diffuse_indirect, clamp_indirect);
  dc.specular_direct = colorspace_brightness_clamp_max(dc.specular_direct, clamp_direct);
  dc.specular_indirect = colorspace_brightness_clamp_max(dc.specular_indirect, clamp_indirect);

  dc.diffuse_direct *= uniform_buf.clamp.direct_scale;
  dc.diffuse_indirect *= uniform_buf.clamp.indirect_scale;
  dc.specular_direct *= uniform_buf.clamp.direct_scale;
  dc.specular_indirect *= uniform_buf.clamp.indirect_scale;
}

float4 deferred_combine_final_output(DeferredCombine dc)
{
  float4 out_combined = float4(dc.out_direct + dc.out_indirect, 0.0f);
  out_combined = any(isnan(out_combined)) ? float4(1.0f, 0.0f, 1.0f, 0.0f) : out_combined;
  return colorspace_safe_color(out_combined);
}
