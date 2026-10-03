/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_view.bsl.hh"
#include "eevee_bxdf.bsl.hh"
#include "eevee_gbuffer_read.bsl.hh"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_uniform.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_matrix_construct_lib.glsl"

namespace eevee {

/* Returns view-space ray. */
BsdfSample ray_generate_direction(float2 noise,
                                  ClosureUndetermined cl,
                                  float3 V,
                                  Thickness thickness)
{
  if (closure_is_npr_reflection(cl)) {
    float3x3 frame = closure_reflection_frame(cl);
    BsdfSample samp = bxdf_ggx_anisotropic_sample(
        noise, V * frame, bxdf_ggx_anisotropic_axes(cl.data.x, cl.data.y));
    samp.direction = frame * float3(samp.direction);
    return samp;
  }
  float3 random_point_on_cylinder = sample_cylinder(noise);
  /* Bias the rays so we never get really high energy rays almost parallel to the surface. */
  constexpr float rng_bias = 0.08f;
  /* When modeling object thickness as a sphere, the outgoing rays are distributed uniformly
   * over the sphere. We don't want the RAY_BIAS in this case. */
  if (cl.type != CLOSURE_BSDF_TRANSLUCENT_ID || thickness.mode() == ThicknessMode::Slab) {
    random_point_on_cylinder.x = 1.0f - random_point_on_cylinder.x * (1.0f - rng_bias);
  }

  switch (cl.type) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      bxdf_ggx_context_amend_transmission(cl, V, thickness);
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
    case CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID:
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
      break;
    case CLOSURE_NONE_ID:
      assert(false);
      break;
  }

  float3x3 tangent_to_world = from_up_axis(cl.N);

  BsdfSample samp;
  samp.pdf = 0.0f;
  samp.direction = float3(0.0f);
  switch (cl.type) {
    case CLOSURE_BSDF_TRANSLUCENT_ID:
      samp = bxdf_translucent_sample(random_point_on_cylinder, thickness);
      break;
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
      samp = bxdf_diffuse_sample(random_point_on_cylinder);
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID: {
      samp = bxdf_ggx_sample_reflection(random_point_on_cylinder,
                                        V * tangent_to_world,
                                        square(to_closure_reflection(cl).roughness),
                                        true);
      break;
    }
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID: {
      samp = bxdf_ggx_sample_refraction(random_point_on_cylinder,
                                        V * tangent_to_world,
                                        square(to_closure_refraction(cl).roughness),
                                        to_closure_refraction(cl).ior,
                                        thickness,
                                        true);
      break;
    }
    case CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID: {
      samp = bxdf_ggx_sample_reflection(random_point_on_cylinder,
                                        V * tangent_to_world,
                                        square(to_closure_thin_refraction(cl).roughness),
                                        true);
      samp.direction.z = -samp.direction.z;
      break;
    }
    case CLOSURE_NONE_ID:
      assert(false);
      break;
  }
  samp.direction = tangent_to_world * float3(samp.direction);

  return samp;
}

}  // namespace eevee

namespace eevee::raytracing {

struct RayGenerate {
  [[specialization_constant(0)]] int closure_index;

