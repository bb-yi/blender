/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

float3 calc_barycentric_distances(float3 pos0, float3 pos1, float3 pos2)
{
  float3 edge21 = pos2 - pos1;
  float3 edge10 = pos1 - pos0;
  float3 edge02 = pos0 - pos2;
  float3 d21 = normalize(edge21);
  float3 d10 = normalize(edge10);
  float3 d02 = normalize(edge02);

  float3 dists;
  float d = dot(d21, edge02);
  dists.x = sqrt(dot(edge02, edge02) - d * d);
  d = dot(d02, edge10);
  dists.y = sqrt(dot(edge10, edge10) - d * d);
  d = dot(d10, edge21);
  dists.z = sqrt(dot(edge21, edge21) - d * d);
  return dists;
}

float2 calc_barycentric_co(int vertid)
{
  float2 bary;
  bary.x = float((vertid % 3) == 0);
  bary.y = float((vertid % 3) == 1);
  return bary;
}

struct TextureHandle {
  uint type;
  int index;
};

/* Scene Color is a filter-domain handle. Keep it outside the existing NPR/world ranges. */
#define TEX_HANDLE_SCENE 30u
#define TEX_HANDLE_FILTER_GRAPH_INPUT 31u
#define TEX_HANDLE_FILTER_GRAPH_TEXTURE 32u

#define TEXTURE_HANDLE_DEFAULT TextureHandle(0u, 0)

#if defined(NPR_SHADER) || defined(MAT_FILTER)
float4 TextureHandle_eval(TextureHandle tex, float2 offset, bool texel_offset);
float4 TextureHandle_eval(TextureHandle tex);
float4 TextureHandle_eval_uv(TextureHandle tex, float2 uv);
#  if defined(MAT_FILTER)
bool TextureHandle_stores_transmittance_alpha(TextureHandle tex);
bool TextureHandle_is_scene_depth(TextureHandle tex);
#  endif
#else
float4 TextureHandle_eval(TextureHandle tex, float2 offset, bool texel_offset)
{
  return float4(0.0f);
}

float4 TextureHandle_eval(TextureHandle tex)
{
  return TextureHandle_eval(tex, float2(0.0f), false);
}

float4 TextureHandle_eval_uv(TextureHandle tex, float2 uv)
{
  return TextureHandle_eval(tex, uv, false);
}
#endif

/* Assumes GPU_VEC4 is color data, special case that needs luminance coefficients from OCIO. */
#define float_from_float4(v, luminance_coefficients) dot(v.rgb, luminance_coefficients)
#define float_from_float3(v) ((v.r + v.g + v.b) * (1.0f / 3.0f))
#define float_from_float2(v) ((v.x + v.y) * (1.0f / 2.0f))
#define float_from_TextureHandle(t, luminance_coefficients) \
  float_from_float4(TextureHandle_eval(t), luminance_coefficients)

#define float2_from_float4(v) v.xy
#define float2_from_float3(v) v.xy
#define float2_from_float(v) float2(v)
#define float2_from_TextureHandle(t) TextureHandle_eval(t).xy

#define float3_from_float4(v) v.rgb
#define float3_from_float2(v) float3(v.xy, 0.0f)
#define float3_from_float(v) float3(v)
#define float3_from_TextureHandle(t) TextureHandle_eval(t).rgb

#define float4_from_float3(v) float4(v, 1.0f)
#define float4_from_float2(v) float4(v.xy, 0.0f, 1.0f)
#define float4_from_float(v) float4(float3(v), 1.0f)
#define float4_from_TextureHandle(t) TextureHandle_eval(t)

#ifdef MAT_BAKE_COLOR
#  define FrontFacing true
#elif defined(GPU_FRAGMENT_SHADER)
#  define FrontFacing gl_FrontFacing
#else
#  define FrontFacing true
#endif

enum ClosureType : uchar {
  CLOSURE_NONE_ID = 0u,
  /* Diffuse */
  CLOSURE_BSDF_DIFFUSE_ID = 1u,
  // CLOSURE_BSDF_OREN_NAYAR_ID = 2u,   /* TODO */
  // CLOSURE_BSDF_SHEEN_ID = 4u,        /* TODO */
  // CLOSURE_BSDF_DIFFUSE_TOON_ID = 5u, /* TODO */
  CLOSURE_BSDF_TRANSLUCENT_ID = 6u,

