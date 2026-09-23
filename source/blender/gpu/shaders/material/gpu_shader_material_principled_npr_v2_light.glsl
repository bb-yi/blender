/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef GPU_SHADER_MATERIAL_PRINCIPLED_NPR_V2_LIGHT_GLSL
#define GPU_SHADER_MATERIAL_PRINCIPLED_NPR_V2_LIGHT_GLSL

#include "gpu_shader_material_principled_npr_v2_math.glsl"

/* Deterministic balance-heuristic MIS: emitter sampling + anisotropic GGX VNDF.
 * Unlike an emitter-only grid, VNDF samples retain a narrow highlight inside a large emitter.
 * Both complete single-light integrals are evaluated BEFORE the caller reshapes the highlight.
 * This budget is an internal integration setting, not another material control.
 * The reference includes normal incidence and a fixed 45-degree pure-F90 contribution.
 * At 64 samples per strategy these plus the actual response require 384 GGX evaluations;
 * non-unit size adds 256 original-reference evaluations. This is not a performance claim. */
#define NPR_V2_LIGHT_SAMPLE_COUNT 64
#define NPR_V2_EMITTER_CONE 0
#define NPR_V2_EMITTER_RECT 1
#define NPR_V2_EMITTER_ELLIPSE 2

struct NPRV2Emitter {
  int kind;
  /* Relative position for area emitters; unit center direction for cones. */
  float3 center;
  float3 axis_u;
  float3 axis_v;
  /* Points from the receiving side toward an area emitter, matching LightData::z_axis(). */
  float3 normal;
  float2 half_size;
  /* Store 1-cos(theta) directly so a small source does not lose its solid angle. */
  float one_minus_cosine;
};

float2 npr_v2_light_sequence(int index, int count)
{
  float radical_inverse = float(bitfieldReverse(uint(index))) * 2.3283064365386963e-10f;
  return float2((float(index) + 0.5f) / float(count),
                fract(radical_inverse + 0.5f / float(count)));
}

float npr_v2_emitter_area(NPRV2Emitter emitter)
{
  float scale = emitter.kind == NPR_V2_EMITTER_RECT ? 4.0f : 3.141592653589793f;
  return scale * emitter.half_size.x * emitter.half_size.y;
}

float npr_v2_emitter_pdf(NPRV2Emitter emitter, float3 L)
{
  if (emitter.kind == NPR_V2_EMITTER_CONE) {
    float3 difference = L - emitter.center;
    if (dot(difference, difference) > 2.0f * emitter.one_minus_cosine) {
      return 0.0f;
    }
    return 1.0f / (6.283185307179586f * emitter.one_minus_cosine);
  }

  float plane_distance = dot(emitter.center, emitter.normal);
  float cosine = dot(L, emitter.normal);
  if (plane_distance <= 0.0f || cosine <= 0.0f) {
    return 0.0f;
  }
  float distance = plane_distance / cosine;
  float3 delta = distance * L - emitter.center;
  float2 uv = float2(dot(delta, emitter.axis_u), dot(delta, emitter.axis_v)) / emitter.half_size;
  bool inside = emitter.kind == NPR_V2_EMITTER_RECT ? max(abs(uv.x), abs(uv.y)) <= 1.0f :
                                                     dot(uv, uv) <= 1.0f;
  if (!inside) {
    return 0.0f;
  }
  return distance * distance / (cosine * npr_v2_emitter_area(emitter));
}

/* Returns a direction and its solid-angle PDF. */
float4 npr_v2_emitter_sample(NPRV2Emitter emitter, float2 random_value)
{
  if (emitter.kind == NPR_V2_EMITTER_CONE) {
    float d = emitter.one_minus_cosine * random_value.x;
    float radius = sqrt(max(d * (2.0f - d), 0.0f));
    float phi = 6.283185307179586f * random_value.y;
    float3 L = emitter.center * (1.0f - d) +
               radius * (emitter.axis_u * cos(phi) + emitter.axis_v * sin(phi));
    L = npr_v2_safe_normalize(L, emitter.center);
    return float4(L, 1.0f / (6.283185307179586f * emitter.one_minus_cosine));
  }

  float2 point;
  if (emitter.kind == NPR_V2_EMITTER_RECT) {
    point = 2.0f * random_value - 1.0f;
  }
  else {
    float phi = 6.283185307179586f * random_value.y;
    point = sqrt(random_value.x) * float2(cos(phi), sin(phi));
  }
  point *= emitter.half_size;
  float3 to_light = emitter.center + emitter.axis_u * point.x + emitter.axis_v * point.y;
  float distance_squared = dot(to_light, to_light);
  if (distance_squared <= 0.0f) {
    return float4(0.0f);
  }
  float3 L = to_light * inversesqrt(distance_squared);
  float cosine = dot(L, emitter.normal);
  float pdf = cosine > 0.0f ? distance_squared / (cosine * npr_v2_emitter_area(emitter)) : 0.0f;
  return float4(L, pdf);
}

