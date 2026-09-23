/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_bxdf.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"

/* Return the apparent roughness of a closure compared to a GGX reflection lobe. */
float closure_apparent_roughness_get(ClosureUndetermined cl)
{
  switch (cl.type) {
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
      return bxdf_diffuse_perceived_roughness();
    case CLOSURE_BSDF_TRANSLUCENT_ID:
      return bxdf_translucent_perceived_roughness();
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      return bxdf_ggx_perceived_roughness_reflection(to_closure_reflection(cl).roughness);
    case CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID:
      return bxdf_ggx_perceived_roughness_reflection(to_closure_thin_refraction(cl).roughness);
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      return bxdf_ggx_perceived_roughness_transmission(to_closure_refraction(cl).roughness,
                                                       to_closure_refraction(cl).ior);
    case CLOSURE_NONE_ID:
      return 0.0f;
  }
  return 0.0f;
}

float closure_evaluate_pdf(ClosureUndetermined cl, float3 L, float3 V, Thickness thickness)
{
  if (closure_is_npr_reflection(cl)) {
    float3x3 frame = closure_reflection_frame(cl);
    return bxdf_ggx_anisotropic_eval(
               L * frame, V * frame, bxdf_ggx_anisotropic_axes(cl.data.x, cl.data.y)).pdf;
  }
  switch (cl.type) {
    case CLOSURE_BSDF_TRANSLUCENT_ID:
      return bxdf_translucent_eval(cl.N, L, thickness).pdf;
    case CLOSURE_BSSRDF_BURLEY_ID:
      /* TODO(fclem): Sampled BSSDF. */
      return bxdf_diffuse_eval(cl.N, L).pdf;
    case CLOSURE_BSDF_DIFFUSE_ID:
      return bxdf_diffuse_eval(cl.N, L).pdf;
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID: {
      ClosureReflection cl_ = to_closure_reflection(cl);
      float roughness_sq = square(cl_.roughness);
      return bxdf_ggx_eval_reflection(cl.N, L, V, roughness_sq, true).pdf;
    }
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID: {
      ClosureRefraction cl_ = to_closure_refraction(cl);
      float roughness_sq = square(cl_.roughness);
      return bxdf_ggx_eval_refraction(cl.N, L, V, roughness_sq, cl_.ior, thickness, true).pdf;
    }
    case CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID: {
      ClosureThinRefraction cl_ = to_closure_thin_refraction(cl);
      float roughness_sq = square(cl_.roughness);
      return bxdf_ggx_eval_reflection(-cl.N, L, reflect(V, cl.N), roughness_sq, true).pdf;
    }
    case CLOSURE_NONE_ID:
      break;
  }
  assert(false);
  return 0.0f;
}

/* Conservative filtering / ray-cone width: never blur away the narrow anisotropic axis. */
float closure_filter_roughness_get(ClosureUndetermined cl)
{
  if (closure_is_npr_reflection(cl)) {
    float2 alpha = bxdf_ggx_anisotropic_axes(cl.data.x, cl.data.y);
    return sqrt(min(alpha.x, alpha.y));
  }
  return closure_apparent_roughness_get(cl);
}

/* Select the estimator by material semantics, not anisotropy amount. Otherwise a tiny nonzero
 * anisotropy would switch log/linear estimators and create a discontinuous reflection brightness.
 * Native BSDFs, including the NPR node's native coat layer, retain their existing filter. */
bool closure_uses_linear_reflection_filter(ClosureUndetermined cl)
{
  return closure_is_npr_reflection(cl);
}

float closure_anisotropic_similarity(ClosureUndetermined center, ClosureUndetermined other)
{
  bool center_anisotropic = closure_is_npr_reflection(center);
  bool other_anisotropic = closure_is_npr_reflection(other);
  if (!center_anisotropic && !other_anisotropic) {
    return 1.0f;
  }
  if (center_anisotropic != other_anisotropic) {
    return 0.0f;
  }
  float alignment = abs(dot(closure_reflection_tangent(center), closure_reflection_tangent(other)));
  float axis_weight = smoothstep(0.9659f, 0.9962f, alignment);
  axis_weight = mix(1.0f, axis_weight, saturate(max(center.data.y, other.data.y)));
  float roughness_weight = saturate(1.0f - abs(center.data.x - other.data.x) * 20.0f);
  float anisotropy_weight = saturate(1.0f - abs(center.data.y - other.data.y) * 10.0f);
  return axis_weight * roughness_weight * anisotropy_weight;
}

/* Compact NPR reflection history metadata: tangent (11+11), roughness (5), anisotropy (5).
 * Zero is reserved for native closures. Tangent rejection vanishes at isotropy, without switching
 * the sampling/filtering estimator. This is metadata, not a random material hash. */
