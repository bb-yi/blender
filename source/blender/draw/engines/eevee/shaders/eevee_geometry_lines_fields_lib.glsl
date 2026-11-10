/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "gpu_shader_material_mix_color.bsl.hh"
#include "gpu_shader_bicubic_sampler.bsl.hh"
#include "gpu_shader_common_math.bsl.hh"
#include "gpu_shader_material_vector_math.bsl.hh"
#include "gpu_shader_material_tex_white_noise.bsl.hh"

#define LINE_FIELD_ALLOW_GAP_CAPS 16

float4 line_image_sample(sampler2D image, float2 uv, int interpolation, int extension)
{
  int2 extent = textureSize(image, 0);
  int wrap_mode = extension == 0 ? 0 : extension == 3 ? 2 : extension == 2 ? 4 : 1;
  int2 wrap = int2(wrap_mode);
  if (interpolation == 1) {
    return line_map_texel(image, int2(floor(uv * float2(extent))), extent, wrap);
  }
  if (extension == 1) {
    return interpolation >= 2 ? texture_bicubic(image, uv) : textureLod(image, uv, 0.0f);
  }
  /* Wrapping UV before a clamped hardware lookup does not wrap the filter's
   * neighboring texels. Address each tap, including the cubic border taps. */
  float2 position = uv * float2(extent) - 0.5f;
  int2 base = int2(floor(position));
  float2 f = fract(position);
  if (interpolation < 2) {
    return mix(mix(line_map_texel(image, base, extent, wrap),
                   line_map_texel(image, base + int2(1, 0), extent, wrap), f.x),
               mix(line_map_texel(image, base + int2(0, 1), extent, wrap),
                   line_map_texel(image, base + int2(1, 1), extent, wrap), f.x), f.y);
  }
  float2 w0, w1, w2, w3;
  cubic_bspline_coefficients(f, w0, w1, w2, w3);
  float4 wx = float4(w0.x, w1.x, w2.x, w3.x);
  float4 wy = float4(w0.y, w1.y, w2.y, w3.y);
  float4 result = float4(0.0f);
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      result += line_map_texel(image, base + int2(x - 1, y - 1), extent, wrap) * wx[x] * wy[y];
    }
  }
  return result;
}

float line_field_math(int op, float a, float b, float c)
{
  float result = 0.0f;
  switch (op) {
    case 0: math_add(a, b, c, result); break;
    case 1: math_subtract(a, b, c, result); break;
    case 2: math_multiply(a, b, c, result); break;
    case 3: math_divide(a, b, c, result); break;
    case 4: math_sine(a, b, c, result); break;
    case 5: math_cosine(a, b, c, result); break;
    case 6: math_tangent(a, b, c, result); break;
    case 7: math_arcsine(a, b, c, result); break;
    case 8: math_arccosine(a, b, c, result); break;
    case 9: math_arctangent(a, b, c, result); break;
    case 10: math_power(a, b, c, result); break;
    case 11: math_logarithm(a, b, c, result); break;
    case 12: math_minimum(a, b, c, result); break;
    case 13: math_maximum(a, b, c, result); break;
    case 14: math_round(a, b, c, result); break;
    case 15: math_less_than(a, b, c, result); break;
    case 16: math_greater_than(a, b, c, result); break;
    case 17: math_modulo(a, b, c, result); break;
    case 18: math_absolute(a, b, c, result); break;
    case 19: math_arctan2(a, b, c, result); break;
    case 20: math_floor(a, b, c, result); break;
    case 21: math_ceil(a, b, c, result); break;
    case 22: math_fraction(a, b, c, result); break;
    case 23: math_sqrt(a, b, c, result); break;
    case 24: math_inversesqrt(a, b, c, result); break;
    case 25: math_sign(a, b, c, result); break;
    case 26: math_exponent(a, b, c, result); break;
    case 27: math_radians(a, b, c, result); break;
    case 28: math_degrees(a, b, c, result); break;
    case 29: math_sinh(a, b, c, result); break;
    case 30: math_cosh(a, b, c, result); break;
    case 31: math_tanh(a, b, c, result); break;
    case 32: math_trunc(a, b, c, result); break;
    case 33: math_snap(a, b, c, result); break;
    case 34: math_wrap(a, b, c, result); break;
    case 35: math_compare(a, b, c, result); break;
    case 36: math_multiply_add(a, b, c, result); break;
    case 37: math_pingpong(a, b, c, result); break;
    case 38: math_smoothmin(a, b, c, result); break;
    case 39: math_smoothmax(a, b, c, result); break;
    case 40: math_floored_modulo(a, b, c, result); break;
  }
  return result;
}

