/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_upsample)

#include "eevee_outline_lib.glsl"

bool outline_surface_in_front(float occluder_depth, float source_depth)
{
  const float tolerance = max(abs(source_depth) * 2e-6f, 1e-10f);
  return occluder_depth > source_depth + tolerance;
}

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = textureSize(depth_tx, 0);
  const int2 reduced_extent = textureSize(reduced_color_tx, 0);
  const float2 sample_position = gl_FragCoord.xy * float2(reduced_extent) / float2(extent) - 0.5f;
  const int2 sample_base = int2(floor(sample_position));
  const float2 sample_weight = fract(sample_position);
  const float scene_depth = texelFetch(depth_tx, texel, 0).r;
  const uint center_info_a = texelFetch(outline_info_tx, int3(texel, 0), 0).a;
  const uint center_id = textureSize(outline_info_tx, 0).z > 2 ?
                            texelFetch(outline_info_tx, int3(texel, 2), 0).g :
                            outline_id_unpack(center_info_a);
  const float occlusion_depth = texelFetch(outline_occlusion_depth_tx, texel, 0).r;

  float4 color = float4(0.0f);
  float depth = scene_depth;
  float4 velocity = float4(0.0f);
  float strongest_weight = 0.0f;
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      const int2 sample_texel = clamp(sample_base + int2(x, y), int2(0), reduced_extent - 1);
      const float4 sample_color = texelFetch(reduced_color_tx, sample_texel, 0);
      if (sample_color.a <= 0.0f) {
        continue;
      }
      const float sample_depth = texelFetch(reduced_depth_tx, sample_texel, 0).r;
      const uint sample_id = texelFetch(reduced_id_tx, sample_texel, 0).r;
      /* Recheck visibility at full resolution so interpolation cannot cross a foreground edge. */
      if ((sample_id != center_id && outline_surface_in_front(scene_depth, sample_depth)) ||
          (use_outline_occlusion_depth != 0 &&
           outline_surface_in_front(occlusion_depth, sample_depth)))
      {
        continue;
      }
      const float weight = (x == 0 ? 1.0f - sample_weight.x : sample_weight.x) *
                           (y == 0 ? 1.0f - sample_weight.y : sample_weight.y);
      color += sample_color * weight;
      /* Blend premultiplied color only. Keep depth and velocity from one covered source. */
      const float coverage = sample_color.a * weight;
      if (coverage > strongest_weight) {
        strongest_weight = coverage;
        depth = sample_depth;
        velocity = texelFetch(reduced_velocity_tx, sample_texel, 0);
      }
    }
  }
  color *= 1.0f - outline_expansion_block_unpack(center_info_a);
  if (color.a <= 0.0f) {
    depth = scene_depth;
    velocity = float4(0.0f);
  }
  out_outline_color = color;
  out_outline_depth = depth;
  out_outline_velocity = velocity;
}