uint closure_anisotropic_history_signature(ClosureUndetermined cl)
{
  if (!closure_is_npr_reflection(cl)) {
    return 0u;
  }
  uint2 tangent = uint2(floor(saturate(cl.data.zw) * 2047.0f + 0.5f));
  uint roughness = uint(floor(saturate(cl.data.x) * 31.0f + 0.5f));
  uint anisotropy = uint(floor(saturate(cl.data.y) * 31.0f + 0.5f));
  return max(1u, tangent.x | (tangent.y << 11u) | (roughness << 22u) | (anisotropy << 27u));
}

bool closure_anisotropic_history_matches(uint current, uint history)
{
  if (current == 0u || history == 0u) {
    return current == history;
  }
  float3 T_current = closure_tangent_unpack(
      float2(current & 2047u, (current >> 11u) & 2047u) / 2047.0f);
  float3 T_history = closure_tangent_unpack(
      float2(history & 2047u, (history >> 11u) & 2047u) / 2047.0f);
  int roughness_delta = abs(int((current >> 22u) & 31u) - int((history >> 22u) & 31u));
  int anisotropy_delta = abs(int(current >> 27u) - int(history >> 27u));
  float direction_weight = float(max(current >> 27u, history >> 27u)) / 31.0f;
  return (1.0f - abs(dot(T_current, T_history))) * direction_weight < 0.000343f && roughness_delta <= 1 &&
         anisotropy_delta <= 1;
}

LightProbeRay bxdf_lightprobe_ray(ClosureUndetermined cl,
                                  float3 /*P*/,
                                  float3 V,
                                  Thickness thickness)
{
  switch (cl.type) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      bxdf_ggx_context_amend_transmission(cl, V, thickness);
      break;
    case CLOSURE_NONE_ID:
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
    case CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID:
      break;
  }

  switch (cl.type) {
    case CLOSURE_BSDF_TRANSLUCENT_ID:
      return bxdf_translucent_lightprobe(cl.N, thickness);
    case CLOSURE_NONE_ID:
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
      return bxdf_diffuse_lightprobe(cl.N);
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      return bxdf_ggx_lightprobe_reflection(to_closure_reflection(cl), V);
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      return bxdf_ggx_lightprobe_transmission(to_closure_refraction(cl), V, thickness);
    case CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID:
      return bxdf_ggx_lightprobe_thin_glass_transmission(to_closure_thin_refraction(cl), V);
  }

  assert(false);
  return {};
}

ClosureLight closure_light_new_ex([[resource_table]] const UtilityTexture &util_tx,
                                  ClosureUndetermined cl,
                                  float3 V,
                                  Thickness thickness,
                                  const bool is_transmission)
{
  ClosureLight cl_light;
  if (is_transmission) {
    /* Transmission. */
    switch (cl.type) {
      case CLOSURE_BSSRDF_BURLEY_ID:
        /* If the `thickness / sss_radius` ratio is near 0, this transmission term should converge
         * to a uniform term like the translucent BSDF. But we need to find what to do in other
         * cases. For now, approximate the transmission term as just back-facing. */
        cl_light = bxdf_translucent_light(cl, V, Thickness::zero());
        break;
      case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
        cl_light = bxdf_ggx_light_transmission(util_tx, to_closure_refraction(cl), V, thickness);
        break;
      case CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID:
        cl_light = bxdf_ggx_light_thin_glass_transmission(
            util_tx, to_closure_thin_refraction(cl), V);
        break;
      case CLOSURE_BSDF_TRANSLUCENT_ID:
      /* Defaults to avoid UB. */
      case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      case CLOSURE_BSDF_DIFFUSE_ID:
      case CLOSURE_NONE_ID:
        cl_light = bxdf_translucent_light(cl, V, thickness);
        break;
    }
  }
  else {
    /* Reflection. */
    switch (cl.type) {
      case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
        cl_light = bxdf_ggx_light_reflection(util_tx, to_closure_reflection(cl), V);
        break;
      case CLOSURE_BSSRDF_BURLEY_ID:
      case CLOSURE_BSDF_DIFFUSE_ID:
      /* Defaults to avoid UB. */
      case CLOSURE_BSDF_TRANSLUCENT_ID:
      case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      case CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID:
      case CLOSURE_NONE_ID:
        cl_light = bxdf_diffuse_light(cl);
        break;
    }
  }
  cl_light.light_shadowed = float3(0.0f);
  cl_light.light_unshadowed = float3(0.0f);
  cl_light.shadow_N = cl.N;
  cl_light.npr_policy = cl.npr.light_policy;
  return cl_light;
}

ClosureLight closure_light_new([[resource_table]] const UtilityTexture &util_tx,
                               ClosureUndetermined cl,
                               float3 V,
                               Thickness thickness)
{
  return closure_light_new_ex(util_tx, cl, V, thickness, true);
}

ClosureLight closure_light_new([[resource_table]] const UtilityTexture &util_tx,
                               ClosureUndetermined cl,
                               float3 V)
{
  return closure_light_new_ex(util_tx, cl, V, Thickness::zero(), false);
}
