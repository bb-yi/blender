/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "infos/eevee_outline_infos.hh"

#include "eevee_defines.hh"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

/* Select one full-resolution source pixel per reduced outline pixel. Integer data (IDs,
 * packed parameters and surface depth) must never be filtered across material boundaries. */
int2 outline_source_texel(int2 texel, int2 source_extent, int2 target_extent)
{
  return min(int2((float2(texel) + 0.5f) * float2(source_extent) / float2(target_extent)),
             source_extent - int2(1));
}

/* Outline info is stored in integer channels to avoid Vulkan storage-image issues with UNORM
 * formats. Reserve the top width code-point so unpacking never hits 1.0 exactly and can
 * represent very large widths. */
#define OUTLINE_INFO_CODE_MAX 65535u
#define OUTLINE_WIDTH_PACK_MAX 65534u
/* Mask isolating the low-16-bit width code in an info.R channel. The upper 16 bits of R now carry
 * depth_threshold_range and depth_edge_width (see outline_info_r_pack); without this mask,
 * outline_width_unpack would read the full 32-bit channel and decode a width of ~65534 whenever
 * any upper byte is non-zero, flooding JFA coverage to the whole screen. */
#define OUTLINE_WIDTH_PACK_MASK 65535u

uint outline_unorm16_pack(float value)
{
  return uint(saturate(value) * float(OUTLINE_INFO_CODE_MAX) + 0.5f);
}

uint outline_unorm_pack(float value, uint max_value)
{
  return uint(saturate(value) * float(max_value) + 0.5f);
}

uint outline_width_pack(float width)
{
  width = max(width, 0.0f);
  if (width <= 0.0f) {
    return 0u;
  }
  return min(outline_unorm16_pack(width / (width + 1.0f)), OUTLINE_WIDTH_PACK_MAX);
}

float outline_width_unpack(uint width_packed)
{
  width_packed = min(width_packed & OUTLINE_WIDTH_PACK_MASK, OUTLINE_WIDTH_PACK_MAX);
  if (width_packed == 0u) {
    return 0.0f;
  }
  const float width_normalized = float(width_packed) / float(OUTLINE_INFO_CODE_MAX);
  return width_normalized / (1.0f - width_normalized);
}

/* Outline info packing (UINT_32_32_32_32, each channel is a 32-bit unsigned int).
 * R: [31..24: depth_edge_width  u8] [23..16: depth_threshold_range u8] [15..0: outline_width_pack]
 * G: [31..24: normal_edge_width u8] [23..16: normal_threshold_range u8] [15..0: depth_threshold unorm16]
 * B: [31: freestyle_edge bit] [30..23: id_edge_width u8] [19..18: priority]
 *    [17..16: ID combine] [15..0: normal_threshold unorm16]
 * A: [31..24: expansion block u8] [15: id_edge bit] [14..0: outline_id 15 bits].
 * Store the inverse mask so cleared pixels and old shaders allow expansion. */
#define OUTLINE_THRESHOLD_VALUE_MASK OUTLINE_INFO_CODE_MAX
#define OUTLINE_UNORM8_MAX 255u

#define OUTLINE_DEPTH_THRESHOLD_RANGE_SHIFT 16u
#define OUTLINE_DEPTH_EDGE_WIDTH_SHIFT 24u
#define OUTLINE_NORMAL_THRESHOLD_RANGE_SHIFT 16u
#define OUTLINE_NORMAL_EDGE_WIDTH_SHIFT 24u
#define OUTLINE_ID_EDGE_WIDTH_SHIFT 23u

#define OUTLINE_ID_VALUE_MASK 32767u
#define OUTLINE_ID_EDGE_BIT 32768u
#define OUTLINE_NORMAL_THRESHOLD_VALUE_MASK OUTLINE_INFO_CODE_MAX
#define OUTLINE_FREESTYLE_EDGE_BIT 2147483648u

/* R channel: line width + depth range + depth edge width. */
uint outline_info_r_pack(float line_width,
                         float depth_threshold_range,
                         float depth_edge_width)
{
  return outline_width_pack(line_width) |
         (outline_unorm_pack(depth_threshold_range, OUTLINE_UNORM8_MAX)
          << OUTLINE_DEPTH_THRESHOLD_RANGE_SHIFT) |
         (outline_unorm_pack(depth_edge_width, OUTLINE_UNORM8_MAX)
          << OUTLINE_DEPTH_EDGE_WIDTH_SHIFT);
}

