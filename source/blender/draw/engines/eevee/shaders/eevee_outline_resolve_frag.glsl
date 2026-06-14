/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_outline_resolve)

#include "draw_view_lib.glsl"
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

float outline_depth_fetch(int2 texel)
{
  return reverse_z::read(texelFetch(depth_tx, texel, 0).r);
}

float outline_raw_depth_fetch(int2 texel)
{
  return texelFetch(depth_tx, texel, 0).r;
}

float4 outline_velocity_fetch(int2 texel)
{
  return texelFetch(vector_tx, texel, 0);
}

float outline_occlusion_depth_fetch(int2 texel)
{
  return reverse_z::read(texelFetch(outline_occlusion_depth_tx, texel, 0).r);
}

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const float center_depth = outline_depth_fetch(texel);
  const float center_occlusion_depth = outline_occlusion_depth_fetch(texel);
  const uint center_outline_id = outline_id_unpack(outline_source_info_fetch(texel).a);
  float4 outline_pass_color = float4(0.0f);
  float outline_pass_depth = outline_raw_depth_fetch(texel);
  float4 outline_pass_velocity = float4(0.0f);

  const float2 seed_coord = texelFetch(jfa_tx, texel, 0).rg;
  if (seed_coord.x >= -1e9f) {
    const int2 seed_texel = int2(seed_coord);
    const float2 offset = float2(texel) + 0.5f - seed_coord;
    const float offset_length = length(offset) - 0.5f;

    const float4 seed = texelFetch(outline_seed_tx, seed_texel, 0);
    const float seed_width = seed.a;
    if (seed_width > 0.0f && offset_length <= seed_width * 0.5f) {
      const float4 outline_color = outline_source_color_fetch(seed_texel);
      float alpha = outline_color.a;
      if (offset_length <= 0.0f && seed_width <= 1.0f) {
        alpha *= seed_width;
      }
      else {
        alpha *= clamp(seed_width * 0.5f - offset_length, 0.0f, 1.0f);
      }

      const float sample_depth = outline_depth_fetch(seed_texel);
      const float sample_occlusion_depth = outline_occlusion_depth_fetch(seed_texel);
      const uint sample_outline_id = outline_id_unpack(outline_source_info_fetch(seed_texel).a);
      const bool different_outline_id = sample_outline_id != center_outline_id;
      const bool blocked_by_scene_surface = sample_outline_id != center_outline_id &&
                                            center_depth < sample_depth;
      /* In the 5.2 path, raytrace-transmission occluders can already populate the main depth
       * while the background outline ID still seeds the outline buffers. Treat the auxiliary
       * occlusion depth as a foreground mask for JFA seeds generated behind these occluders. */
      const bool different_id_seed_occluded = different_outline_id &&
                                              sample_occlusion_depth < 1.0f - 1e-5f;
      const bool same_id_in_occluder_mask = !different_outline_id &&
                                            (center_occlusion_depth < 1.0f - 1e-5f ||
                                             sample_occlusion_depth < 1.0f - 1e-5f);
      const bool blocked_by_forward_occluder = use_outline_occlusion_depth != 0 &&
                                               (different_id_seed_occluded ||
                                                same_id_in_occluder_mask);

      if (alpha > 0.0f && !(blocked_by_scene_surface || blocked_by_forward_occluder)) {
        outline_pass_color = float4(outline_color.rgb * alpha, alpha);
        outline_pass_depth = outline_raw_depth_fetch(seed_texel);
        outline_pass_velocity = outline_velocity_fetch(seed_texel);
      }
    }
  }
  out_outline_color = outline_pass_color;
  out_outline_depth = outline_pass_depth;
  out_outline_velocity = outline_pass_velocity;
}