  /* Glossy */
  CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID = 7u,
  // CLOSURE_BSDF_ASHIKHMIN_SHIRLEY_ID = 8u, /* TODO */
  // CLOSURE_BSDF_ASHIKHMIN_VELVET_ID = 9u,  /* TODO */
  // CLOSURE_BSDF_GLOSSY_TOON_ID = 10u,      /* TODO */
  // CLOSURE_BSDF_HAIR_REFLECTION_ID = 11u,  /* TODO */

  /* Transmission */
  CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID = 12u,
  CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID = 13u,

  /* Glass */
  // CLOSURE_BSDF_HAIR_HUANG_ID = 14u, /* TODO */

  /* BSSRDF */
  CLOSURE_BSSRDF_BURLEY_ID = 15u,
};

/* Precomputed direct lighting keeps the physical albedo available to indirect lighting and SSS.
 * The additive term is already colored; it must not be multiplied by the albedo again. */
struct ClosureNPRLightPolicy {
  uint code;
  float strength;
  float direct_gain;
};

ClosureNPRLightPolicy closure_npr_light_policy_default()
{
  ClosureNPRLightPolicy policy;
  policy.code = 0u;
  policy.strength = 1.0f;
  policy.direct_gain = 1.0f;
  return policy;
}

/* controls = (shadow type, quality, samples, numeric light group).
 * Bit 31 distinguishes an explicit No Shadows policy from ordinary native shading. */
ClosureNPRLightPolicy closure_npr_light_policy(float4 controls, float strength, float direct_gain)
{
  ClosureNPRLightPolicy policy;
  uint group = controls.w < 0.0f ? 16383u : uint(clamp(controls.w, 0.0f, 9999.0f));
  policy.code = (1u << 31u) | uint(clamp(controls.x, 0.0f, 3.0f)) |
                (uint(clamp(controls.y, 1.0f, 2.0f)) << 2u) |
                (uint(clamp(controls.z, 1.0f, 32.0f)) << 4u) |
                (group << 10u);
  policy.strength = clamp(strength, 0.0f, 1.0f);
  policy.direct_gain = max(direct_gain, 0.0f);
  return policy;
}

ClosureNPRLightPolicy closure_npr_light_policy(float4 controls, float strength)
{
  return closure_npr_light_policy(controls, strength, 1.0f);
}

bool closure_npr_policy_enabled(ClosureNPRLightPolicy policy)
{
  return (policy.code & (1u << 31u)) != 0u;
}
uint closure_npr_policy_shadow_type(ClosureNPRLightPolicy policy)
{
  return policy.code & 3u;
}
uint closure_npr_policy_quality(ClosureNPRLightPolicy policy)
{
  return (policy.code >> 2u) & 3u;
}
uint closure_npr_policy_samples(ClosureNPRLightPolicy policy)
{
  return (policy.code >> 4u) & 63u;
}
uint closure_npr_policy_group(ClosureNPRLightPolicy policy)
{
  return (policy.code >> 10u) & 16383u;
}

struct ClosureNPRDirect {
  packed_float3 multiplier;
  packed_float3 additive;
  float indirect_weight;
  ClosureNPRLightPolicy light_policy;
  bool enabled;
};

ClosureNPRDirect closure_npr_direct_default()
{
  ClosureNPRDirect npr = {};
  npr.indirect_weight = 1.0f;
  npr.light_policy = closure_npr_light_policy_default();
  return npr;
}

struct ClosureUndetermined {
  packed_float3 color;
  float weight;
  packed_float3 N;
  ClosureType type;
  /* Additional data different for each closure type. */
  packed_float4 data;
  ClosureNPRDirect npr;
};

bool closure_has_transmission(const ClosureType closure)
{
  return closure == CLOSURE_BSDF_TRANSLUCENT_ID ||
         closure == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID ||
         closure == CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID;
}

