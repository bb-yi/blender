/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

#ifdef OUTLINE_DOWNSAMPLED
#  ifdef OUTLINE_JFA_PACKED
FRAGMENT_SHADER_CREATE_INFO(eevee_outline_resolve_scaled_packed)
#  else
FRAGMENT_SHADER_CREATE_INFO(eevee_outline_resolve_scaled)
#  endif
#else
#ifdef OUTLINE_JFA_PACKED
FRAGMENT_SHADER_CREATE_INFO(eevee_outline_resolve_packed)
#else
FRAGMENT_SHADER_CREATE_INFO(eevee_outline_resolve)
#endif
#endif

#include "draw_view_lib.glsl"
#include "eevee_outline_lib.glsl"
#include "eevee_reverse_z_lib.glsl"

float4 outline_source_color_fetch(int2 texel)
{
#ifdef OUTLINE_DOWNSAMPLED
  texel = outline_source_texel(texel, textureSize(outline_color_tx, 0), outline_extent);
#endif
  return texelFetch(outline_color_tx, texel, 0);
}

uint4 outline_source_info_fetch(int2 texel)
{
#ifdef OUTLINE_DOWNSAMPLED
  texel = outline_source_texel(texel, textureSize(outline_info_tx, 0).xy, outline_extent);
#endif
  return texelFetch(outline_info_tx, int3(texel, 0), 0);
}

uint outline_identity_fetch(int2 texel, bool source)
{
  if (textureSize(outline_info_tx, 0).z <= 2) {
    return outline_id_unpack(outline_source_info_fetch(texel).a);
  }
#ifdef OUTLINE_DOWNSAMPLED
  texel = outline_source_texel(texel, textureSize(depth_tx, 0), outline_extent);
#endif
  const uint2 identity = texelFetch(outline_info_tx, int3(texel, 2), 0).rg;
  return source ? identity.r : identity.g;
}

float outline_raw_depth_fetch(int2 texel)
{
  if (outline_width_unpack(outline_source_info_fetch(texel).r) > 0.0f &&
      outline_source_color_fetch(texel).a > 0.0f)
  {
#ifdef OUTLINE_DOWNSAMPLED
    texel = outline_source_texel(texel, textureSize(depth_tx, 0), outline_extent);
#endif
    return uintBitsToFloat(texelFetch(outline_info_tx, int3(texel, 1), 0).r);
  }
#ifdef OUTLINE_DOWNSAMPLED
  texel = outline_source_texel(texel, textureSize(depth_tx, 0), outline_extent);
#endif
  return texelFetch(depth_tx, texel, 0).r;
}

float4 outline_velocity_fetch(int2 texel)
{
#ifdef OUTLINE_DOWNSAMPLED
  texel = outline_source_texel(texel, textureSize(depth_tx, 0), outline_extent);
#endif
  return texelFetch(vector_tx, texel, 0);
}

float outline_occlusion_depth_fetch(int2 texel)
{
#ifdef OUTLINE_DOWNSAMPLED
  texel = outline_source_texel(texel, textureSize(depth_tx, 0), outline_extent);
#endif
  return texelFetch(outline_occlusion_depth_tx, texel, 0).r;
}

bool outline_surface_in_front(float occluder_depth, float source_depth)
{
  /* Compare raw reverse-Z so distant surfaces retain their depth precision. */
  const float tolerance = max(abs(source_depth) * 2e-6f, 1e-10f);
  return occluder_depth > source_depth + tolerance;
}

float outline_scene_depth_fetch(int2 texel)
{
#ifdef OUTLINE_DOWNSAMPLED
  texel = outline_source_texel(texel, textureSize(depth_tx, 0), outline_extent);
#endif
  return texelFetch(depth_tx, texel, 0).r;
}

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const float center_occlusion_depth = outline_occlusion_depth_fetch(texel);
  const uint center_outline_id = outline_identity_fetch(texel, false);
  float4 outline_pass_color = float4(0.0f);
  float outline_pass_depth = outline_raw_depth_fetch(texel);
  float4 outline_pass_velocity = float4(0.0f);

  float2 seed_coord = float2(-1e10f);
  if ((outline_flags.x & OUTLINE_NEEDS_JFA) == 0u) {
    /* With full width <= 1, every other pixel has zero coverage. A valid seed always wins
     * at its own center (distance zero), so no nearest-seed lookup is needed. */
    const float4 center_seed = texelFetch(outline_seed_tx, texel, 0);
    if (center_seed.a > 0.0f && center_seed.r > 0.0f) {
      seed_coord = float2(texel) + 0.5f;
    }
  }
  else {
#ifdef OUTLINE_JFA_PACKED
    const uint packed_coord = texelFetch(jfa_tx, texel, 0).r;
    if (packed_coord != 0xffffffffu) {
      seed_coord = float2(uint2(packed_coord & 0xffffu, packed_coord >> 16u)) + 0.5f;
    }
#else
    seed_coord = texelFetch(jfa_tx, texel, 0).rg;
#endif
  }
  if (seed_coord.x >= -1e9f) {
    const int2 seed_texel = int2(seed_coord);
    const float2 offset = float2(texel) + 0.5f - seed_coord;
    const float offset_length = length(offset) - 0.5f;

    const float4 seed = texelFetch(outline_seed_tx, seed_texel, 0);
    /* .a = full line width.
     * .r = per-seed width modulation factor in (0, 1], already smoothed along the contour by the
     * factor-blur pass (eevee_outline_factor_blur), so the drawn radius varies smoothly along the
     * stroke without the per-Voronoi-cell sawtooth. JFA uses the effective width in its
     * distance metric so a narrow seed does not erase a wider neighbor's coverage. */
    const float seed_width = seed.a;
    const float width_factor = clamp(seed.r, 0.0f, 1.0f);
    const float effective_width = seed_width * width_factor;
    if (effective_width > 0.0f && offset_length <= effective_width * 0.5f) {
      const float4 outline_color = outline_source_color_fetch(seed_texel);
      float alpha = outline_color.a;
      if (offset_length <= 0.0f && effective_width <= 1.0f) {
        alpha *= effective_width;
      }
      else {
        alpha *= clamp(effective_width * 0.5f - offset_length, 0.0f, 1.0f);
      }
#ifndef OUTLINE_DOWNSAMPLED
      alpha *= 1.0f - outline_expansion_block_unpack(outline_source_info_fetch(texel).a);
#endif

      const float sample_depth = outline_raw_depth_fetch(seed_texel);
      const float sample_occlusion_depth = outline_occlusion_depth_fetch(seed_texel);
      const uint sample_outline_id = outline_identity_fetch(seed_texel, true);
      const bool different_outline_id = sample_outline_id != center_outline_id;
      /* The main depth may belong to a later transmission surface while the outline style
       * still belongs to the surface behind it. Always compare against the source's depth. */
      const bool blocked_by_scene_surface =
          outline_surface_in_front(outline_scene_depth_fetch(seed_texel), sample_depth) ||
          (different_outline_id &&
           outline_surface_in_front(outline_scene_depth_fetch(texel), sample_depth));
      const bool blocked_by_forward_occluder = use_outline_occlusion_depth != 0 &&
          (outline_surface_in_front(center_occlusion_depth, sample_depth) ||
           outline_surface_in_front(sample_occlusion_depth, sample_depth));

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
#ifdef OUTLINE_DOWNSAMPLED
  out_outline_id = outline_pass_color.a > 0.0f ?
                       outline_identity_fetch(int2(seed_coord), true) : 0u;
#endif
}
