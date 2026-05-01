/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_detect)

#include "draw_view_lib.glsl"
#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_outline_lib.glsl"
#include "eevee_reverse_z_lib.glsl"

float4 outline_source_color_fetch(int2 texel)
{
  return texelFetch(outline_color_tx, texel, 0);
}

float4 outline_source_info_fetch(int2 texel)
{
  return texelFetch(outline_info_tx, texel, 0);
}

float outline_screen_depth_fetch(int2 texel)
{
  return reverse_z::read(texelFetch(depth_tx, texel, 0).r);
}

float3 outline_screen_to_view(int2 texel, int2 extent, float screen_depth)
{
  const float2 uv = (float2(texel) + 0.5f) / float2(extent);
  return drw_point_screen_to_view(float3(uv, screen_depth));
}

float3 outline_reconstruct_normal(int2 texel, int2 extent, float3 current_view_direction)
{
  const int2 texel_min = int2(0);
  const int2 texel_max = extent - int2(1);

  const float3 t0 = outline_screen_to_view(texel, extent, outline_screen_depth_fetch(texel));
  const float3 x1 = outline_screen_to_view(
      clamp(texel + int2(-1, 0), texel_min, texel_max),
      extent,
      outline_screen_depth_fetch(clamp(texel + int2(-1, 0), texel_min, texel_max)));
  const float3 x2 = outline_screen_to_view(
      clamp(texel + int2(1, 0), texel_min, texel_max),
      extent,
      outline_screen_depth_fetch(clamp(texel + int2(1, 0), texel_min, texel_max)));
  const float3 y1 = outline_screen_to_view(
      clamp(texel + int2(0, -1), texel_min, texel_max),
      extent,
      outline_screen_depth_fetch(clamp(texel + int2(0, -1), texel_min, texel_max)));
  const float3 y2 = outline_screen_to_view(
      clamp(texel + int2(0, 1), texel_min, texel_max),
      extent,
      outline_screen_depth_fetch(clamp(texel + int2(0, 1), texel_min, texel_max)));

  const float x_distance_1 = abs(x1.z - t0.z);
  const float x_distance_2 = abs(x2.z - t0.z);
  const float y_distance_1 = abs(y1.z - t0.z);
  const float y_distance_2 = abs(y2.z - t0.z);

  const float3 x = (x_distance_1 < x_distance_2) ? x1 : x2;
  const float3 y = (y_distance_1 < y_distance_2) ? y1 : y2;

  float3 n = normalize(cross(x - t0, y - t0));
  n = (dot(n, current_view_direction) < 0.0f) ? n : -n;
  return drw_normal_view_to_world(n);
}

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = textureSize(depth_tx, 0);

  const float4 outline_color = outline_source_color_fetch(texel);
  const float4 outline_info = outline_source_info_fetch(texel);
  const float line_width = outline_width_unpack(outline_info.r);
  const bool use_depth_outline = outline_info.g < 1.0f;
  const bool use_normal_outline = outline_info.b < 1.0f;
  const bool use_geometry_outline = use_depth_outline || use_normal_outline;
  const bool center_id_edge = outline_id_edge_unpack(outline_info.a);
  const float depth_threshold = use_depth_outline ? pow(outline_info.g, 10.0f) * 999.0f +
                                                       1.0f :
                                                   0.0f;
  const float normal_threshold = max(0.01f, outline_info.b);

  out_outline_seed = float4(0.0f);
  if (line_width <= 0.0f || outline_color.a <= 0.0f) {
    return;
  }
  if (!center_id_edge && !use_geometry_outline) {
    return;
  }

  const float center_depth = outline_screen_depth_fetch(texel);
  if (center_depth >= 1.0f) {
    return;
  }

  const uint center_outline_id = outline_id_unpack(outline_info.a);

  float3 center_position = float3(0.0f);
  float3 center_normal = float3(0.0f);
  float3 true_normal_camera = float3(0.0f);
  float3 current_view_direction = float3(0.0f);
  bool mask_only_outline = false;

  if (use_geometry_outline) {
    const gbuffer::Header center_header = gbuffer::read_header(texel);
    const bool center_has_gbuffer = !center_header.is_empty();
    center_position = outline_screen_to_view(texel, extent, center_depth);
    const float2 center_uv = (float2(texel) + 0.5f) / float2(extent);
    current_view_direction = drw_point_screen_to_view(float3(center_uv, 1.0f));

    float3 true_normal = float3(0.0f);
    for (int x = -1; x <= 1; x++) {
      for (int y = -1; y <= 1; y++) {
        const int2 sample_texel = texel + int2(x, y);
        if (any(lessThan(sample_texel, int2(0))) || any(greaterThanEqual(sample_texel, extent))) {
          continue;
        }
        true_normal += outline_reconstruct_normal(sample_texel, extent, current_view_direction);
      }
    }
    true_normal = normalize(true_normal);
    if (any(isnan(true_normal))) {
      return;
    }
    const float3 center_gbuffer_normal = center_has_gbuffer ? gbuffer::read_normal(texel) :
                                                              true_normal;
    const float center_normal_alignment = dot(center_gbuffer_normal, true_normal);
    center_normal = (center_has_gbuffer && center_normal_alignment > 0.5f) ?
                        center_gbuffer_normal :
                        true_normal;
    mask_only_outline = !(center_has_gbuffer && center_normal_alignment > 0.5f);
    true_normal_camera = drw_normal_world_to_view(true_normal);
  }

  const int2 offsets[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
  float max_delta_distance = 0.0f;
  float max_delta_angle = 0.0f;
  bool has_id_edge = false;
  float seed_line_width = line_width;

  for (int i = 0; i < 4; i++) {
    const int2 offset = offsets[i];
    const int2 sample_texel = texel + offset;
    if (any(lessThan(sample_texel, int2(0))) || any(greaterThanEqual(sample_texel, extent))) {
      continue;
    }

    const float sample_depth = outline_screen_depth_fetch(sample_texel);
    const bool center_is_not_behind_sample = center_depth <= sample_depth + 1.0e-5f;

    if (center_id_edge && center_is_not_behind_sample) {
      const float4 sample_outline_info = outline_source_info_fetch(sample_texel);
      const float sample_line_width = outline_width_unpack(sample_outline_info.r);
      const uint sample_outline_id = outline_id_unpack(sample_outline_info.a);
      if (sample_outline_id != center_outline_id) {
        has_id_edge = true;
        seed_line_width = max(seed_line_width, sample_line_width);
      }
    }

    if (use_geometry_outline && !mask_only_outline && center_is_not_behind_sample) {
      const bool sample_has_gbuffer = !gbuffer::read_header(sample_texel).is_empty();
      const float3 sample_true_normal = outline_reconstruct_normal(
          sample_texel, extent, current_view_direction);
      const float3 sample_gbuffer_normal = sample_has_gbuffer ? gbuffer::read_normal(sample_texel) :
                                                              sample_true_normal;
      const float sample_normal_alignment = dot(sample_gbuffer_normal, sample_true_normal);
      const float3 sample_normal = (sample_has_gbuffer && sample_normal_alignment > 0.5f) ?
                                       sample_gbuffer_normal :
                                       sample_true_normal;
      const float3 sample_position = outline_screen_to_view(sample_texel, extent, sample_depth);
      const float delta_normal = dot(center_normal, sample_normal);
      const float plane_distance = dot(true_normal_camera, center_position);
      const float sample_plane_distance = dot(true_normal_camera, sample_position);
      const float pixel_world_size = max(
          outline_pixel_world_size_at(sample_depth, extent, sample_texel), 1e-6f);
      const float delta_distance = abs(plane_distance - sample_plane_distance) / pixel_world_size;

      if (use_depth_outline) {
        max_delta_distance = max(max_delta_distance, delta_distance);
      }
      if (use_normal_outline) {
        max_delta_angle = max(max_delta_angle, 1.0f - delta_normal);
      }
    }
  }

  const bool has_silhouette = has_id_edge ||
                              (!mask_only_outline && use_depth_outline &&
                               max_delta_distance > depth_threshold);
  const bool has_internal_edge = !mask_only_outline && use_normal_outline &&
                                 max_delta_angle > normal_threshold;

  if (has_silhouette || has_internal_edge) {
    out_outline_seed = float4(outline_color.rgb, seed_line_width);
  }
}