float outline_depth_threshold_range_unpack(uint info_r)
{
  return float((info_r >> OUTLINE_DEPTH_THRESHOLD_RANGE_SHIFT) & OUTLINE_UNORM8_MAX) /
         float(OUTLINE_UNORM8_MAX);
}

float outline_depth_edge_width_unpack(uint info_r)
{
  return float((info_r >> OUTLINE_DEPTH_EDGE_WIDTH_SHIFT) & OUTLINE_UNORM8_MAX) /
         float(OUTLINE_UNORM8_MAX);
}

/* G channel: depth threshold + normal range + normal edge width. */
uint outline_info_g_pack(float depth_threshold,
                         float normal_threshold_range,
                         float normal_edge_width)
{
  return outline_unorm16_pack(depth_threshold) |
         (outline_unorm_pack(normal_threshold_range, OUTLINE_UNORM8_MAX)
          << OUTLINE_NORMAL_THRESHOLD_RANGE_SHIFT) |
         (outline_unorm_pack(normal_edge_width, OUTLINE_UNORM8_MAX)
          << OUTLINE_NORMAL_EDGE_WIDTH_SHIFT);
}

float outline_depth_threshold_unpack(uint threshold_packed)
{
  return float(threshold_packed & OUTLINE_THRESHOLD_VALUE_MASK) /
         float(OUTLINE_THRESHOLD_VALUE_MASK);
}

float outline_normal_threshold_range_unpack(uint info_g)
{
  return float((info_g >> OUTLINE_NORMAL_THRESHOLD_RANGE_SHIFT) & OUTLINE_UNORM8_MAX) /
         float(OUTLINE_UNORM8_MAX);
}

float outline_normal_edge_width_unpack(uint info_g)
{
  return float((info_g >> OUTLINE_NORMAL_EDGE_WIDTH_SHIFT) & OUTLINE_UNORM8_MAX) /
         float(OUTLINE_UNORM8_MAX);
}

/* B channel: normal threshold + id edge width + freestyle bit. */
uint outline_info_b_pack(float normal_threshold, float id_edge_width, bool freestyle_edge)
{
  uint packed_b = outline_unorm16_pack(normal_threshold);
  packed_b |= outline_unorm_pack(id_edge_width, OUTLINE_UNORM8_MAX)
              << OUTLINE_ID_EDGE_WIDTH_SHIFT;
  if (freestyle_edge) {
    packed_b |= OUTLINE_FREESTYLE_EDGE_BIT;
  }
  return packed_b;
}

float outline_normal_threshold_unpack(uint threshold_packed)
{
  return float(threshold_packed & OUTLINE_NORMAL_THRESHOLD_VALUE_MASK) /
         float(OUTLINE_NORMAL_THRESHOLD_VALUE_MASK);
}

float outline_id_edge_width_unpack(uint info_b)
{
  return float((info_b >> OUTLINE_ID_EDGE_WIDTH_SHIFT) & OUTLINE_UNORM8_MAX) /
         float(OUTLINE_UNORM8_MAX);
}

bool outline_freestyle_edge_unpack(uint threshold_packed)
{
  return (threshold_packed & OUTLINE_FREESTYLE_EDGE_BIT) != 0u;
}

/* A channel: outline id + id edge bit (unchanged). */
uint outline_id_pack(uint outline_id, bool id_edge)
{
  uint packed_id = min(outline_id, OUTLINE_ID_VALUE_MASK);
  if (id_edge) {
    packed_id |= OUTLINE_ID_EDGE_BIT;
  }
  return packed_id;
}

uint outline_id_unpack(uint outline_id_packed)
{
  return outline_id_packed & OUTLINE_ID_VALUE_MASK;
}

bool outline_id_edge_unpack(uint outline_id_packed)
{
  return (outline_id_packed & OUTLINE_ID_EDGE_BIT) != 0u;
}

uint outline_expansion_block_pack(float block)
{
  return outline_unorm_pack(block, OUTLINE_UNORM8_MAX) << 24u;
}

float outline_expansion_block_unpack(uint info_a)
{
  return float(info_a >> 24u) / float(OUTLINE_UNORM8_MAX);
}

uint outline_expansion_block_replace(uint info_a, float block)
{
  return (info_a & 0x00ffffffu) | outline_expansion_block_pack(block);
}

#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_OUTPUT) && defined(MAT_OUTLINE_CLEAR) && \
    defined(GPU_FRAGMENT_SHADER)