float4 line_field_vector_math(int op, float3 a, float3 b, float3 c, float scale)
{
  float3 vector = float3(0);
  float value = 0.0f;
  switch (op) {
    case 0: vector_math_add(a, b, c, scale, vector, value); break;
    case 1: vector_math_subtract(a, b, c, scale, vector, value); break;
    case 2: vector_math_multiply(a, b, c, scale, vector, value); break;
    case 3: vector_math_divide(a, b, c, scale, vector, value); break;
    case 4: vector_math_cross(a, b, c, scale, vector, value); break;
    case 5: vector_math_project(a, b, c, scale, vector, value); break;
    case 6: vector_math_reflect(a, b, c, scale, vector, value); break;
    case 7: vector_math_dot(a, b, c, scale, vector, value); break;
    case 8: vector_math_distance(a, b, c, scale, vector, value); break;
    case 9: vector_math_length(a, b, c, scale, vector, value); break;
    case 10: vector_math_scale(a, b, c, scale, vector, value); break;
    case 11: vector_math_normalize(a, b, c, scale, vector, value); break;
    case 12: vector_math_snap(a, b, c, scale, vector, value); break;
    case 13: vector_math_floor(a, b, c, scale, vector, value); break;
    case 14: vector_math_ceil(a, b, c, scale, vector, value); break;
    case 15: vector_math_modulo(a, b, c, scale, vector, value); break;
    case 16: vector_math_fraction(a, b, c, scale, vector, value); break;
    case 17: vector_math_absolute(a, b, c, scale, vector, value); break;
    case 18: vector_math_minimum(a, b, c, scale, vector, value); break;
    case 19: vector_math_maximum(a, b, c, scale, vector, value); break;
    case 20: vector_math_wrap(a, b, c, scale, vector, value); break;
    case 21: vector_math_sine(a, b, c, scale, vector, value); break;
    case 22: vector_math_cosine(a, b, c, scale, vector, value); break;
    case 23: vector_math_tangent(a, b, c, scale, vector, value); break;
    case 24: vector_math_refract(a, b, c, scale, vector, value); break;
    case 25: vector_math_faceforward(a, b, c, scale, vector, value); break;
    case 26: vector_math_multiply_add(a, b, c, scale, vector, value); break;
    case 27: vector_math_power(a, b, c, scale, vector, value); break;
    case 28: vector_math_sign(a, b, c, scale, vector, value); break;
    case 29: vector_math_round(a, b, c, scale, vector, value); break;
  }
  return op >= 7 && op <= 9 ? float4(value) : float4(vector, 0);
}

float4 line_field_image(int index, float2 uv, int flags)
{
  /* Blender image extension: repeat, extend, clip, mirror. GPU image textures provide
   * scene-linear RGB using the image's color-space settings. Alpha is separate. */
  int extension = flags & 255;
  if (extension == 2 && (any(lessThan(uv, float2(0))) || any(greaterThan(uv, float2(1))))) {
    return float4(0);
  }
  uv = extension == 0 ? fract(uv) : extension == 3 ? 1.0f - abs(mod(uv, 2.0f) - 1.0f) : clamp(uv, 0.0f, 1.0f);
  if (index == 0) { return line_image_sample(field_image_0_tx, uv, flags >> 8, extension); }
  if (index == 1) { return line_image_sample(field_image_1_tx, uv, flags >> 8, extension); }
  if (index == 2) { return line_image_sample(field_image_2_tx, uv, flags >> 8, extension); }
  if (index == 3) { return line_image_sample(field_image_3_tx, uv, flags >> 8, extension); }
  return float4(0);
}

GeometryLinePartData line_source_part(GeometryLinePartData part, float arc)
{
  uint first = part.source_range.x;
  uint count = part.source_range.y;
  if (count == 0u) { return part; }
  float position = (part.meta.y & 1u) != 0u ? mod(arc, max(part.arc.z, 1e-8f)) : arc;
  uint lo = first, hi = first + count;
  while (lo + 1u < hi) {
    uint mid = (lo + hi) / 2u;
    if (original_parts[mid].arc.x <= position) { lo = mid; }
    else { hi = mid; }
  }
  return original_parts[lo];
}

