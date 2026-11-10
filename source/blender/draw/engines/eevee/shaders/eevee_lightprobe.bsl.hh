/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_closure.bsl.hh"
#include "eevee_lightprobe_sphere.bsl.hh"
#include "eevee_lightprobe_volume.bsl.hh"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_subsurface_lib.bsl.hh"
#include "eevee_thickness_lib.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_spherical_harmonics.bsl.hh"

namespace eevee {

struct LightprobeRenderData {
  [[resource_table]] LightprobeSphereRenderData spheres;
  [[resource_table]] LightprobeVolumeRenderData volumes;
  [[resource_table]] Sampling sampling;

  /**
   * Return cached light-probe data at P.
   * Ng and V are use for biases.
   */
  LightProbeSample load(float2 screen_texel, float3 P, float3 Ng, float3 V) const
  {
    float noise = random::interleaved_gradient(screen_texel, 0.0f, 0.0f);
    noise = fract(noise + sampling.rng_1D_get(SAMPLING_LIGHTPROBE));

    LightProbeSample result;
    result.volume_irradiance = volumes.sample_probe(sampling, P, V, Ng);
    result.spherical_id = spheres.select_probe(P, noise);
    return result;
  }

  float3 eval_direction(LightProbeSample samp,
                        float3 P,
                        float3 L,
                        float perceptual_roughness) const
  {
    /* Avoid over-blurring diffuse. */
    perceptual_roughness = min(0.6f, perceptual_roughness);
    float lod = lightprobe::sphere::roughness_to_lod(perceptual_roughness);
    float3 radiance_sh = spheres.spherical_sample_normalized_with_parallax(samp, P, L, lod);
    return radiance_sh;
  }

  /* TODO: Port that inside a BSSDF file. */
  float3 eval(const UtilityTexture &util_tx,
              LightProbeSample samp,
              ClosureSubsurface cl,
              float3 /*P*/,
              float3 /*V*/,
              Thickness thickness) const
  {
    float3 sss_profile = subsurface_transmission(util_tx, cl.sss_radius, thickness.value());
    float3 radiance_sh = samp.volume_irradiance.evaluate_lambert(cl.N).rgb;
    radiance_sh += samp.volume_irradiance.evaluate_lambert(-cl.N).rgb * sss_profile;
    return radiance_sh;
  }

  float3 eval(
      LightProbeSample samp, ClosureUndetermined cl, float3 P, float3 V, Thickness thickness) const
  {
    if (closure_is_npr_reflection(cl)) {
      float3x3 frame = closure_reflection_frame(cl);
      float3 Vt = V * frame;
      float2 alpha = bxdf_ggx_anisotropic_axes(cl.data.x, cl.data.y);
      float2 noise = fract(sampling.rng_2D_get(SAMPLING_RAYTRACE_U) +
                           float2(random::interleaved_gradient(P.xy, 0.0f, P.z),
                                  random::interleaved_gradient(P.yz, 1.0f, P.x)));
      float3 radiance = float3(0.0f);
      float weight_sum = 0.0f;
      for (int i = 0; i < 8; i++) {
        float2 random = fract(float2((float(i) + 0.5f) / 8.0f, float(i) * 0.61803398875f) +
                               noise);
        BsdfSample ray = bxdf_ggx_anisotropic_sample(random, Vt, alpha);
        if (ray.pdf > 0.0f) {
          float weight = bxdf_ggx_anisotropic_eval(ray.direction, Vt, alpha).weight;
          /* The distribution is integrated by the directions. Do not blur it a second time or
           * replace a rough anisotropic lobe by directionless volume-probe SH. */
          radiance += spheres.spherical_sample_normalized_with_parallax(
                          samp, P, frame * float3(ray.direction), 0.0f) * weight;
          weight_sum += weight;
        }
      }
      return radiance * safe_rcp(weight_sum);
    }

    LightProbeRay ray = bxdf_lightprobe_ray(cl, P, V, thickness);

    float lod = lightprobe::sphere::roughness_to_lod(ray.perceptual_roughness);
    float fac = lightprobe::sphere::roughness_to_mix_fac(ray.perceptual_roughness);

    float3 radiance_cube = spheres.spherical_sample_normalized_with_parallax(
        samp, P, ray.dominant_direction, lod);
    float3 radiance_sh = samp.volume_irradiance.evaluate_lambert(ray.dominant_direction).rgb;
    return mix(radiance_cube, radiance_sh, fac);
  }
};

}  // namespace eevee
