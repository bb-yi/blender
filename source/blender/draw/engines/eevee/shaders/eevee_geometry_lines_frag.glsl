/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_geometry_lines_infos.hh"
FRAGMENT_SHADER_CREATE_INFO(eevee_geometry_lines_draw)
#include "eevee_geometry_lines_lib.glsl"

float coverage_at(float2 pixel, GeometryLineInstanceData instance, GeometryLineBrushData brush, out float t, out float2 local)
{
  t = 0.0f;
  float coverage;
  if ((instance.meta.y & 3u) == 0u) {
    float2 d = instance.b.xy - instance.a.xy;
    float path_t = dot(pixel - instance.a.xy, d) / max(dot(d, d), 1e-8f);
    t = clamp(path_t, 0.0f, 1.0f);
    float radius = mix(instance.a.w, instance.b.w, t);
    float size_alpha = 1.0f;
    if (brush.curve_options.z != 0 && antialiasing > 0.0f) {
      float width = radius * 2.0f;
      float domain = (width < 2.0f ? width : width * 0.5f + 1.0f) *
                     line_brush_envelope(brush, instance.meta.x,
                                         mix(instance.stroke.x, instance.stroke.y, path_t), instance.stroke.z, false);
      /* Raster strokes have a different size response from an analytic capsule.
       * Keep very thin tips as subpixel opacity rather than full-strength pen dots. */
      size_alpha = domain < 1.0f ? domain * domain : 1.0f;
      radius = (domain < 1.0f ? 1.0f : 2.0f * domain - 1.0f) * 0.5f;
    }
    float distance = length(pixel - mix(instance.a.xy, instance.b.xy, t));
    float aa = max(antialiasing, 1e-5f);
    coverage = antialiasing == 0.0f ? float(distance <= radius) : clamp((radius - distance) / aa + 0.5f, 0.0f, 1.0f);
    coverage *= size_alpha;
    local = float2(t, distance / max(radius, 1e-5f));
  }
  else {
    float radius = instance.a.w;
    float size_alpha = 1.0f;
    bool raster_tip = brush.curve_options.z != 0;
    float tip_aa = raster_tip ? min(antialiasing, 1.0f) : antialiasing;
    if (brush.curve_options.z != 0 && antialiasing > 0.0f) {
      float ratio = radius / tip_aa;
      size_alpha = ratio < 1.0f ? ratio * ratio : 1.0f;
      radius = max(radius, tip_aa);
    }
    float2 d = pixel - instance.a.xy;
    float c = cos(instance.stroke.w), s = sin(instance.stroke.w);
    local = float2(c * d.x + s * d.y, -s * d.x + c * d.y) /
            max(float2(radius * brush.tip.w, radius), float2(1e-5f));
    float distance = length(local);
    float aa = max(tip_aa / max(radius, 0.1f), 1e-5f);
    coverage = raster_tip && tip_aa == 0.0f ? float(distance < 1.0f) :
        clamp((1.0f - distance) / max(aa, brush.variation.y) + (raster_tip ? 0.0f : 0.5f), 0.0f, 1.0f);
    float2 uv = local * 0.5f + 0.5f;
    bool inside = all(greaterThanEqual(uv, float2(0.0f))) && all(lessThanEqual(uv, float2(1.0f)));
    bool procedural = (brush.options.w & 2) != 0;
    if (procedural) {
      /* Each 64px mask has a one-texel border; layers keep minification independent. */
      float2 tip_uv = (uv * 64.0f + 1.0f) / 66.0f;
      bool tip_inside = all(greaterThanEqual(tip_uv, float2(0.0f))) &&
                        all(lessThanEqual(tip_uv, float2(1.0f)));
      float3 tip_coordinate = float3(tip_uv, float(instance.meta.y >> 2u));
      /* Raster coverage uses the canonical cached mask, including for minified tips. */
      coverage = !tip_inside ? 0.0f : raster_tip ? textureLod(tip_tx, tip_coordinate, 0.0f).r :
                                                            texture(tip_tx, tip_coordinate).r;
      float ellipse_distance = distance * radius;
      float edge_distance = radius - ellipse_distance;
      if (brush.tip.w != 1.0f && ellipse_distance > 0.0f) {
        edge_distance *= length(d) / ellipse_distance;
      }
      float edge_coverage = tip_aa == 0.0f ? float(edge_distance > 0.0f) :
                               clamp(edge_distance / max(tip_aa, 1e-5f), 0.0f, 1.0f);
      coverage *= edge_coverage;
    }
    if ((brush.options.w & 1) != 0) {
      float map_alpha = inside ? texture(brush_tx, uv).a : 0.0f;
      coverage = procedural ? coverage * map_alpha : map_alpha;
    }
    if (brush.detail_maps.x >= 0.0f) {
      GeometryLineMapData map = line_maps[int(brush.detail_maps.x)];
      float3 sampled;
      if (map.rotation.z == 2.0f) {
        GeometryLinePartData part = source_parts[instance.meta.z];
        float attribute_t = clamp((instance.stroke.x - part.arc.x) / max(part.arc.y, 1e-20f), 0.0f, 1.0f);
        sampled = line_attribute_sample(part.brush_color_a, part.brush_color_b, attribute_t);
      }
      else { sampled = line_map_sample(brush_map_tx, map, uv).rgb; }
      coverage *= mix(1.0f, clamp(dot(sampled, float3(1.0f / 3.0f)), 0.0f, 1.0f), brush.detail_maps.y);
    }
    coverage *= size_alpha;
  }
  return coverage;
}

