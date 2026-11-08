/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

uint line_tip_random_next(uint state)
{
  return (state * 2061u + 7u) & 65535u;
}

uint line_tip_random_entry(uint index)
{
  /* The table is an affine 16-bit sequence. Skip ahead without a GPU lookup buffer. */
  uint count = (index & 65535u) + 1u;
  uint value = 0u, multiplier = 2061u, addend = 7u;
  for (int bit = 0; bit < 17; bit++) {
    if ((count & 1u) != 0u) { value = (value * multiplier + addend) & 65535u; }
    addend = (addend * multiplier + addend) & 65535u;
    multiplier = (multiplier * multiplier) & 65535u;
    count >>= 1u;
  }
  return value;
}

float line_noise(uint seed)
{
  seed ^= seed >> 16u;
  seed *= 0x7feb352du;
  seed ^= seed >> 15u;
  seed *= 0x846ca68bu;
  seed ^= seed >> 16u;
  return float(seed >> 8u) * (1.0f / 16777216.0f);
}

float3 line_attribute_sample(float4 a, float4 b, float t)
{
  if (a.w <= 0.0f || b.w <= 0.0f) { return float3(0.0f); }
  return mix(a.xyz * a.w, b.xyz * b.w, t) / max(mix(a.w, b.w, t), 1e-20f);
}

float line_wave_noise(float2 position, float cell_size, uint seed)
{
  float result = 0.0f;
  for (int side = 0; side < 2; side++) {
    float2 p = position / max(cell_size, 0.001f) + (side == 0 ? -0.25f : 0.25f);
    int2 cell = int2(floor(p));
    float2 t = fract(p);
    uint base = seed + uint(cell.x) * 73856093u + uint(cell.y) * 19349663u;
    float a = mix(line_noise(base), line_noise(base + 73856093u), t.x);
    float b = mix(line_noise(base + 19349663u), line_noise(base + 73856093u + 19349663u), t.x);
    result += mix(a, b, t.y) * 2.0f - 1.0f;
  }
  return result * 0.5f;
}

float line_pixel_wave(GeometryLineBrushData brush, float arc, float2 position, uint seed)
{
  float phase = arc * 6.2831854820251465f / max(brush.wave_settings.x, 5.0f) + brush.wave_settings.y;
  if (brush.wave_options.x > 0.0f) {
    phase += (line_noise(seed + 91u) * 2.0f - 1.0f) * brush.wave_options.x * 6.2831854820251465f;
  }
  if (brush.wave_settings.w > 0.0f) {
    phase += line_wave_noise(position, brush.wave_settings.x * 0.5f, seed + 7777u) *
               brush.wave_settings.w * 3.1415927410125732f;
  }
  float amplitude = brush.deformation.z;
  if (brush.wave_settings.z > 0.0f) {
    amplitude *= 1.0f + line_wave_noise(position, brush.wave_settings.x * 0.5f, seed + 77u) * brush.wave_settings.z;
  }
  return sin(phase) * amplitude;
}

float line_envelope(float arc, float path_length, float start, float end)
{
  if (path_length <= 0.0f) {
    return 1.0f;
  }
  float a = start > 0.0f ? clamp(arc / max(path_length * start, 1e-6f), 0.0f, 1.0f) : 1.0f;
  float b = end > 0.0f ? clamp((path_length - arc) / max(path_length * end, 1e-6f), 0.0f, 1.0f) : 1.0f;
  return a * b;
}

float line_brush_envelope(GeometryLineBrushData brush, uint brush_index, float arc, float path_length, bool opacity)
{
  if (path_length <= 0.0f) { return 1.0f; }
  bool enabled = opacity ? brush.curve_options.y != 0 : brush.curve_options.x != 0;
  if (!enabled) {
    return opacity ? line_envelope(arc, path_length, brush.taper.z, brush.taper.w) :
                     line_envelope(arc, path_length, brush.taper.x, brush.taper.y);
  }
  /* The calibrated raster path addresses 256 table cells, with the final entry
   * extending through the endpoint. Analytic brushes keep their original domain. */
  float position = min(clamp(arc / path_length, 0.0f, 1.0f) *
                       (brush.curve_options.z != 0 ? 256.0f : 255.0f), 255.0f);
  int a = int(position), b = min(a + 1, 255);
  float2 values = mix(brush_curves[brush_index].values[a], brush_curves[brush_index].values[b], fract(position));
  return opacity ? values.y : values.x;
}
int line_map_wrap(int value, int size, int mode)
{
  if (mode == 0) { return ((value % size) + size) % size; }
  if (mode == 2) {
    int p = ((value % (size * 2)) + size * 2) % (size * 2);
    return p < size ? p : size * 2 - 1 - p;
  }
  return clamp(value, 0, size - 1);
}

float4 line_map_texel(sampler2D image, int2 p, int2 size, int2 wrap)
{
  if ((wrap.x == 4 && (p.x < 0 || p.x >= size.x)) ||
      (wrap.y == 4 && (p.y < 0 || p.y >= size.y))) { return float4(0.0f); }
  return texelFetch(image, int2(line_map_wrap(p.x, size.x, wrap.x), line_map_wrap(p.y, size.y, wrap.y)), 0);
}

float4 line_map_sample(sampler2D image, GeometryLineMapData map, float2 uv)
{
  uv = uv * map.transform.xy + map.transform.zw;
  float2 p = uv - 0.5f;
  uv = float2(map.rotation.x * p.x - map.rotation.y * p.y,
              map.rotation.y * p.x + map.rotation.x * p.y) + 0.5f;
  if (any(isnan(uv)) || any(isinf(uv))) { return float4(1.0f); }
  for (int axis = 0; axis < 2; axis++) {
    int wrap = map.sampling[axis];
    if (wrap == 4 && (uv[axis] < 0.0f || uv[axis] > 1.0f)) { return float4(0.0f); }
    if (wrap == 0) { uv[axis] = fract(uv[axis]); }
    else if (wrap == 2) { uv[axis] = 1.0f - abs(mod(uv[axis], 2.0f) - 1.0f); }
    else if (wrap == 3) { uv[axis] = clamp(abs(uv[axis]), 0.0f, 1.0f); }
    else { uv[axis] = clamp(uv[axis], 0.0f, 1.0f); }
  }
  int2 size = textureSize(image, 0);
  if (map.sampling.z == 0) {
    return line_map_texel(image, int2(floor(uv * float2(size))), size, map.sampling.xy);
  }
  float2 position = uv * float2(size) - 0.5f;
  int2 pixel = int2(floor(position));
  float2 t = fract(position);
  return mix(mix(line_map_texel(image, pixel, size, map.sampling.xy),
                 line_map_texel(image, pixel + int2(1, 0), size, map.sampling.xy), t.x),
             mix(line_map_texel(image, pixel + int2(0, 1), size, map.sampling.xy),
                 line_map_texel(image, pixel + int2(1, 1), size, map.sampling.xy), t.x), t.y);
}