/* Anisotropic visible-normal spherical-cap sampling, matching separable Smith GGX.
 * This is the unbounded VNDF: below-surface reflected directions are rejected by the BSDF,
 * not renormalized away. The resulting PDF is D(H)*G1(V)/(4*NdotV). */
float3 npr_v2_ggx_vndf_direction(float3 N,
                                float3 V,
                                float3 T,
                                float3 B,
                                float2 axes,
                                float2 random_value)
{
  float3 view_local = float3(dot(V, T), dot(V, B), dot(V, N));
  float3 stretched_view = npr_v2_safe_normalize(
      float3(axes * view_local.xy, view_local.z), float3(0.0f, 0.0f, 1.0f));
  float cosine = mix(-stretched_view.z, 1.0f, random_value.x);
  float radius = sqrt(max(1.0f - cosine * cosine, 0.0f));
  float phi = 6.283185307179586f * random_value.y;
  float3 cap = float3(radius * cos(phi), radius * sin(phi), cosine);
  float3 half_stretched = stretched_view + cap;
  float3 half_local = npr_v2_safe_normalize(
      float3(axes * half_stretched.xy, max(half_stretched.z, 0.0f)),
      float3(0.0f, 0.0f, 1.0f));
  float3 H = T * half_local.x + B * half_local.y + N * half_local.z;
  return npr_v2_safe_normalize(reflect(-V, H), N);
}

float npr_v2_ggx_vndf_pdf(float3 N, float3 V, float3 L, float3 T, float3 B, float2 axes)
{
  float NoV = dot(N, V);
  if (NoV <= 0.0f) {
    return 0.0f;
  }
  float3 H = npr_v2_safe_normalize(V + L, N);
  if (dot(N, H) <= 0.0f || dot(V, H) <= 0.0f) {
    return 0.0f;
  }
  float hx = dot(H, T) / axes.x;
  float hy = dot(H, B) / axes.y;
  float hz = dot(H, N);
  float d = hx * hx + hy * hy + hz * hz;
  float distribution = 1.0f / (3.141592653589793f * axes.x * axes.y * d * d);
  return distribution * npr_v2_smith_g1(V, N, T, B, axes) / (4.0f * NoV);
}

float3 npr_v2_integrate_emitter(NPRV2Emitter emitter,
                               float3 N,
                               float3 V,
                               float3 tangent,
                               float4 shape,
                               float3 base,
                               float metallic,
                               float ior,
                               int sample_count)
{
  N = npr_v2_safe_normalize(N, float3(0.0f, 0.0f, 1.0f));
  V = npr_v2_safe_normalize(V, N);
  if (shape.w <= 0.0f || dot(N, V) <= 0.0f) {
    return float3(0.0f);
  }
  if (emitter.kind != NPR_V2_EMITTER_CONE && dot(emitter.center, emitter.normal) <= 0.0f) {
    return float3(0.0f);
  }
  float3 T, B;
  npr_v2_tangent_basis(N, tangent, shape.z, T, B);
  float2 axes = npr_v2_roughness_axes(shape.x, shape.y) * shape.w;
  float3 integrated = float3(0.0f);
  int count = max(sample_count, 1);
  for (int i = 0; i < count; i++) {
    float2 random_value = npr_v2_light_sequence(i, count);
    float4 emitter_direction = npr_v2_emitter_sample(emitter, random_value);
    if (emitter_direction.w > 0.0f) {
      float bsdf_pdf = npr_v2_ggx_vndf_pdf(N, V, emitter_direction.xyz, T, B, axes);
      integrated += npr_v2_ggx_response(
                        N, V, emitter_direction.xyz, tangent, shape, base, metallic, ior) /
                    (emitter_direction.w + bsdf_pdf);
    }

    /* A golden-angle azimuth avoids synchronizing the VNDF's few grazing-tail samples
     * with the emitter grid. Keep the radial coordinate stratified. */
    random_value.y = fract((float(i) + 0.5f) * 0.6180339887498949f);
    float3 direction = npr_v2_ggx_vndf_direction(N, V, T, B, axes, random_value);
    float emitter_pdf = npr_v2_emitter_pdf(emitter, direction);
    if (emitter_pdf > 0.0f) {
      float bsdf_pdf = npr_v2_ggx_vndf_pdf(N, V, direction, T, B, axes);
      integrated += npr_v2_ggx_response(N, V, direction, tangent, shape, base, metallic, ior) /
                    (emitter_pdf + bsdf_pdf);
    }
  }
  return integrated / float(count);
}

