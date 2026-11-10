/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_geometry_lines_infos.hh"
COMPUTE_SHADER_CREATE_INFO(eevee_geometry_lines_expand)
#include "eevee_geometry_lines_lib.glsl"
#include "eevee_geometry_lines_fields_lib.glsl"

float3 line_world_position(float3 pixel)
{
  float4 p = world_from_clip * float4(pixel.xy / float2(line_extent) * 2.0f - 1.0f, pixel.z, 1.0f);
  return p.xyz / (abs(p.w) > 1e-20f ? p.w : 1e-20f);
}

float line_distance_factor(uint index, float3 position)
{
  if (index == 0xffffffffu || reductions[index].domain.z == 0.0f) { return 1.0f; }
  float distance = length(position - reductions[index].origin.xyz);
  float inverse_span = reductions[index].domain.x;
  float normalized = inverse_span == 0.0f ? float(distance >= reductions[index].domain.y) :
                     (distance - reductions[index].origin.w) * inverse_span;
  float t = clamp(normalized, 0.0f, 1.0f) * 255.0f;
  int a = int(t);
  return mix(reductions[index].values[a], reductions[index].values[min(a + 1, 255)], t - float(a));
}

float line_scale_width(float radius, float factor, GeometryLineBrushData brush)
{
  if (brush.curve_options.z == 0 || !raster_response) { return radius * factor; }
  float w = radius * 2.0f;
  float domain = (w < 2.0f ? w : w * 0.5f + 1.0f) * factor;
  return (domain < 2.0f ? domain : 2.0f * domain - 2.0f) * 0.5f;
}

float3 line_point(GeometryLinePartData part, GeometryLineBrushData brush, float t)
{
  float original_arc = part.arc.x + part.arc.y * t;
  float3 local_position;
  float4 field_color;
  float field_width, field_opacity;
  line_fields(brush, part, original_arc, original_arc, 1, local_position, field_color, field_width, field_opacity);
  float mapped_arc = local_position.x;
  /* An unchanged X is an exact passthrough. Reconstructing t from rounded
   * cumulative arc lengths needlessly moves short source segments. */
  if (brush.field_outputs.w != 0 && mapped_arc != original_arc) {
    part = line_source_part(part, mapped_arc);
    if ((part.meta.y & 1u) != 0u) { mapped_arc = mod(mapped_arc, max(part.arc.z, 1e-8f)); }
    /* Allow tangent extrapolation only at an open path's true ends. */
    t = (mapped_arc - part.arc.x) / max(part.arc.y, 1e-20f);
  }
  float3 p = mix(part.a.xyz, part.b.xyz, t);
  float arc = part.arc.x + part.arc.y * t;
  float2 delta = part.b.xy - part.a.xy;
  float2 tangent = delta / max(length(delta), 1e-8f);
  float2 normal = float2(-tangent.y, tangent.x);
  float2 path_normal = mix(part.offset_normals.xy, part.offset_normals.zw, t);
  if (dot(path_normal, path_normal) < 1e-12f) { path_normal = normal; }
  uint seed = part.meta.x + uint(brush.options.z) + part.range.w * 27u;
  float progress = arc / max(part.arc.z, 1e-8f);
  float offset = brush.deformation.y + (line_noise(seed + 131u) * 2.0f - 1.0f) * brush.wave_options.z;
  float legacy_offset = 0.0f;
  if (brush.wave_options.y != 0.0f) {
    offset += line_pixel_wave(brush, arc, p.xy, seed);
  }
  else {
    /* Keep the original per-stroke cycle/noise pattern for saved legacy brushes. */
    legacy_offset = sin(progress * 6.283185307f * brush.deformation.w) * brush.deformation.z +
                    (line_noise(seed + uint(max(floor(arc / 8.0f), 0.0f))) * 2.0f - 1.0f) * brush.offset_noise.z;
  }
  if (brush.detail_maps.z >= 0.0f) {
    GeometryLineMapData map = line_maps[int(brush.detail_maps.z)];
    bool attribute_source = map.rotation.z == 2.0f;
    bool valid = map.rotation.z == 1.0f || (part.distortion_uv_a.w != 0.0f && part.distortion_uv_b.w != 0.0f);
    if (attribute_source) {
      float3 mapped = line_attribute_sample(part.distortion_uv_a, part.distortion_uv_b, t);
      offset += clamp(dot(mapped, float3(1.0f / 3.0f)), 0.0f, 1.0f) * brush.detail_maps.w;
    }
    else if (valid) {
      float2 uv;
      if (map.rotation.z == 1.0f) {
        uv = float2(p.x / float(line_extent.x), 1.0f - p.y / float(line_extent.y));
      }
      else {
        float q = mix(part.distortion_uv_a.z, part.distortion_uv_b.z, t);
        uv = mix(part.distortion_uv_a.xy * part.distortion_uv_a.z,
                 part.distortion_uv_b.xy * part.distortion_uv_b.z, t) / max(q, 1e-20f);
      }
      float3 mapped = line_map_sample(distortion_map_tx, map, uv).rgb;
      offset += clamp(dot(mapped, float3(1.0f / 3.0f)), 0.0f, 1.0f) * brush.detail_maps.w;
    }
  }
  p.xy += path_normal * (offset + local_position.y) + normal * legacy_offset + brush.offset_noise.xy;
  if ((part.meta.y & 1u) == 0u && (part.meta.y & 2u) != 0u && t <= 0.0f) {
    p.xy -= tangent * brush.deformation.x;
  }
  if ((part.meta.y & 1u) == 0u && (part.meta.y & 4u) != 0u && t >= 1.0f) {
    p.xy += tangent * brush.deformation.x;
  }
  return p;
}