float4 g_outline_staged_color;
uint4 g_outline_staged_info;
bool g_outline_staged_receiver;
#endif

#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_OUTPUT) && defined(GPU_FRAGMENT_SHADER)
/* Emitted with the material graph, where its shading globals and depth offset are available.
 * Layer 1 stores raw reverse-Z depth followed by the world-space surface normal. */
uint4 outline_surface_data();
ObjectInfos drw_object_infos();
uint drw_resource_id();
#endif

void outline_output_reset()
{
#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_OUTPUT) && defined(MAT_OUTLINE_CLEAR) && \
    defined(GPU_FRAGMENT_SHADER)
  g_outline_staged_color = float4(0.0f);
  g_outline_staged_info = uint4(0u);
  g_outline_staged_receiver = false;
#endif
}

void outline_output_flush(float surface_opacity)
{
#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_OUTPUT) && defined(MAT_OUTLINE_CLEAR) && \
    defined(GPU_FRAGMENT_SHADER)
  surface_opacity = saturate(surface_opacity);
  if (!g_outline_staged_receiver || surface_opacity <= 0.0f) {
    return;
  }
  int2 texel = int2(gl_FragCoord.xy);
  g_outline_staged_color.a *= surface_opacity;
  const bool contributes = g_outline_staged_color.a > 0.0f &&
                           outline_width_unpack(g_outline_staged_info.r) > 0.0f;
  uint4 previous_info = uint4(0u);
#  ifdef MAT_FORWARD
  previous_info = imageLoadFast(outline_info_img, int3(texel, 0));
#  endif
  const float block = outline_expansion_block_unpack(previous_info.a) +
                      outline_expansion_block_unpack(g_outline_staged_info.a) * surface_opacity;
  uint4 stored_info = contributes ? g_outline_staged_info : previous_info;
  stored_info.a = outline_expansion_block_replace(stored_info.a, block);
  if (contributes) {
    imageStoreFast(outline_color_img, texel, g_outline_staged_color);
    imageStoreFast(outline_info_img, int3(texel, 1), outline_surface_data());
  }
  if (contributes || block > 0.0f) {
    imageStoreFast(outline_info_img, int3(texel, 0), stored_info);
  }
  if (imageSize(outline_info_img).z > 2) {
    uint4 identity = uint4(0u);
#  ifdef MAT_FORWARD
    identity = imageLoadFast(outline_info_img, int3(texel, 2));
#  endif
    identity.g = drw_resource_id() + 1u;
    if (contributes) {
      identity.r = identity.g;
    }
    imageStoreFast(outline_info_img, int3(texel, 2), identity);
  }
#endif
}

void attenuate_outline(float keep_factor)
{
#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_OUTPUT) && defined(MAT_OUTLINE_CLEAR) && \
    defined(MAT_FORWARD) && defined(GPU_FRAGMENT_SHADER)
  int2 texel = int2(gl_FragCoord.xy);
  keep_factor = saturate(keep_factor);
  if (keep_factor <= 1e-5f) {
    imageStoreFast(outline_color_img, texel, float4(0.0f));
    imageStoreFast(outline_info_img, int3(texel, 0), uint4(0u));
    if (imageSize(outline_info_img).z > 2) {
      imageStoreFast(outline_info_img, int3(texel, 2), uint4(0u));
    }
    return;
  }
  if (keep_factor >= 0.99999f) {
    return;
  }

  uint4 previous_info = imageLoadFast(outline_info_img, int3(texel, 0));
  const bool had_block = (previous_info.a >> 24u) != 0u;
  const float block = outline_expansion_block_unpack(previous_info.a) * keep_factor;
  previous_info.a = outline_expansion_block_replace(previous_info.a, block);
  if (had_block) {
    imageStoreFast(outline_info_img, int3(texel, 0), previous_info);
  }
  float4 outline_color = imageLoadFast(outline_color_img, texel);
  if (outline_color.a <= 1e-5f) {
    imageStoreFast(outline_color_img, texel, float4(0.0f));
    imageStoreFast(outline_info_img, int3(texel, 0), uint4(0u, 0u, 0u, outline_expansion_block_pack(block)));
    return;
  }

  outline_color.a *= keep_factor;
  if (outline_color.a <= 1e-5f) {
    imageStoreFast(outline_color_img, texel, float4(0.0f));
    imageStoreFast(outline_info_img, int3(texel, 0), uint4(0u, 0u, 0u, outline_expansion_block_pack(block)));
    return;
  }
  imageStoreFast(outline_color_img, texel, outline_color);
#else
  UNUSED_VARS(keep_factor);
#endif
}