  [[storage(4, read)]] const uint (&tiles_coord_buf)[];
  [[image(0, write, SFLOAT_16_16_16_16)]] image2D out_ray_data_img;
};

/**
 * Generate Ray direction along with other data that are then used
 * by the next pass to trace the rays.
 */
[[compute, local_size(RAYTRACE_GROUP_SIZE, RAYTRACE_GROUP_SIZE)]]
void generate_rays([[resource_table]] RayGenerate &srt,
                   [[resource_table]] const Uniform &uni,
                   [[resource_table]] const Sampling &sampling,
                   [[resource_table]] const UtilityTexture &util_tx,
                   [[resource_table]] const draw::View &views,
                   [[resource_table]] const gbuffer::Reader &reader,
                   [[work_group_id]] const uint3 group_id,
                   [[local_invocation_id]] const uint3 local_id)
{
  constexpr uint tile_size = RAYTRACE_GROUP_SIZE;
  uint2 tile_coord = unpackUvec2x16(srt.tiles_coord_buf[group_id.x]);
  int2 texel = int2(local_id.xy + tile_coord * tile_size);

  if (any(greaterThanEqual(texel, imageSize(srt.out_ray_data_img)))) {
    return;
  }

  int2 texel_fullres = texel * uni.raytrace_buf.trace_pixel_scale +
                       uni.raytrace_buf.trace_pixel_offset;

  if (any(greaterThanEqual(texel_fullres, uni.raytrace_buf.full_resolution))) {
    imageStore(srt.out_ray_data_img, texel, float4(0.0f));
    return;
  }

  gbuffer::Header gbuf_header = reader.read_header(texel_fullres);
  ClosureUndetermined closure = reader.read_bin(texel_fullres, srt.closure_index);

  if (closure.type == CLOSURE_NONE_ID ||
      uni.raytrace_buf.skip_hardware_phase(
          closure.type == CLOSURE_BSDF_DIFFUSE_ID || closure.type == CLOSURE_BSSRDF_BURLEY_ID,
          closure.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID,
          closure.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID)) {
    imageStore(srt.out_ray_data_img, texel, float4(0.0f));
    return;
  }

  const ViewMatrices view = views.get(0);

  float2 uv = (float2(texel_fullres) + 0.5f) / float2(textureSize(reader.gbuf_header_tx, 0).xy);
  float3 P = view.point_screen_to_world(float3(uv, 0.5f));
  float3 V = view.world_incident_vector(P);
  const bool hardware = uni.raytrace_buf.use_hardware_tracing_method;
  const bool diffuse_like = closure.type == CLOSURE_BSDF_DIFFUSE_ID ||
                            closure.type == CLOSURE_BSSRDF_BURLEY_ID;
  if (hardware && diffuse_like &&
      uni.raytrace_buf.hardware_trace_phase != HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR)
  {
    /* Pinned Nuru: keep GI rays in the visible geometric hemisphere. Shading normals can
     * otherwise aim through a back-facing shell; retain its small crevice-normal blend. */
    const gbuffer::Layers gbuf = reader.read_layers(texel_fullres);
    float3 Ng = gbuf.header.geometry_normal(gbuf.surface_N());
    if (dot(Ng, V) < 0.0f) {
      Ng = -Ng;
    }
    if (dot(Ng, Ng) > 1.0e-10f) {
      const float3 Ns = gbuf.surface_N();
      const float align = saturate(dot(Ng, Ns));
      const float crevice = saturate((0.68f - align) / 0.68f);
      closure.N = normalize(mix(Ng, Ns, crevice * 0.4f));
    }
  }

  float2 noise;
  if (hardware && diffuse_like && (uni.raytrace_buf.hardware_feature_mask & 1u) != 0u) {
    const float2 ign_noise = interleaved_gradient_noise(
        float2(texel_fullres) + 0.5f,
        float2(5.0f + float(srt.closure_index) * 13.0f,
               11.0f + float(srt.closure_index) * 17.0f),
        sampling.rng_2D_get(SAMPLING_RAYTRACE_W));
    const float2 blue_noise = util_tx.fetch(float2(texel_fullres), UTIL_BLUE_NOISE_LAYER).rg;
    noise = fract(float2(ign_noise.x, blue_noise.y) + sampling.rng_2D_get(SAMPLING_RAYTRACE_U));
  }
  else if (hardware) {
    const int2 noise_offset = int2(sampling.rng_2D_get(SAMPLING_RAYTRACE_V) * float(UTIL_TEX_SIZE));
    const int2 noise_texel = int2(texel_fullres.x * 73 + texel_fullres.y * 19,
                                  texel_fullres.x * 29 + texel_fullres.y * 71) + noise_offset;
    const float2 blue_noise = util_tx.fetch(float2(noise_texel), UTIL_BLUE_NOISE_LAYER).rg;
    const float2 ig_noise = interleaved_gradient_noise(
        float2(texel_fullres) + 0.5f, float2(5.0f, 11.0f), sampling.rng_2D_get(SAMPLING_RAYTRACE_W));
    noise = fract(blue_noise + ig_noise + sampling.rng_2D_get(SAMPLING_RAYTRACE_U));
  }
  else {
    /* Keep the existing Eevee/NPR sequence when hardware tracing is disabled. */
    noise = util_tx.fetch(float2(texel), UTIL_BLUE_NOISE_LAYER).rg;
    noise = fract(noise + sampling.rng_2D_get(SAMPLING_RAYTRACE_U));
  }

  Thickness thickness = reader.read_thickness(gbuf_header, texel_fullres);

  BsdfSample samp = ray_generate_direction(noise.xy, closure, V, thickness);

  /* Store inverse pdf to speedup denoising.
   * Limit to the smallest non-0 value that the format can encode.
   * Strangely it does not correspond to the IEEE spec. */
  float inv_pdf = (samp.pdf == 0.0f) ? 0.0f : max(6e-8f, 1.0f / samp.pdf);
  imageStoreFast(srt.out_ray_data_img, texel, float4(samp.direction, inv_pdf));
}

}  // namespace eevee::raytracing

PipelineCompute eevee_ray_generate(eevee::raytracing::generate_rays);