void main()
{
  uint index = gl_GlobalInvocationID.x;
  if (index >= uint(instance_count)) {
    return;
  }
  int lo = 0, hi = part_count;
  while (lo + 1 < hi) {
    int mid = (lo + hi) / 2;
    if (parts[mid].range.x <= index) { lo = mid; }
    else { hi = mid; }
  }
  GeometryLinePartData part = parts[lo];
  GeometryLineBrushData brush = brushes[part.range.z];
  uint local = index - part.range.x;
  uint sample_index = local;
  bool simple = brush.options.x == 0;
  uint grid_index = min(part.sampling.x + sample_index * part.sampling.y, part.sampling.z);
  float t0 = float(grid_index) / float(max(part.sampling.z, 1u));
  /* Derive both endpoints from bounded integer grid positions. Adding a float
   * step to the last t0 can round above 1 and extrapolate a terminal field. */
  uint next_grid_index = min(grid_index + 1u, part.sampling.z);
  float t1 = simple ? float(next_grid_index) / float(max(part.sampling.z, 1u)) : t0;
  float s0 = part.arc.x + part.arc.y * t0;
  float s1 = part.arc.x + part.arc.y * t1;
  /* A negative length marks a loop: explicit curves still address its full
   * domain, while the legacy open-end taper remains disabled. */
  float path_length = (part.meta.y & 1u) == 0u ? part.arc.z : -part.arc.z;
  uint seed = part.meta.x + uint(brush.options.z);
  float radius = brush.tip.x * width_scale * part.arc.w * 0.5f;
  radius *= 1.0f - brush.variation.z * line_noise(seed);
  float3 a = line_point(part, brush, t0);
  float3 b = line_point(part, brush, t1);
  float2 tangent = line_point(part, brush, min(t0 + 0.001f, 1.0f)).xy -
                   line_point(part, brush, max(t0 - 0.001f, 0.0f)).xy;
  float angle = (brush.curve_options.w == 0 ? atan(tangent.y, tangent.x) : 0.0f) +
                (brush.variation.x + part.a.w) * 0.01745329252f;
  uint tip_layer = part.meta.y >> 8u;
  if (!simple && brush.curve_options.w == 2) {
    float2 point = float2(a.x, float(line_extent.y) - a.y);
    uint random = line_tip_random_entry(uint((int(point.y) * 2697 + int(point.x)) * 3));
    uint warmup = uint(int(point.x - point.y)) & 3u;
    for (uint i = 0u; i < warmup; i++) { random = line_tip_random_next(random); }
    random = line_tip_random_next(random);
    tip_layer = min(9u, uint(float(random) * (10.0f / 65536.0f)));
    random = line_tip_random_next(random);
    angle -= (float(random) / 32768.0f - 1.0f) * 3.1415927410125732f;
  }
  GeometryLineInstanceData instance_data;
  float3 position_a, position_b;
  float4 field_color_a, field_color_b;
  float field_width_a, field_width_b, field_opacity_a, field_opacity_b;
  line_fields(brush, part, s0, s0, 1, position_a, field_color_a, field_width_a, field_opacity_a);
  line_fields(brush, part, s1, s1, 1, position_b, field_color_b, field_width_b, field_opacity_b);
  /* Re-evaluate source fields at the X-remapped source location. */
  float3 unused_position;
  line_fields(brush, part, s0, position_a.x, 2, unused_position, field_color_a, field_width_a, field_opacity_a);
  line_fields(brush, part, s1, position_b.x, 2, unused_position, field_color_b, field_width_b, field_opacity_b);
  instance_data.field_color_a = field_color_a;
  instance_data.field_color_b = field_color_b;
  instance_data.field_parameters = float4(position_a.x, position_b.x, field_opacity_a, field_opacity_b);
  /* Raster Simple pressure is evaluated at the pixel's path projection. Baking it into
   * overlapping capsules lets a wider downstream cap fill an otherwise thin tip. */
  bool pixel_pressure = simple && raster_response && brush.curve_options.z != 0;
  instance_data.a = float4(a, pixel_pressure ? radius : line_scale_width(radius, line_brush_envelope(brush, part.range.z, s0, path_length, false), brush));
  instance_data.b = float4(b, pixel_pressure ? radius : line_scale_width(radius, line_brush_envelope(brush, part.range.z, s1, path_length, false), brush));
  instance_data.a.w *= field_width_a / max(brush.tip.x, 1e-20f);
  instance_data.b.w *= field_width_b / max(brush.tip.x, 1e-20f);
  instance_data.stroke = float4(s0, s1, path_length, angle);
  if (brush.maps.x >= 0.0f) {
    GeometryLineMapData map = line_maps[int(brush.maps.x)];
    float2 factors = float2(1.0f);
    for (int end = 0; end < 2; end++) {
      float t = end == 0 ? t0 : t1;
      if (map.rotation.z == 2.0f) {
        float3 color = line_attribute_sample(part.width_uv_a, part.width_uv_b, t);
        float value = clamp(dot(color, float3(1.0f / 3.0f)), 0.0f, 1.0f);
        factors[end] = mix(1.0f, value, brush.maps.y);
      }
      else if (map.rotation.z == 1.0f || (part.width_uv_a.w != 0.0f && part.width_uv_b.w != 0.0f)) {
        float2 uv;
        if (map.rotation.z == 1.0f) {
          float2 pixel = end == 0 ? a.xy : b.xy;
          uv = float2(pixel.x / float(line_extent.x), 1.0f - pixel.y / float(line_extent.y));
        }
        else {
          float q = mix(part.width_uv_a.z, part.width_uv_b.z, t);
          uv = mix(part.width_uv_a.xy * part.width_uv_a.z, part.width_uv_b.xy * part.width_uv_b.z, t) / max(q, 1e-20f);
        }
        float3 color = line_map_sample(width_map_tx, map, uv).rgb;
        float value = clamp(dot(color, float3(1.0f / 3.0f)), 0.0f, 1.0f);
        factors[end] = mix(1.0f, value, brush.maps.y);
      }
    }
    for (int end = 0; end < 2; end++) {
      float r = end == 0 ? instance_data.a.w : instance_data.b.w;
      r = line_scale_width(r, factors[end], brush);
      if (end == 0) { instance_data.a.w = r; }
      else { instance_data.b.w = r; }
    }
  }
  float2 alpha_factors = float2(1.0f);
  if (part.meta.z != 0xffffffffu || part.meta.w != 0xffffffffu) {
    float3 world_a = line_world_position(mix(part.a.xyz, part.b.xyz, t0));
    float3 world_b = simple ? line_world_position(mix(part.a.xyz, part.b.xyz, t1)) : world_a;
    instance_data.a.w = line_scale_width(instance_data.a.w, line_distance_factor(part.meta.z, world_a), brush);
    instance_data.b.w = line_scale_width(instance_data.b.w, line_distance_factor(part.meta.z, world_b), brush);
    alpha_factors = float2(line_distance_factor(part.meta.w, world_a), line_distance_factor(part.meta.w, world_b));
  }
  if (!simple && brush.curve_options.z != 0) {
    /* Keep pressure/maps/reduction composable in the encoded domain, then apply
     * the textured raster's physical radius of (width + AA + 1)/2. */
    float width = radius * 2.0f;
    float aa = clamp(raster_antialiasing, 0.0f, 1.0f);
    float base_radius = (width < 2.0f ? (aa + 1.0f) * width : width + aa + 1.0f) * 0.5f;
    float base_domain = width < 2.0f ? width : width * 0.5f + 1.0f;
    for (int end = 0; end < 2; end++) {
      float value = end == 0 ? instance_data.a.w : instance_data.b.w;
      float factor;
      if (raster_response) {
        float encoded_width = value * 2.0f;
        float domain = encoded_width < 2.0f ? encoded_width : encoded_width * 0.5f + 1.0f;
        factor = domain / max(base_domain, 1e-20f);
      }
      else { factor = value / max(radius, 1e-20f); }
      value = base_radius * factor;
      if (end == 0) { instance_data.a.w = value; }
      else { instance_data.b.w = value; }
    }
  }
  float2 previous_a = mix(part.previous_a.xy, part.previous_b.xy, t0);
  float2 previous_b = mix(part.previous_a.xy, part.previous_b.xy, t1);
  float2 next_a = mix(part.next_a.xy, part.next_b.xy, t0);
  float2 next_b = mix(part.next_a.xy, part.next_b.xy, t1);
  float2 current_a = mix(part.a.xy, part.b.xy, t0);
  float2 current_b = mix(part.a.xy, part.b.xy, t1);
  instance_data.motion_a = float4(previous_a - current_a, current_a - next_a);
  instance_data.motion_b = float4(previous_b - current_b, current_b - next_b);
  instance_data.meta = uint4(part.range.z, uint(brush.options.x) | (tip_layer << 2u), uint(lo), packHalf2x16(alpha_factors));
  instances[index] = instance_data;
}