float outline_pixel_world_size_at(float depth, int2 extent, int2 texel)
{
#if defined(DRW_VIEW_LIB_FUNCTIONS_DEFINED)
  float2 uv = (float2(texel) + 0.5f) / float2(extent);
  int2 next_texel = min(texel + int2(1, 0), extent - int2(1));
  float2 next_uv = (float2(next_texel) + 0.5f) / float2(extent);
  return distance(drw_point_screen_to_view(float3(uv, depth)),
                  drw_point_screen_to_view(float3(next_uv, depth)));
#else
  UNUSED_VARS(depth);
  UNUSED_VARS(extent);
  UNUSED_VARS(texel);
  return 1.0f;
#endif
}

void output_outline(float4 line_color,
                    float line_width,
                    float depth_threshold,
                    float depth_threshold_range,
                    float depth_edge_width,
                    float normal_threshold,
                    float normal_threshold_range,
                    float normal_edge_width,
                    float outline_id,
                    bool id_edge,
                    float id_edge_width,
                    bool freestyle_edge,
                    float expansion_mask,
                    float id_combine,
                    float edge_priority)
{
#if defined(MAT_OUTLINE_SUPPORT) && defined(MAT_OUTLINE_OUTPUT) && defined(GPU_FRAGMENT_SHADER)
  if (flag_test(drw_object_infos().flag, OBJECT_HOLDOUT)) {
    return;
  }

  const bool contributes = line_width > 0.0f && line_color.a > 0.0f;
  const float block = 1.0f - saturate(expansion_mask);
  const bool instance_identity = imageSize(outline_info_img).z > 2;
  if (!contributes && block <= 0.0f && !instance_identity) {
    return;
  }

  int2 texel = int2(gl_FragCoord.xy);
  uint custom_outline_id = uint(max(outline_id, 0.0f) + 0.5f);
  uint resolved_outline_id = (custom_outline_id > 0u) ? custom_outline_id :
                                                      min(drw_resource_id() + 1u,
                                                          OUTLINE_ID_VALUE_MASK);

  float4 stored_color = line_color;
  stored_color.a = saturate(stored_color.a);
  if (!contributes) {
    stored_color = float4(0.0f);
  }
  uint4 stored_info = uint4(
      outline_info_r_pack(line_width, depth_threshold_range, depth_edge_width),
      outline_info_g_pack(depth_threshold, normal_threshold_range, normal_edge_width),
      outline_info_b_pack(normal_threshold, id_edge_width, freestyle_edge),
      outline_id_pack(resolved_outline_id, id_edge));
  /* Receiving-only metadata must not create an ID boundary or lend width to a neighbor. */
  if (!contributes) {
    stored_info = uint4(0u);
  }
  stored_info.b |= (uint(clamp(id_combine, 0.0f, 3.0f)) << 16u) |
                   (uint(clamp(edge_priority, 0.0f, 3.0f)) << 18u);
  stored_info.a |= outline_expansion_block_pack(block);
#  if defined(MAT_OUTLINE_CLEAR)
  g_outline_staged_color = stored_color;
  g_outline_staged_info = stored_info;
  g_outline_staged_receiver = true;
#    if !defined(MAT_FORWARD)
  /* Deferred refraction preserves the existing source style. It has no Forward flush, but
   * its visible receiving surface must still be able to protect pixels from expansion. */
  if (block > 0.0f) {
    uint4 receiver_info = imageLoadFast(outline_info_img, int3(texel, 0));
    receiver_info.a = outline_expansion_block_replace(receiver_info.a, block);
    imageStoreFast(outline_info_img, int3(texel, 0), receiver_info);
  }
  if (instance_identity) {
    uint4 identity = imageLoadFast(outline_info_img, int3(texel, 2));
    identity.g = drw_resource_id() + 1u;
    imageStoreFast(outline_info_img, int3(texel, 2), identity);
  }
#    endif
#  else
  imageStoreFast(outline_color_img, texel, stored_color);
  imageStoreFast(outline_info_img, int3(texel, 0), stored_info);
  imageStoreFast(outline_info_img, int3(texel, 1), outline_surface_data());
  if (instance_identity) {
    const uint identity = drw_resource_id() + 1u;
    imageStoreFast(outline_info_img, int3(texel, 2),
                   uint4(contributes ? identity : 0u, identity, 0u, 0u));
  }
#  endif
#endif
}
