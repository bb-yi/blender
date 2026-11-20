/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

#ifdef OUTLINE_JFA_PACKED
#  ifdef OUTLINE_JFA_VISIBILITY
COMPUTE_SHADER_CREATE_INFO(eevee_outline_jfa_step_visible_packed)
#  else
COMPUTE_SHADER_CREATE_INFO(eevee_outline_jfa_step_packed)
#  endif
#  define JFAState uint2
#else
#  ifdef OUTLINE_JFA_VISIBILITY
COMPUTE_SHADER_CREATE_INFO(eevee_outline_jfa_step_visible)
#  else
COMPUTE_SHADER_CREATE_INFO(eevee_outline_jfa_step)
#  endif
#  define JFAState float2
#endif

#ifdef OUTLINE_JFA_VISIBILITY
int2 outline_full_source_texel(int2 texel)
{
  const int2 source_extent = textureSize(depth_tx, 0);
  return min(int2((float2(texel) + 0.5f) * float2(source_extent) /
                 float2(imageSize(jfa_out_img))), source_extent - int2(1));
}

bool outline_depth_in_front(float depth, float source_depth)
{
  return depth > source_depth + max(abs(source_depth) * 2e-6f, 1e-10f);
}

bool outline_candidate_visible(int2 texel, int2 seed_texel)
{
  const int2 target = outline_full_source_texel(texel);
  const int2 source = outline_full_source_texel(seed_texel);
  const float source_depth = uintBitsToFloat(texelFetch(outline_info_tx, int3(source, 1), 0).r);
  const uint source_id = texelFetch(outline_info_tx, int3(source, 2), 0).r;
  const uint target_id = texelFetch(outline_info_tx, int3(target, 2), 0).g;
  if (outline_depth_in_front(texelFetch(depth_tx, source, 0).r, source_depth) ||
      (source_id != target_id &&
       outline_depth_in_front(texelFetch(depth_tx, target, 0).r, source_depth)))
  {
    return false;
  }
  return use_outline_occlusion_depth == 0 ||
         !(outline_depth_in_front(texelFetch(outline_occlusion_depth_tx, target, 0).r, source_depth) ||
           outline_depth_in_front(texelFetch(outline_occlusion_depth_tx, source, 0).r, source_depth));
}
#endif

float outline_seed_distance_squared(int2 texel, JFAState state)
{
#ifdef OUTLINE_JFA_PACKED
  if (state.x == 0xffffffffu) {
    return 1e20f;
  }
  const float2 seed_coord = float2(uint2(state.x & 0xffffu, state.x >> 16u)) + 0.5f;
  const float width = uintBitsToFloat(state.y);
#else
  const float2 seed_coord = state;
  if (seed_coord.x < -1e9f) {
    return 1e20f;
  }
  const float4 seed = texelFetch(outline_seed_tx, int2(seed_coord), 0);
  const float width = seed.a * clamp(seed.r, 0.0f, 1.0f);
#endif
  if (width <= 0.0f) {
    return 1e20f;
  }
  const float2 diff = float2(texel) + 0.5f - seed_coord;
#ifdef OUTLINE_JFA_VISIBILITY
  if (!outline_candidate_visible(texel, int2(seed_coord))) {
    return 1e20f;
  }
  const float distance_squared = dot(diff, diff);
  const float coverage_radius = width * 0.5f + 0.5f;
  /* Keep outside candidates available for propagation, but never let a seed with zero
   * coverage displace one that actually contributes at this pixel. Covered grid samples
   * have a normalized squared distance <= 1 (subpixel widths only cover their own center). */
  return distance_squared / max(width * width, 1e-10f) +
         (distance_squared < coverage_radius * coverage_radius ? 0.0f : 2.0f);
#else
  /* Radius-weighted distance preserves coverage from a wide seed even when a narrower
   * seed is closer in pixels. A value <= 1/4 is inside the seed's coverage disk. */
  return dot(diff, diff) / max(width * width, 1e-10f);
#endif
}

void main()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const int2 extent = imageSize(jfa_out_img);
  if (any(greaterThanEqual(texel, extent))) {
    return;
  }

  JFAState best_coord = imageLoad(jfa_in_img, texel).rg;
  float best_dist_sq = outline_seed_distance_squared(texel, best_coord);

  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      const int2 sample_texel = texel + int2(dx, dy) * jfa_step_size;
      if (any(lessThan(sample_texel, int2(0))) || any(greaterThanEqual(sample_texel, extent))) {
        continue;
      }
      const JFAState sample_coord = imageLoad(jfa_in_img, sample_texel).rg;
#ifdef OUTLINE_JFA_PACKED
      const bool invalid_coord = sample_coord.x == 0xffffffffu;
#else
      const bool invalid_coord = sample_coord.x < -1e9f;
#endif
      if (invalid_coord) {
        continue;
      }
      const float dist_sq = outline_seed_distance_squared(texel, sample_coord);
      if (dist_sq < best_dist_sq) {
        best_dist_sq = dist_sq;
        best_coord = sample_coord;
      }
    }
  }

#ifdef OUTLINE_JFA_PACKED
  imageStore(jfa_out_img, texel, uint4(best_coord, 0u, 0u));
#else
  imageStore(jfa_out_img, texel, float4(best_coord, 0.0f, 0.0f));
#endif
}