ClosureUndetermined closure_new(ClosureType type)
{
  ClosureUndetermined cl = {};
  cl.type = type;
  cl.npr = closure_npr_direct_default();
  return cl;
}

bool closure_is_anisotropic(ClosureUndetermined cl)
{
  return cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID && cl.data.y > 0.0f;
}

bool closure_is_npr_reflection(ClosureUndetermined cl)
{
  return cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID && cl.npr.enabled;
}

float3 closure_tangent_orthogonalize(float3 N, float3 T)
{
  T -= N * dot(N, T);
  if (dot(T, T) < 1e-12f) {
    float3 axis = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    T = cross(axis, N);
  }
  return normalize(T);
}

/* World-space octahedral tangent, independent of the normal's packing. */
float2 closure_tangent_pack(float3 T)
{
  T /= abs(T.x) + abs(T.y) + abs(T.z);
  float2 signs = float2(T.x < 0.0f ? -1.0f : 1.0f, T.y < 0.0f ? -1.0f : 1.0f);
  float2 oct = T.z >= 0.0f ? T.xy : (1.0f - abs(T.yx)) * signs;
  return oct * 0.5f + 0.5f;
}

float3 closure_tangent_unpack(float2 oct)
{
  oct = oct * 2.0f - 1.0f;
  float3 T = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
  float fold = clamp(-T.z, 0.0f, 1.0f);
  T.x += T.x >= 0.0f ? -fold : fold;
  T.y += T.y >= 0.0f ? -fold : fold;
  return normalize(T);
}

float3 closure_reflection_tangent(ClosureUndetermined cl)
{
  return closure_tangent_orthogonalize(cl.N, closure_tangent_unpack(cl.data.zw));
}

float3x3 closure_reflection_frame(ClosureUndetermined cl)
{
  float3 T = closure_reflection_tangent(cl);
  return float3x3(T, cross(cl.N, T), cl.N);
}

/* An axis, not an oriented vector: T and -T describe the same anisotropic lobe. This fits in a
 * single 10-bit GBuffer channel; the fourth data channel has only two bits. */
float closure_tangent_angle_pack(ClosureUndetermined cl)
{
  float3 T0 = closure_tangent_orthogonalize(cl.N, float3(0.0f));
  float3 B0 = cross(cl.N, T0);
  float3 T = closure_reflection_tangent(cl);
  return fract(atan(dot(T, B0), dot(T, T0)) / 3.141592653589793f + 1.0f);
}

float2 closure_tangent_angle_unpack(float3 N, float angle)
{
  float3 T0 = closure_tangent_orthogonalize(N, float3(0.0f));
  float3 B0 = cross(N, T0);
  angle *= 3.141592653589793f;
  return closure_tangent_pack(cos(angle) * T0 + sin(angle) * B0);
}

struct ClosureOcclusion {
  packed_float3 N;
};

struct ClosureDiffuse {
  packed_float3 color;
  float weight;
  packed_float3 N;
};

struct ClosureSubsurface {
  packed_float3 color;
  float weight;
  packed_float3 N;
  packed_float3 sss_radius;
};

struct ClosureTranslucent {
  packed_float3 color;
  float weight;
  packed_float3 N;
};

struct ClosureReflection {
  packed_float3 color;
  float weight;
  packed_float3 N;
  float roughness;
};

struct ClosureRefraction {
  packed_float3 color;
  float weight;
  packed_float3 N;
  float roughness;
  float ior;
};

struct ClosureHair {
  packed_float3 color;
  float weight;
  packed_float3 T;
  float offset;
  packed_float2 roughness;
};

struct ClosureVolumeScatter {
  packed_float3 scattering;
  float weight;
  float anisotropy;
};

struct ClosureVolumeAbsorption {
  packed_float3 absorption;
  float weight;
};

struct ClosureEmission {
  packed_float3 emission;
  float weight;
};

struct ClosureTransparency {
  packed_float3 transmittance;
  float weight;
  float holdout;
};

struct ClosureThinRefraction {
  packed_float3 color;
  float weight;
  packed_float3 N;
  float roughness;
};

ClosureDiffuse to_closure_diffuse(ClosureUndetermined cl)
{
  ClosureDiffuse closure;
  closure.N = cl.N;
  closure.color = cl.color;
  return closure;
}