float4 line_source_value(GeometryLinePartData part, float arc, int index, out bool valid)
{
  valid = false;
  if (index < 0 || index >= LINE_FIELD_MAX_SOURCES) { return float4(0); }
  float t = clamp((arc - part.arc.x) / max(part.arc.y, 1e-20f), 0.0f, 1.0f);
  float q = mix(part.field_weights.x, part.field_weights.y, t);
  valid = ((part.field_meta.x & part.field_meta.y) & (1u << uint(index))) != 0u;
  return mix(part.field_source_a[index] * part.field_weights.x,
             part.field_source_b[index] * part.field_weights.y, t) / max(q, 1e-20f);
}

float4 line_aov_value(GeometryLinePartData part, float arc, int index, bool value_type,
                      float4 fallback, out bool valid)
{
  valid = false;
  if (index < 0 || part.field_meta.w != 0u) { return fallback; }
  float4 config = part.field_source_a[index];
  if (config.x < 0.0f) { return fallback; }
  float t = clamp((arc - part.arc.x) / max(part.arc.y, 1e-20f), 0.0f, 1.0f);
  float3 source = mix(part.a.xyz, part.b.xyz, t);
  int2 center = int2(floor(source.xy));
  float best = 1e30f;
  float4 result = fallback;
  int2 extent = textureSize(field_aov_color_tx, 0).xy;
  for (int y = -2; y <= 2; y++) {
    for (int x = -2; x <= 2; x++) {
      int2 pixel = center + int2(x, y);
      if (any(lessThan(pixel, int2(0))) || any(greaterThanEqual(pixel, extent))) { continue; }
      float4 marker = texelFetch(field_aov_color_tx, int3(pixel, int(config.y)), 0);
      uint identity = uint(marker.x + 0.5f) | (uint(marker.y + 0.5f) << 11u) | (uint(marker.z + 0.5f) << 22u);
      float4 material = texelFetch(field_aov_color_tx, int3(pixel, int(config.y) + 1), 0);
      uint material_id = uint(material.x + 0.5f) | (uint(material.y + 0.5f) << 11u) | (uint(material.z + 0.5f) << 22u);
      uint expected_material = uint(config.z) | (uint(config.w) << 16u);
      if (marker.w < 0.5f || identity != part.field_meta.z || material_id != expected_material) { continue; }
      float depth = texelFetch(field_depth_tx, pixel, 0).r;
      float error = abs(depth - (1.0f - (source.z * 0.5f + 0.5f)));
      if (error > 0.002f) { continue; }
      float score = float(x * x + y * y) + error;
      if (score < best) {
        best = score;
        valid = true;
        result = value_type ? float4(texelFetch(field_aov_value_tx, int3(pixel, int(config.x)), 0).r) :
                              texelFetch(field_aov_color_tx, int3(pixel, int(config.x)), 0);
      }
    }
  }
  return result;
}