/* GLSLLight is expressed using point-friendly power. Undo this conversion exactly once.
 * Default lamps include point_falloff in attenuation, which cancels here. A light shader
 * deliberately omits it; retaining the division matches native no-distance-light semantics. */
float3 npr_v2_finite_radiance(float3 specular_color,
                             float attenuation,
                             float point_falloff,
                             float shape_radiance,
                             float point_radiance)
{
  float denominator = point_falloff * (point_radiance / shape_radiance);
  if (denominator <= 0.0f) {
    return float3(0.0f);
  }
  return specular_color * (attenuation / denominator);
}

float3 npr_v2_preserve_light_reference(float3 response,
                                       float3 resized_reference,
                                       float3 original_reference)
{
  /* Do not divide a black channel or create radiance where the sampled reference is zero. */
  float3 correction = float3(
      resized_reference.x > 0.0f ? original_reference.x / resized_reference.x : 0.0f,
      resized_reference.y > 0.0f ? original_reference.y / resized_reference.y : 0.0f,
      resized_reference.z > 0.0f ? original_reference.z / resized_reference.z : 0.0f);
  return response * correction;
}

float3 npr_v2_punctual_reference(float3 direction,
                                  float3 tangent,
                                  float4 shape,
                                  float3 base,
                                  float metallic,
                                  float ior)
{
  float3 normal = npr_v2_ggx_response(
      direction, direction, direction, tangent, shape, base, metallic, ior);
  float f90 = npr_v2_material_f90(metallic, ior);
  if (f90 <= 0.0f) {
    return normal;
  }
  float3 grazing_N, grazing_V;
  npr_v2_grazing_reference_frame(direction, tangent, grazing_N, grazing_V);
  float3 grazing = npr_v2_ggx_response(
      grazing_N, grazing_V, direction, tangent, shape, float3(0.0f), 1.0f, ior) * f90;
  return npr_v2_complete_reference(normal, grazing);
}

void npr_v2_punctual_response(float3 direction,
                              float3 N,
                              float3 V,
                              float3 tangent,
                              float4 shape,
                              float3 base,
                              float metallic,
                              float ior,
                              float3 &response,
                              float3 &reference)
{
  response = npr_v2_ggx_response(N, V, direction, tangent, shape, base, metallic, ior);
  reference = npr_v2_punctual_reference(direction, tangent, shape, base, metallic, ior);
  if (shape.w != 1.0f) {
    float4 original_shape = shape;
    original_shape.w = 1.0f;
    float3 original_reference = npr_v2_punctual_reference(
        direction, tangent, original_shape, base, metallic, ior);
    response = npr_v2_preserve_light_reference(response, reference, original_reference);
    reference = original_reference;
  }
}

float3 npr_v2_emitter_reference(NPRV2Emitter emitter,
                                float3 tangent,
                                float4 shape,
                                float3 base,
                                float metallic,
                                float ior,
                                int sample_count)
{
  float3 center = npr_v2_safe_normalize(emitter.center, float3(0.0f, 0.0f, 1.0f));
  float3 normal = npr_v2_integrate_emitter(
      emitter, center, center, tangent, shape, base, metallic, ior, sample_count);
  float f90 = npr_v2_material_f90(metallic, ior);
  if (f90 <= 0.0f) {
    return normal;
  }
  float3 grazing_N, grazing_V;
  npr_v2_grazing_reference_frame(center, tangent, grazing_N, grazing_V);
  float3 grazing = npr_v2_integrate_emitter(
      emitter, grazing_N, grazing_V, tangent, shape, float3(0.0f), 1.0f, ior, sample_count) * f90;
  return npr_v2_complete_reference(normal, grazing);
}

void npr_v2_emitter_response(NPRV2Emitter emitter,
                             float3 N,
                             float3 V,
                             float3 tangent,
                             float4 shape,
                             float3 base,
                             float metallic,
                             float ior,
                             int sample_count,
                             float3 &response,
                             float3 &reference)
{
  response = npr_v2_integrate_emitter(emitter, N, V, tangent, shape, base, metallic, ior, sample_count);
  reference = npr_v2_emitter_reference(emitter, tangent, shape, base, metallic, ior, sample_count);
  if (shape.w != 1.0f) {
    float4 original_shape = shape;
    original_shape.w = 1.0f;
    float3 original_reference = npr_v2_emitter_reference(
        emitter, tangent, original_shape, base, metallic, ior, sample_count);
    response = npr_v2_preserve_light_reference(response, reference, original_reference);
    reference = original_reference;
  }
}

