/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_detect)

#include "draw_view_lib.glsl"
#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_outline_lib.glsl"

float outline_depth_fetch(int2 texel)
{
  return 1.0f - texelFetch(depth_tx, texel, 0).r;
}

float3 outline_view_position_from_depth(int2 texel, int2 extent, float depth)
{
  const float2 uv = (float2(texel) + 0.5f) / float2(extent);
  return drw_point_screen_to_view(float3(uv, depth));
}

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = textureSize(depth_tx, 0);

  const float4 outline_color = texelFetch(outline_color_tx, texel, 0);
  const float4 outline_info = texelFetch(outline_info_tx, texel, 0);
  const float line_width = outline_width_unpack(outline_info.r);
  const float depth_threshold = pow(outline_info.g, 10.0f) * 999.0f + 1.0f;
  const float normal_threshold = max(0.01f, outline_info.b);

  out_outline_seed = float4(0.0f);
  if (line_width <= 0.0f || outline_color.a <= 0.0f) {
    return;
  }

  const float center_depth = outline_depth_fetch(texel);
  if (center_depth >= 1.0f) {
    return;
  }

  const gbuffer::Header center_header = gbuffer::read_header(texel);
  const bool center_has_gbuffer = !center_header.is_empty();
  const float3 center_normal = center_has_gbuffer ? gbuffer::read_normal(texel) : float3(0.0f);
  const float3 center_vP = outline_view_position_from_depth(texel, extent, center_depth);
  const uint center_outline_id = outline_id_unpack(outline_info.a);

  const int2 offsets[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
  float max_delta_distance = 0.0f;
  float max_delta_angle = 0.0f;
  bool has_id_boundary = false;

  for (int i = 0; i < 4; i++) {
    const int2 offset = offsets[i];
    const int2 sample_texel = texel + offset;
    if (any(lessThan(sample_texel, int2(0))) || any(greaterThanEqual(sample_texel, extent))) {
      continue;
    }

    const float sample_outline_alpha = texelFetch(outline_color_tx, sample_texel, 0).a;
    const float sample_depth = outline_depth_fetch(sample_texel);

    if (sample_outline_alpha <= 0.0f || sample_depth >= 1.0f) {
      has_id_boundary = true;
      continue;
    }

    if (center_depth <= sample_depth) {
      const gbuffer::Header sample_header = gbuffer::read_header(sample_texel);
      const bool sample_has_gbuffer = !sample_header.is_empty();
      const float3 sample_normal = sample_has_gbuffer ? gbuffer::read_normal(sample_texel) :
                                                        float3(0.0f);
      const float3 sample_vP = outline_view_position_from_depth(sample_texel, extent, sample_depth);
      const uint sample_outline_id = outline_id_unpack(
          texelFetch(outline_info_tx, sample_texel, 0).a);

      const float delta_normal = (center_has_gbuffer && sample_has_gbuffer) ?
                                     (1.0f - dot(center_normal, sample_normal)) :
                                     0.0f;
      const float pixel_world_size = max(
          outline_pixel_world_size_at(sample_depth, extent, sample_texel), 1e-6f);
      const float delta_distance = abs(center_vP.z - sample_vP.z) / pixel_world_size;

      max_delta_distance = max(max_delta_distance, delta_distance);
      max_delta_angle = max(max_delta_angle, delta_normal);
      has_id_boundary = has_id_boundary || (sample_outline_id != center_outline_id);
    }
  }

  const bool has_silhouette = has_id_boundary || max_delta_distance > depth_threshold;
  const bool has_internal_edge = max_delta_angle > normal_threshold;

  if (has_silhouette || has_internal_edge) {
    out_outline_seed = float4(outline_color.rgb, has_silhouette ? -line_width : line_width);
  }
}