void line_fields(GeometryLineBrushData brush, GeometryLinePartData part, float arc,
                 float source_arc, int stage, out float3 position, out float4 color, out float width,
                 out float opacity)
{
  position = float3(arc, 0, 0);
  color = brush.color;
  width = brush.tip.x;
  opacity = brush.tip.y;
  if (brush.field_outputs.w == 0) { return; }
  float4 registers[LINE_FIELD_MAX_REGISTERS];
  GeometryLinePartData source = line_source_part(part, source_arc);
  float mapped_arc = (part.meta.y & 1u) != 0u ? mod(source_arc, max(part.arc.z, 1e-8f)) : source_arc;
  for (int i = 0; i < brush.field_program.y; i++) {
    GeometryLineFieldInstruction instruction = field_instructions[brush.field_program.x + i];
    if ((instruction.extra.w & stage) == 0) { continue; }
    int4 op = instruction.op;
    int4 meta = instruction.meta;
    float4 a = op.y >= 0 ? registers[op.y] : float4(0);
    float4 b = op.z >= 0 ? registers[op.z] : float4(0);
    float4 c = op.w >= 0 ? registers[op.w] : float4(0);
    float4 d = instruction.extra.x >= 0 ? registers[instruction.extra.x] : float4(0);
    float4 e = instruction.extra.y >= 0 ? registers[instruction.extra.y] : float4(0);
    float4 f = instruction.extra.z >= 0 ? registers[instruction.extra.z] : float4(0);
    float4 result = instruction.value;
    if (op.x == LINE_FIELD_INFO) {
      if (meta.y == 0) { result = op.y >= 0 ? a : float4(arc, 0, 0, 0); }
      else if (meta.y == 1) { result = float4(arc); }
      else if (meta.y == 2) { result = float4(part.arc.z); }
      /* Factor is the original stroke's normalized parameter, including at
       * rounded endpoints. Keep intentional X extrapolation in Position Offset;
       * do not turn one ULP of arc error into a huge Float Curve end slope. */
      else if (meta.y == 3) { result = float4(clamp(arc / max(part.arc.z, 1e-8f), 0.0f, 1.0f)); }
      else if (meta.y == 4) { result = float4(float(part.meta.x & 0xffffffu)); }
      else if (meta.y == 5) { result = float4(float(arc <= 1e-5f && (part.meta.y & 1u) == 0u)); }
      else if (meta.y == 6) { result = float4(float(arc >= part.arc.z - 1e-5f && (part.meta.y & 1u) == 0u)); }
      else { result = float4(float((part.meta.y & 1u) != 0u)); }
    }
    else if (op.x == LINE_FIELD_MATH) {
      result = float4(line_field_math(meta.y, a.x, b.x, c.x));
      if ((meta.w & 1) != 0) { result = clamp(result, 0.0f, 1.0f); }
    }
    else if (op.x == LINE_FIELD_VECTOR_MATH) {
      result = line_field_vector_math(meta.y, a.xyz, b.xyz, c.xyz, d.x);
    }
    else if (op.x == LINE_FIELD_SEPARATE) { result = float4(a[meta.z]); }
    else if (op.x == LINE_FIELD_COMBINE) { result = float4(a.x, b.x, c.x, 0); }
    else if (op.x == LINE_FIELD_POSITION) { result = d.x != 0.0f ? b + c : a; }
    else if (op.x == LINE_FIELD_RAMP) {
      float p = clamp(a.x, 0.0f, 1.0f) * 256.0f;
      int index = int(p);
      result = mix(field_ramps[meta.y + index], field_ramps[meta.y + min(index + 1, 256)], fract(p));
      if (meta.z == 1) { result = float4(result.a); }
    }
    else if (op.x == LINE_FIELD_CURVE) {
      float t = (a.x - instruction.value.x) * instruction.value.y;
      float p = clamp(t, 0.0f, 1.0f) * 256.0f;
      int index = int(p);
      float curve = mix(field_ramps[meta.y + index].x,
                        field_ramps[meta.y + min(index + 1, 256)].x, fract(p));
      curve += min(t, 0.0f) * instruction.value.z + max(t - 1.0f, 0.0f) * instruction.value.w;
      result = float4(mix(a.x, curve, b.x));
    }
    else if (op.x == LINE_FIELD_WHITE_NOISE) {
      float value = 0.0f;
      float4 noise_color = float4(0);
      if (meta.y == 1) { node_white_noise_1d(a.xyz, b.x, value, noise_color); }
      else if (meta.y == 2) { node_white_noise_2d(a.xyz, b.x, value, noise_color); }
      else if (meta.y == 3) { node_white_noise_3d(a.xyz, b.x, value, noise_color); }
      else { node_white_noise_4d(a.xyz, b.x, value, noise_color); }
      result = meta.z == 1 ? noise_color : float4(value);
    }
    else if (op.x == LINE_FIELD_IMAGE) {
      result = line_field_image(meta.y, a.xy, meta.w);
      if (meta.z == 1) { result = float4(result.a); }
    }
    else if (op.x == LINE_FIELD_SOURCE || op.x == LINE_FIELD_AOV) {
      bool valid;
      result = op.x == LINE_FIELD_SOURCE ? line_source_value(source, mapped_arc, meta.y, valid) :
                                         line_aov_value(source, mapped_arc, meta.y, meta.w != 0, a, valid);
      if (meta.z == 1) { result = float4(result.a); }
      else if (meta.z == 2) { result = float4(float(valid)); }
      else if (meta.z == 3) { result = float4(result.r); }
    }
    else if (op.x == LINE_FIELD_CONVERT) {
      result = a;
      /* SOCK_FLOAT=0, VECTOR=1, RGBA=2, BOOLEAN=4, INT=6. */
      if (meta.y == 0 || meta.y == 6 || meta.y == 4) {
        float value = meta.z == 1 ? dot(a.xyz, float3(1.0f / 3.0f)) : meta.z == 2 ? dot(a.xyz, float3(0.2126f, 0.7152f, 0.0722f)) : a.x;
        result = float4(meta.y == 4 ? float(value != 0.0f) : meta.y == 6 ? trunc(value) : value);
      }
      else if (meta.y == 2) { result.a = 1.0f; }
    }
    else if (op.x == LINE_FIELD_MIX) {
      if ((meta.w & 1) != 0) { a = clamp(a, 0.0f, 1.0f); }
      result = mix(b, c, a);
      if (meta.y == 1) { node_mix_add(a.x, b, c, result); }
      else if (meta.y == 2) { node_mix_mult(a.x, b, c, result); }
      else if (meta.y == 3) { node_mix_sub(a.x, b, c, result); }
      else if (meta.y == 4) { node_mix_screen(a.x, b, c, result); }
      else if (meta.y == 5) { node_mix_div_fallback(a.x, b, c, result); }
      else if (meta.y == 6) { node_mix_diff(a.x, b, c, result); }
      else if (meta.y == 7) { node_mix_dark(a.x, b, c, result); }
      else if (meta.y == 8) { node_mix_light(a.x, b, c, result); }
      else if (meta.y == 9) { node_mix_overlay(a.x, b, c, result); }
      else if (meta.y == 10) { node_mix_dodge(a.x, b, c, result); }
      else if (meta.y == 11) { node_mix_burn(a.x, b, c, result); }
      else if (meta.y == 12) { node_mix_hue(a.x, b, c, result); }
      else if (meta.y == 13) { node_mix_sat(a.x, b, c, result); }
      else if (meta.y == 14) { node_mix_val(a.x, b, c, result); }
      else if (meta.y == 15) { node_mix_color(a.x, b, c, result); }
      else if (meta.y == 16) { node_mix_soft(a.x, b, c, result); }
      else if (meta.y == 17) { node_mix_linear(a.x, b, c, result); }
      else if (meta.y == 18) { node_mix_exclusion(a.x, b, c, result); }
      if ((meta.w & 2) != 0) { result = clamp(result, 0.0f, 1.0f); }
    }
    else if (op.x == LINE_FIELD_MAP_RANGE) {
      for (int j = 0; j < 4; j++) {
        float t = c[j] != b[j] ? (a[j] - b[j]) / (c[j] - b[j]) : 0.0f;
        if (meta.y == 1) { t = f[j] > 0.0f ? floor(t * (f[j] + 1.0f)) / f[j] : 0.0f; }
        if (meta.y >= 2) {
          t = clamp(t, 0.0f, 1.0f);
          t = meta.y == 2 ? t * t * (3.0f - 2.0f * t) : t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }
        if (meta.w != 0) { t = clamp(t, 0.0f, 1.0f); }
        result[j] = mix(d[j], e[j], t);
      }
    }
    if (any(isnan(result)) || any(isinf(result))) { result = float4(0); }
    registers[meta.x] = result;
  }
  if ((stage & 1) != 0 && brush.field_program.z >= 0) { position = registers[brush.field_program.z].xyz; }
  if ((stage & 4) != 0 && brush.field_program.w >= 0) { color = registers[brush.field_program.w]; color.a = 1.0f; }
  if ((stage & 2) != 0 && brush.field_outputs.x >= 0) { width = max(registers[brush.field_outputs.x].x, 0.0f); }
  if ((stage & 8) != 0 && brush.field_outputs.y >= 0) { opacity = clamp(registers[brush.field_outputs.y].x, 0.0f, 1.0f); }
  /* A connection gap is a break in drawing, rather than an extra source segment. */
  if ((stage & LINE_FIELD_ALLOW_GAP_CAPS) == 0 && source.source_range.y != 0u &&
      mapped_arc > source.arc.x + source.arc.y + 1e-4f &&
      source.source_range.x + source.source_range.y - 1u !=
          line_source_part(part, mapped_arc).source_range.z) {
    opacity = 0.0f;
  }
}
