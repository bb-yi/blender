/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef GPU_SHADER_MATERIAL_PRINCIPLED_NPR_V2_MATH_GLSL
#define GPU_SHADER_MATERIAL_PRINCIPLED_NPR_V2_MATH_GLSL

/* Pure numerical helpers. Keep engine/light/closure state outside this file.
 * The GPU oracle loads this source verbatim, adapting only BSL type/reference syntax.
 * Point identity is a CPU-sorted relative rank (0..31), not an arbitrary integer in a float. */
#define NPR_V2_MAX_POINTS 32
#define NPR_V2_MIN_ALPHA 1e-3f

float npr_v2_luminance(float3 color)
{
  return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

/* Energy is scene-linear incident diffuse luminance (including attenuation / pi),
 * not albedo, visibility or display exposure. Its fixed reference is 1. Coordinate
 * Scale is the existing calibration control; no per-light normalization cancels
 * edits to lamp power. The zero-influence endpoint also avoids pow(0, 0). */
float npr_v2_energy_coordinate(float coordinate, float energy, float influence)
{
  float k = clamp(influence, 0.0f, 1.0f);
  if (k == 0.0f) {
    return coordinate;
  }
  if (energy <= 0.0f) {
    return 0.0f;
  }
  return coordinate * pow(energy, k);
}

/* Share other lamps' visible incident energy, while keeping the current lamp's
 * own energy unchanged: its shadow is already in its mapping coordinate. */
float npr_v2_shared_energy(float energy, float visible_fraction, float total_available)
{
  return energy + max(total_available - energy * visible_fraction, 0.0f);
}

float3 npr_v2_safe_normalize(float3 value, float3 fallback)
{
  float length_squared = dot(value, value);
  return length_squared > 1e-20f ? value * inversesqrt(length_squared) : fallback;
}

/* Distance-driven flattening normalized by the lamp's actual surface influence
 * radius, not a material-side scene-unit scale. Sun/zero-radius lights bypass. */
float3 npr_v2_flatten_normal(float3 N, float3 L, float distance, float strength, float radius)
{
  strength = clamp(strength, 0.0f, 1.0f);
  if (strength <= 1e-4f || radius <= 0.0f) {
    return N;
  }
  float3 tangent = N - L * dot(N, L);
  if (dot(tangent, tangent) <= 1e-8f) {
    float3 axis = abs(L.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
    tangent = axis - L * dot(axis, L);
  }
  tangent = npr_v2_safe_normalize(tangent, N);
  float t = clamp(distance / radius, 0.0f, 1.0f);
  float3 target = npr_v2_safe_normalize(mix(L, tangent, t), N);
  float3 blended = mix(N, target, strength);
  float length_squared = dot(blended, blended);
  return length_squared > 1e-8f ? blended * inversesqrt(length_squared) : N;
}

float npr_v2_smoothstep01(float x)
{
  x = clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

float npr_v2_inverse_smoothstep_centered(float x)
{
  float t = 2.0f * clamp(x, 0.0f, 1.0f) - 1.0f;
  if (abs(t) < 0.01f) {
    /* sin(asin(t)/3), expanded around zero. Avoid both driver asin cancellation
     * and adding/subtracting 0.5 when a narrow transition amplifies the residual.
     * The first omitted term is 256*t^7/19683 (<1.4e-16 in this interval). */
    float t_squared = t * t;
    return t * (1.0f / 3.0f + t_squared * (4.0f / 81.0f + t_squared * (16.0f / 729.0f)));
  }
  return sin(asin(t) / 3.0f);
}

float npr_v2_inverse_smoothstep(float x)
{
  return 0.5f + npr_v2_inverse_smoothstep_centered(x);
}

void npr_v2_sort_points(float4 (&colors)[NPR_V2_MAX_POINTS],
                        float4 (&meta)[NPR_V2_MAX_POINTS],
                        int point_count)
{
  int count = clamp(point_count, 2, NPR_V2_MAX_POINTS);
  for (int i = 0; i < count; i++) {
    meta[i].x = clamp(meta[i].x, 0.0f, 1.0f);
  }
  for (int i = 1; i < count; i++) {
    float4 point_color = colors[i];
    float4 point_meta = meta[i];
    int insertion = i;
    while (insertion > 0) {
      float4 previous = meta[insertion - 1];
      if (previous.x < point_meta.x ||
          (previous.x == point_meta.x && previous.w <= point_meta.w))
      {
        break;
      }
      colors[insertion] = colors[insertion - 1];
      meta[insertion] = previous;
      insertion--;
    }
    colors[insertion] = point_color;
    meta[insertion] = point_meta;
  }
}

/* meta=(position, interval-relative boundary offset, softness multiplier, stable rank).
 * mapping=(coordinate scale, coordinate offset, global softness, linear interpolation flag).
 * Sort once before a per-light loop, then call this function for each light. */
float4 npr_v2_map_sorted(float coordinate,
                         const float4 (&colors)[NPR_V2_MAX_POINTS],
                         const float4 (&meta)[NPR_V2_MAX_POINTS],
                         int point_count,
                         float4 mapping)
{
  int count = clamp(point_count, 2, NPR_V2_MAX_POINTS);
  float x = clamp(coordinate * mapping.x + mapping.y, 0.0f, 1.0f);
  int right = count;
  for (int i = 0; i < count; i++) {
    if (x < meta[i].x) {
      right = i;
      break;
    }
  }
  if (right == 0) {
    return colors[0];
  }
  if (right == count || x == meta[right - 1].x) {
    return colors[right - 1];
  }

  float low = meta[right - 1].x;
  float high = meta[right].x;
  float interval = high - low;
  float center = 0.5f * (low + high) - clamp(meta[right].y, -0.5f, 0.5f) * interval;
  float width = clamp(max(mapping.z, 0.0f) * max(meta[right].z, 0.0f), 0.0f, 1.0f) *
                interval;
  width = min(width, 2.0f * min(center - low, high - center));
  float factor;
  if (width <= 0.0f) {
    factor = x >= center ? 1.0f : 0.0f;
  }
  else {
    factor = clamp((x - center) / width + 0.5f, 0.0f, 1.0f);
    if (mapping.w < 0.5f) {
      factor = npr_v2_smoothstep01(factor);
    }
  }
  return mix(colors[right - 1], colors[right], factor);
}

/* Convenience wrapper for a single evaluation. The production light loop uses _sorted. */
float4 npr_v2_map(float coordinate,
                  const float4 (&point_colors)[NPR_V2_MAX_POINTS],
                  const float4 (&point_meta)[NPR_V2_MAX_POINTS],
                  int point_count,
                  float4 mapping)
{
  float4 colors[NPR_V2_MAX_POINTS];
  float4 meta[NPR_V2_MAX_POINTS];
  int count = clamp(point_count, 2, NPR_V2_MAX_POINTS);
  for (int i = 0; i < count; i++) {
    colors[i] = point_colors[i];
    meta[i] = point_meta[i];
  }
  npr_v2_sort_points(colors, meta, count);
  return npr_v2_map_sorted(coordinate, colors, meta, count, mapping);
}

void npr_v2_color_decompose(float4 mapped,
                            bool replace,
                            float3 &multiplier,
                            float3 &additive)
{
  float alpha = clamp(mapped.a, 0.0f, 1.0f);
  float3 color = max(mapped.rgb, float3(0.0f));
  multiplier = replace ? float3(1.0f - alpha) : float3(1.0f - alpha) + color * alpha;
  additive = replace ? color * alpha : float3(0.0f);
}

float3 npr_v2_light_tint(float3 lighting)
{
  lighting = max(lighting, float3(0.0f));
  float peak = max(lighting.x, max(lighting.y, lighting.z));
  float3 tint = peak > 0.0f ? lighting / peak : float3(1.0f);
  return tint / npr_v2_luminance(tint);
}

/* Tint the lit anchors BEFORE interpolation. The first sorted anchor is the dark
 * palette color and remains independent of lamp hue. For two points this is exactly
 * mix(dark, lit * tint, mask), including soft boundaries. All editing modes share it. */
void npr_v2_tint_lit_points(float4 (&colors)[NPR_V2_MAX_POINTS],
                            int point_count,
                            float3 lighting)
{
  float3 tint = npr_v2_light_tint(lighting);
  for (int i = 1; i < point_count; i++) {
    colors[i].rgb *= tint;
  }
}

void npr_v2_total_mapped_decompose(float4 mapped,
                                   float3 lighting,
                                   bool replace,
                                   float3 &multiplier,
                                   float3 &additive)
{
  float alpha = clamp(mapped.a, 0.0f, 1.0f);
  float3 color = max(mapped.rgb, float3(0.0f)) * alpha;
  multiplier = max(lighting, float3(0.0f)) * (1.0f - alpha) +
               (replace ? float3(0.0f) : color);
  additive = replace ? color : float3(0.0f);
}

/* Legacy Total Lighting maps incident irradiance once. Preserve its hue, not its amplitude:
 * multiplying the mapped color by irradiance again would reintroduce bright overlap
 * bands. Mapping Alpha blends back to raw lighting, independently of surface Alpha.
 * Peak normalization avoids a division by tiny luminance without an energy floor. */
void npr_v2_total_color_decompose(float4 mapped,
                                  float3 lighting,
                                  bool replace,
                                  float3 &multiplier,
                                  float3 &additive)
{
  mapped.rgb *= npr_v2_light_tint(lighting);
  npr_v2_total_mapped_decompose(mapped, lighting, replace, multiplier, additive);
}

/* mode: NONE=0, CAST=1, ALL=2, SELF=3 (preserves the V1 enum identities).
 * shadow=(total visibility, self occlusion, cast occlusion). */
float npr_v2_shadow_coordinate(float NoL,
                               int mode,
                               float3 shadow,
                               float strength,
                               bool full_range,
                               bool casts_shadow)
{
  float coordinate = full_range ? 0.5f * NoL + 0.5f : NoL;
  strength = clamp(strength, 0.0f, 1.0f);
  if (mode == 0 || strength == 0.0f || !casts_shadow) {
    return coordinate;
  }
  float visibility = mode == 1 ? 1.0f - shadow.z :
                     mode == 3 ? 1.0f - shadow.y :
                                 shadow.x;
  return mix(coordinate, min(coordinate, clamp(visibility, 0.0f, 1.0f)), strength);
}

void npr_v2_tangent_basis(float3 normal,
                          float3 tangent,
                          float rotation,
                          float3 &T,
                          float3 &B)
{
  normal = npr_v2_safe_normalize(normal, float3(0.0f, 0.0f, 1.0f));
  T = tangent - normal * dot(normal, tangent);
  if (dot(T, T) <= 1e-20f) {
    float3 axis = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) :
                                         float3(1.0f, 0.0f, 0.0f);
    T = cross(axis, normal);
  }
  T = npr_v2_safe_normalize(T, float3(1.0f, 0.0f, 0.0f));
  B = npr_v2_safe_normalize(cross(normal, T), float3(0.0f, 1.0f, 0.0f));
  float angle = rotation * 6.283185307179586f;
  T = T * cos(angle) + B * sin(angle);
  B = npr_v2_safe_normalize(cross(normal, T), B);
}

float2 npr_v2_roughness_axes(float roughness, float anisotropy)
{
  roughness = clamp(roughness, 0.0f, 1.0f);
  float alpha = max(roughness * roughness, NPR_V2_MIN_ALPHA);
  float aspect = sqrt(1.0f - 0.9f * clamp(anisotropy, 0.0f, 1.0f));
  return float2(alpha / aspect, alpha * aspect);
}

float3 npr_v2_material_f0(float3 base_color, float metallic, float ior)
{
  ior = max(ior, 1.0f);
  float dielectric = (ior - 1.0f) / (ior + 1.0f);
  dielectric *= dielectric;
  return mix(float3(dielectric), clamp(base_color, 0.0f, 1.0f), clamp(metallic, 0.0f, 1.0f));
}

float npr_v2_material_f90(float metallic, float ior)
{
  ior = max(ior, 1.0f);
  float dielectric = clamp((2.33f / 0.33f) * (ior - 1.0f) / (ior + 1.0f), 0.0f, 1.0f);
  return mix(dielectric, 1.0f, clamp(metallic, 0.0f, 1.0f));
}

void npr_v2_grazing_reference_frame(float3 direction,
                                      float3 tangent,
                                      float3 &normal,
                                      float3 &view)
{
  direction = npr_v2_safe_normalize(direction, float3(0.0f, 0.0f, 1.0f));
  float3 unused;
  npr_v2_tangent_basis(direction, tangent, 0.0f, view, unused);
  normal = npr_v2_safe_normalize(direction + view, direction);
}

float3 npr_v2_complete_reference(float3 normal_reference, float3 grazing_reference)
{
  float normal_luminance = npr_v2_luminance(normal_reference);
  float grazing_luminance = npr_v2_luminance(grazing_reference);
  if (grazing_luminance > normal_luminance) {
    /* Keep max luminance without an RGB winner switch at the crossover. This supplies
     * a physical F90 reference for dark metals, not an absolute epsilon light source. */
    normal_reference += grazing_reference *
                        ((grazing_luminance - normal_luminance) / grazing_luminance);
  }
  return normal_reference;
}

float npr_v2_smith_g1(float3 direction, float3 N, float3 T, float3 B, float2 axes)
{
  float cosine = dot(direction, N);
  if (cosine <= 0.0f) {
    return 0.0f;
  }
  float projected_x = axes.x * dot(direction, T);
  float projected_y = axes.y * dot(direction, B);
  return 2.0f * cosine /
         (cosine + sqrt(cosine * cosine + projected_x * projected_x + projected_y * projected_y));
}

/* Full RGB BRDF times incident cosine. shape=(roughness, anisotropy, rotation, size scale).
 * The internal size scale is not an extra user-facing control. */
float3 npr_v2_ggx_response(float3 N,
                           float3 V,
                           float3 L,
                           float3 tangent,
                           float4 shape,
                           float3 base_color,
                           float metallic,
                           float ior)
{
  if (shape.w <= 0.0f) {
    return float3(0.0f);
  }
  N = npr_v2_safe_normalize(N, float3(0.0f, 0.0f, 1.0f));
  V = npr_v2_safe_normalize(V, N);
  L = npr_v2_safe_normalize(L, N);
  float NoV = dot(N, V);
  if (NoV <= 0.0f || dot(N, L) <= 0.0f) {
    return float3(0.0f);
  }
  float3 T, B;
  npr_v2_tangent_basis(N, tangent, shape.z, T, B);
  float2 axes = npr_v2_roughness_axes(shape.x, shape.y) * shape.w;
  float3 H = npr_v2_safe_normalize(V + L, N);
  float hx = dot(T, H) / axes.x;
  float hy = dot(B, H) / axes.y;
  float hz = dot(N, H);
  float denominator = hx * hx + hy * hy + hz * hz;
  float distribution = 1.0f / (3.141592653589793f * axes.x * axes.y * denominator * denominator);
  float geometry = npr_v2_smith_g1(V, N, T, B, axes) * npr_v2_smith_g1(L, N, T, B, axes);
  float complement = 1.0f - clamp(dot(V, H), 0.0f, 1.0f);
  float complement_squared = complement * complement;
  float fresnel_power = complement_squared * complement_squared * complement;
  float3 F0 = npr_v2_material_f0(base_color, metallic, ior);
  /* Match the native BSDF LUT's continuous no-interface limit. Metals retain F90=1;
   * only the dielectric fraction loses grazing reflection as IOR approaches 1. */
  float F90 = npr_v2_material_f90(metallic, ior);
  float amplitude = distribution * geometry * shape.w * shape.w / (4.0f * NoV);
  return (F0 + (float3(F90) - F0) * fresnel_power) * amplitude;
}

float npr_v2_highlight_curve(float x, float internal_softness)
{
  if (internal_softness <= 0.0f) {
    return x < 0.5f ? 0.0f : x > 0.5f ? 1.0f : 0.5f;
  }
  if (internal_softness >= 1.0f) {
    return x;
  }
  return npr_v2_smoothstep01(npr_v2_inverse_smoothstep_centered(x) / internal_softness + 0.5f);
}

float npr_v2_highlight_denominator(float y, float internal_softness)
{
  return (1.0f - y) + (1.0f - internal_softness) * y;
}

/* Returns (remapped brightness, original color restoration, reference amplitude).
 * Call only for response/reference > 0 and softness < 1; public helpers handle endpoints.
 * No absolute radiance floor: uniformly scaling a light must not move its hard boundary. */
float3 npr_v2_highlight_terms(float response, float reference, float softness, float beta)
{
  float internal = softness * softness * softness;
  float encoded = 1.0f / (1.0f + beta * reference / response);
  float y = npr_v2_highlight_curve(encoded, internal);
  float yr = npr_v2_highlight_curve(1.0f / (1.0f + beta), internal);
  float denominator = npr_v2_highlight_denominator(y, internal);
  float amplitude = reference * (y / yr) * npr_v2_highlight_denominator(yr, internal) / denominator;
  float restore = internal * (1.0f - y) / denominator;
  return float3(amplitude, restore, reference);
}

float npr_v2_highlight_scalar(float response, float reference, float softness, float beta)
{
  softness = clamp(softness, 0.0f, 1.0f);
  if (softness == 1.0f) {
    return response;
  }
  if (response <= 0.0f || reference <= 0.0f) {
    return 0.0f;
  }
  return npr_v2_highlight_terms(response, reference, softness, beta).x;
}

float3 npr_v2_highlight_rgb(float3 response_rgb, float3 reference_rgb, float softness, float beta)
{
  softness = clamp(softness, 0.0f, 1.0f);
  if (softness == 1.0f) {
    return response_rgb;
  }
  float response = npr_v2_luminance(response_rgb);
  float reference = npr_v2_luminance(reference_rgb);
  if (response <= 0.0f || reference <= 0.0f) {
    return float3(0.0f);
  }
  float3 terms = npr_v2_highlight_terms(response, reference, softness, beta);
  return terms.x * mix(reference_rgb / terms.z, response_rgb / response, terms.y);
}

/* Explicit 257-entry calibration row. No dependency on the color-band global state.
 * CPU packs RG16F as beta=R+G/65536; interpolation is always along UI softness. */
float npr_v2_sample_beta(sampler1DArray calibration, float row, float softness)
{
  float position = clamp(softness, 0.0f, 1.0f) * 256.0f;
  int left = min(int(position), 255);
  float fraction = position - float(left);
  float2 a = texelFetch(calibration, int2(left, int(row)), 0).xy;
  float2 b = texelFetch(calibration, int2(left + 1, int(row)), 0).xy;
  return mix(a.x + a.y / 65536.0f, b.x + b.y / 65536.0f, fraction);
}

float npr_v2_hard_edge_coverage(float signed_inside_distance, float footprint_width)
{
  if (footprint_width <= 0.0f) {
    return signed_inside_distance < 0.0f ? 0.0f : signed_inside_distance > 0.0f ? 1.0f : 0.5f;
  }
  return clamp(0.5f + signed_inside_distance / footprint_width, 0.0f, 1.0f);
}

#endif