#ifndef NPR_V2_LIGHT_MATH_ONLY
#  include "gpu_shader_material_glsl_light_access.glsl"

void npr_v2_light_response(uint light_index,
                           bool is_local,
                           GLSLLight lamp,
                           float3 N,
                           float3 V,
                           float3 tangent,
                           float4 shape,
                           float3 base,
                           float metallic,
                           float ior,
                           bool need_reference,
                           float3 &response,
                           float3 &reference)
{
  response = float3(0.0f);
  reference = float3(0.0f);
#  if defined(GPU_FRAGMENT_SHADER) && defined(MAT_GLSL_LIGHT_ACCESS)
  if (!lamp.valid || lamp.attenuation <= 0.0f ||
      all(lessThanEqual(lamp.specular_color, float3(0.0f))))
  {
    return;
  }
  LightData light;
  LightVector lv;
  bool is_directional;
  if (!glsl_light_lookup(light_index, is_local, light, lv, is_directional)) {
    return;
  }
  /* light_vector_get has an undefined direction exactly at a local light's center.
   * Inside a sphere every direction emits, so any finite frame is equivalent. */
  lv.L = npr_v2_safe_normalize(lv.L, npr_v2_safe_normalize(N, float3(0.0f, 0.0f, 1.0f)));

  bool punctual = is_directional ? light.sun().shadow_angle == 0.0f :
                  is_point_light(light.type) ? light.local().local.shadow_radius == 0.0f :
                                              false;
  if (punctual) {
    float3 intensity = lamp.specular_color * lamp.attenuation;
    if (need_reference) {
      npr_v2_punctual_response(lv.L, N, V, tangent, shape, base, metallic, ior, response, reference);
      reference *= intensity;
    }
    else {
      response = npr_v2_ggx_response(N, V, lv.L, tangent, shape, base, metallic, ior);
    }
    response *= intensity;
    return;
  }

  NPRV2Emitter emitter;
  emitter.kind = NPR_V2_EMITTER_CONE;
  emitter.center = lv.L;
  emitter.normal = lv.L;
  emitter.half_size = float2(1.0f);
  emitter.one_minus_cosine = 1.0f;
  npr_v2_tangent_basis(lv.L, float3(0.0f), 0.0f, emitter.axis_u, emitter.axis_v);
  if (is_area_light(light.type)) {
    emitter.kind = light.type == LIGHT_RECT ? NPR_V2_EMITTER_RECT : NPR_V2_EMITTER_ELLIPSE;
    emitter.center = lv.L * lv.dist;
    emitter.axis_u = light.x_axis();
    emitter.axis_v = light.y_axis();
    emitter.normal = light.z_axis();
    emitter.half_size = light.area().size;
  }
  else if (is_sphere_light(light.type)) {
    float radius = light.local().local.shape_radius;
    if (lv.dist <= radius) {
      /* Match the engine's inside-sphere convention: emission covers every direction. */
      emitter.one_minus_cosine = 2.0f;
    }
    else {
      float sine_squared = (radius / lv.dist) * (radius / lv.dist);
      emitter.one_minus_cosine = sine_squared / (1.0f + sqrt(max(1.0f - sine_squared, 0.0f)));
    }
  }
  else {
    /* Native omni/spot DISK is receiver-facing. Sun radius is tan(half-angle) at distance 1. */
    float tangent_radius = is_directional ? light.sun().shape_radius :
                                            light.local().local.shape_radius / lv.dist;
    float hypotenuse = sqrt(1.0f + tangent_radius * tangent_radius);
    emitter.one_minus_cosine = tangent_radius * tangent_radius /
                              (hypotenuse * (hypotenuse + 1.0f));
  }

  float3 radiance = npr_v2_finite_radiance(lamp.specular_color,
                                          lamp.attenuation,
                                          light_point_light(light, is_directional, lv),
                                          glsl_light_shape_radiance(light),
                                          glsl_light_point_radiance(light));
  /* The caller needs no reference for the unresized GGX identity with a constant tint.
   * Keep the actual response's complete MIS integral, only omitting unused reference integrals. */
  if (need_reference) {
    npr_v2_emitter_response(emitter,
                           N,
                           V,
                           tangent,
                           shape,
                           base,
                           metallic,
                           ior,
                           NPR_V2_LIGHT_SAMPLE_COUNT,
                           response,
                           reference);
    reference *= radiance;
  }
  else {
    response = npr_v2_integrate_emitter(
        emitter, N, V, tangent, shape, base, metallic, ior, NPR_V2_LIGHT_SAMPLE_COUNT);
  }
  response *= radiance;
#  endif
}
#endif

#endif