ClosureSubsurface to_closure_subsurface(ClosureUndetermined cl)
{
  ClosureSubsurface closure;
  closure.N = cl.N;
  closure.color = cl.color;
  closure.sss_radius = cl.data.xyz;
  return closure;
}

ClosureTranslucent to_closure_translucent(ClosureUndetermined cl)
{
  ClosureTranslucent closure;
  closure.N = cl.N;
  closure.color = cl.color;
  return closure;
}

ClosureReflection to_closure_reflection(ClosureUndetermined cl)
{
  ClosureReflection closure;
  closure.N = cl.N;
  closure.color = cl.color;
  closure.roughness = cl.data.x;
  return closure;
}

ClosureRefraction to_closure_refraction(ClosureUndetermined cl)
{
  ClosureRefraction closure;
  closure.N = cl.N;
  closure.color = cl.color;
  closure.roughness = cl.data.x;
  closure.ior = cl.data.y;
  return closure;
}

ClosureThinRefraction to_closure_thin_refraction(ClosureUndetermined cl)
{
  ClosureThinRefraction closure;
  closure.N = cl.N;
  closure.color = cl.color;
  closure.roughness = cl.data.x;
  return closure;
}

struct GlobalData {
  /** World position. */
  packed_float3 P;
  /** Surface Normal. Normalized, overridden by bump displacement. */
  packed_float3 N;
  /** Raw interpolated normal (non-normalized) data. */
  packed_float3 Ni;
  /** Geometric Normal. */
  packed_float3 Ng;
  /** Curve Tangent Space. */
  packed_float3 curve_T, curve_B, curve_N;
  /** Barycentric coordinates. */
  packed_float2 barycentric_coords;
  packed_float3 barycentric_dists;
  /** Hair thickness in world space. */
  float hair_diameter;
  /** Index of the strand for per strand effects. */
  int hair_strand_id;
  /** Ray properties (approximation). */
  float ray_depth;
  float ray_length;
  uchar ray_type;
  /** Is hair. */
  bool is_strand;
};

GlobalData g_data;

#ifndef GPU_FRAGMENT_SHADER
/* Stubs. */

#  define dF_impl(a) (float3(0.0f))
#  define dF_branch(a, b, c) (c = float2(0.0f))
#  define dF_branch_incomplete(a, b, c) (c = float2(0.0f))

#elif defined(GPU_FAST_DERIVATIVE) /* TODO(@fclem): User Option? */
/* Fast derivatives */
float3 dF_impl(float3 v)
{
  return float3(0.0f);
}

void dF_branch(float fn, float2 &result)
{
  /* NOTE: this function is currently unused, once it is used we need to check if
   * `g_derivative_filter_width` needs to be applied. */
  result.x = gpu_dfdx(fn) * derivative_scale_get();
  result.y = gpu_dfdy(fn) * derivative_scale_get();
}

#else

/* Offset of coordinates for evaluating bump node. Unit in pixel. */
float g_derivative_filter_width = 0.0f;
/* Precise derivatives */
int g_derivative_flag = 0;

float3 dF_impl(float3 v)
{
  if (g_derivative_flag > 0) {
    return gpu_dfdx(v) * g_derivative_filter_width;
  }
  else if (g_derivative_flag < 0) {
    return gpu_dfdy(v) * g_derivative_filter_width;
  }
  return float3(0.0f);
}

#  define dF_branch(fn, filter_width, result) \
    if (true) { \
      g_derivative_filter_width = filter_width * derivative_scale_get(); \
      g_derivative_flag = 1; \
      result.x = (fn); \
      g_derivative_flag = -1; \
      result.y = (fn); \
      g_derivative_flag = 0; \
      result -= float2((fn)); \
    }

/* Used when the non-offset value is already computed elsewhere */
#  define dF_branch_incomplete(fn, filter_width, result) \
    if (true) { \
      g_derivative_filter_width = filter_width * derivative_scale_get(); \
      g_derivative_flag = 1; \
      result.x = (fn); \
      g_derivative_flag = -1; \
      result.y = (fn); \
      g_derivative_flag = 0; \
    }
#endif