void main()
{
  GeometryLineInstanceData instance = instances[line_instance];
  if (max(instance.a.w, instance.b.w) <= 0.0f) { discard; }
  if ((instance.meta.y & 3u) != 0u && instance.a.w < 0.05f) { discard; }
  GeometryLineBrushData brush = brushes[instance.meta.x];
  float2 pixel = gl_FragCoord.xy;
  float t;
  float2 local;
  float coverage = coverage_at(pixel, instance, brush, t, local);
#ifdef GEOMETRY_LINES_PACKED_COVERAGE
  float4 sample_coverage[4];
  for (int i = 0; i < 4; i++) { sample_coverage[i] = float4(0.0f); }
#endif
  if (coverage_samples > 1) {
    coverage = 0.0f;
    for (int y = 0; y < coverage_samples; y++) {
      for (int x = 0; x < coverage_samples; x++) {
        float2 offset = (float2(x, y) + 0.5f) / float(coverage_samples) - 0.5f;
        float sample_t;
        float2 sample_local;
        float sample_value = coverage_at(pixel + offset, instance, brush, sample_t, sample_local);
        coverage += sample_value;
#ifdef GEOMETRY_LINES_PACKED_COVERAGE
        int sample_index = y * coverage_samples + x;
        sample_coverage[sample_index / 4][sample_index % 4] = sample_value;
#endif
      }
    }
    coverage /= float(coverage_samples * coverage_samples);
  }
  float arc = mix(instance.stroke.x, instance.stroke.y, t);
  float path_arc = arc;
  if ((instance.meta.y & 3u) == 0u) {
    /* Adjacent Simple capsules overlap to cover curves. Their end caps must not fill the
     * next dash gap or advance opacity to a later segment's start. */
    float2 delta = instance.b.xy - instance.a.xy;
    float path_t = dot(pixel - instance.a.xy, delta) / max(dot(delta, delta), 1e-8f);
    path_arc = mix(instance.stroke.x, instance.stroke.y, path_t);
  }
  float depth = 1.0f - (mix(instance.a.z, instance.b.z, t) * 0.5f + 0.5f);
  if (quick_preview && depth + 2e-5f < texture(preview_depth_tx, pixel / float2(line_extent)).r) {
    discard;
  }
  if (brush.dash.w != 0.0f && mod(path_arc + brush.dash.z, max(brush.dash.x + brush.dash.y, 0.001f)) >= brush.dash.x) {
    discard;
  }
  float opacity = line_brush_envelope(brush, instance.meta.x, path_arc, instance.stroke.z, true);
  float2 reduction = unpackHalf2x16(instance.meta.w);
  opacity *= mix(reduction.x, reduction.y, t);
  if (brush.offset_noise.w > 0.0f) {
    uint stamp_seed = source_parts[instance.meta.z].sampling.w + uint(brush.options.z) +
                      source_parts[instance.meta.z].range.w * 101u +
                      uint(max(floor(instance.stroke.x), 0.0f)) * 7u;
    uint grain_seed = stamp_seed + uint(max(floor((local.x + 2.0f) * 67.0f), 0.0f)) +
                      uint(max(floor((local.y + 2.0f) * 71.0f), 0.0f)) * 131u;
    opacity *= 1.0f - brush.offset_noise.w * line_noise(grain_seed);
  }
  float alpha = clamp(coverage * opacity * brush.tip.y * brush.color.a, 0.0f, 1.0f);
  if (alpha <= 0.0f) {
    discard;
  }
#ifdef GEOMETRY_LINES_PACKED_COVERAGE
  /* MAX each subpixel across all stamps; the resolve averages only afterwards. */
  float factor = opacity * brush.tip.y * brush.color.a;
  out_color = clamp(sample_coverage[0] * factor, 0.0f, 1.0f);
  out_depth = clamp(sample_coverage[1] * factor, 0.0f, 1.0f);
  out_motion = clamp(sample_coverage[2] * factor, 0.0f, 1.0f);
  out_sample_tail = clamp(sample_coverage[3] * factor, 0.0f, 1.0f);
  return;
#endif
  if (color_pass && alpha + max(0.0001f, alpha * 0.001f) < texture(coverage_tx, pixel / float2(line_extent)).r) {
    discard;
  }
  float3 color = brush.color.rgb;
  if (color_pass && brush.maps.z >= 0.0f) {
    GeometryLineMapData map = line_maps[int(brush.maps.z)];
    float2 uv;
    bool valid = true;
    if (map.rotation.z == 2.0f) {
      GeometryLinePartData part = source_parts[instance.meta.z];
      float source_arc = (instance.meta.y & 3u) == 0u ? path_arc : arc;
      float attribute_t = clamp((source_arc - part.arc.x) / max(part.arc.y, 1e-20f), 0.0f, 1.0f);
      color = mix(color, line_attribute_sample(part.color_uv_a, part.color_uv_b, attribute_t), brush.maps.w);
      valid = false; /* The attribute has already been applied; no image lookup. */
    }
    else if (map.rotation.z == 1.0f) {
      uv = float2(pixel.x / float(line_extent.x), 1.0f - pixel.y / float(line_extent.y));
    }
    else {
      float4 a = source_parts[instance.meta.z].color_uv_a;
      float4 b = source_parts[instance.meta.z].color_uv_b;
      valid = a.w != 0.0f && b.w != 0.0f;
      float source_arc = (instance.meta.y & 3u) == 0u ? path_arc : arc;
      float t = clamp((source_arc - source_parts[instance.meta.z].arc.x) /
                      max(source_parts[instance.meta.z].arc.y, 1e-20f), 0.0f, 1.0f);
      float q = mix(a.z, b.z, t);
      uv = mix(a.xy * a.z, b.xy * b.z, t) / max(q, 1e-20f);
    }
    if (valid) {
      float4 sampled = line_map_sample(color_map_tx, map, uv);
      color = mix(color, sampled.rgb, brush.maps.w * clamp(sampled.a, 0.0f, 1.0f));
    }
  }
  out_color = coverage_mask ? float4(alpha) : float4(color * alpha, alpha);
  out_depth = float4(depth, 0.0f, 0.0f, 1.0f);
  /* Match eevee::velocity::pack: native camera effects unpack stored UV motion by 100. */
  out_motion = mix(instance.motion_a, instance.motion_b, t) / float4(line_extent, line_extent) * 0.01f;
}
