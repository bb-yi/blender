/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_command_shared.hh"
#include "draw_view.bsl.hh"
#include "eevee_defines.hh"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_uniform.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_utildefines.bsl.hh"

namespace eevee::raytracing {

struct HitEvalInput {
  [[push_constant]] int scene_entry_count;
  [[image(0, read, SFLOAT_32)]] image2D ray_time_img;
  [[image(1, read_write, UINT_32_32_32_32)]] uimage2D hit_identity_img;
  [[sampler(0)]] sampler2D depth_tx;
  [[storage(0, read)]] uint (&tiles_coord_buf)[];

  bool primary_depth_load(RayTraceData data, int2 texel, float3 &screen_position) const
  {
    int2 full_texel = texel * data.trace_pixel_scale + data.trace_pixel_offset;
    if (!in_texture_range(full_texel, depth_tx)) {
      return false;
    }
    float depth = reverse_z::read(texelFetch(depth_tx, full_texel, 0).r);
    if (!(depth > 0.0f && depth < 1.0f)) {
      return false;
    }
    screen_position = float3((float2(full_texel) + 0.5f) * data.full_resolution_inv, depth);
    return true;
  }
};

struct HitEvalCount {
  [[storage(1, read_write)]] uint (&hit_eval_count_buf)[];
};

[[compute, local_size(RAYTRACE_GROUP_SIZE, RAYTRACE_GROUP_SIZE)]]
void hit_eval_count([[resource_table]] HitEvalInput &payload,
                    [[resource_table]] HitEvalCount &srt,
                    [[resource_table]] const Uniform &uni,
                    [[work_group_id]] const uint3 group_id,
                    [[local_invocation_id]] const uint3 local_id)
{
  uint2 tile = unpackUvec2x16(payload.tiles_coord_buf[group_id.x]);
  int2 texel = int2(local_id.xy + tile * RAYTRACE_GROUP_SIZE);
  if (!in_image_range(texel, payload.ray_time_img) ||
      !(imageLoadFast(payload.ray_time_img, texel).x > 0.0f))
  {
    return;
  }
  float3 screen_position;
  if (!payload.primary_depth_load(uni.raytrace_buf, texel, screen_position)) {
    return;
  }
  uint user_id = imageLoadFast(payload.hit_identity_img, texel).x;
  if (user_id < uint(payload.scene_entry_count)) {
    atomicAdd(srt.hit_eval_count_buf[user_id], 1u);
  }
}

struct HitEvalPrefix {
  [[push_constant]] int scene_entry_count;
  [[storage(0, read)]] uint (&hit_eval_count_buf)[];
  [[storage(1, write)]] uint (&hit_eval_offset_buf)[];
  [[storage(2, write)]] uint (&hit_eval_cursor_buf)[];
  [[storage(3, write)]] DrawCommandArray (&hit_eval_indirect_draw_buf)[];
};

[[compute, local_size(64)]]
void hit_eval_prefix([[resource_table]] HitEvalPrefix &srt,
                     [[global_invocation_id]] const uint3 global_id)
{
  uint entry = global_id.x;
  if (entry >= uint(srt.scene_entry_count)) {
    return;
  }
  /* Keep the pinned upstream scan for the baseline. Optimize only after measurement. */
  uint prefix = 0u;
  for (uint i = 0u; i < entry; i++) {
    prefix += srt.hit_eval_count_buf[i];
  }
  uint count = srt.hit_eval_count_buf[entry];
  srt.hit_eval_offset_buf[entry] = prefix;
  srt.hit_eval_cursor_buf[entry] = 0u;
  DrawCommandArray command = {};
  command.vertex_len = count * 3u;
  command.instance_len = count > 0u ? 1u : 0u;
  command.vertex_first = prefix * 3u;
  srt.hit_eval_indirect_draw_buf[entry] = command;
}

struct HitEvalCompact {
  [[storage(1, read)]] uint (&hit_eval_offset_buf)[];
  [[storage(2, read_write)]] uint (&hit_eval_cursor_buf)[];
  [[storage(3, read)]] uint (&hit_eval_resource_id_buf)[];
  [[storage(4, write)]] HardwareHitEvalRecord (&hit_eval_list_buf)[];
  [[sampler(1)]] sampler2D hit_world_position_tx;
  [[image(2, read, SFLOAT_16_16_16_16)]] image2D hit_material_img;
  [[image(3, read, SFLOAT_16_16_16_16)]] image2D hit_normal_img;
  [[image(4, read, SFLOAT_16_16_16_16)]] image2D hit_barycentric_img;

  float3 view_origin_load(int2 texel, float3 primary_P) const
  {
    float3 hit_P = texelFetch(hit_world_position_tx, texel, 0).xyz;
    float distance = imageLoadFast(hit_barycentric_img, texel).z;
    if (!(distance > 0.0f) || !(dot(hit_P, hit_P) > 1.0e-10f)) {
      return primary_P;
    }
    float2 packed_direction = float2(imageLoadFast(hit_material_img, texel).w,
                            imageLoadFast(hit_normal_img, texel).w);
    if (all(equal(packed_direction, float2(0.0f)))) {
      return primary_P;
    }
    packed_direction = packed_direction * 2.0f - 1.0f;
    float3 direction = float3(packed_direction, 1.0f - abs(packed_direction.x) - abs(packed_direction.y));
    float t = clamp(-direction.z, 0.0f, 1.0f);
    direction.x += direction.x >= 0.0f ? -t : t;
    direction.y += direction.y >= 0.0f ? -t : t;
    direction = normalize(direction);
    if (any(isinf(direction)) || !(dot(direction, direction) > 1.0e-10f)) {
      return primary_P;
    }
    return hit_P - direction * distance;
  }
};

[[compute, local_size(RAYTRACE_GROUP_SIZE, RAYTRACE_GROUP_SIZE)]]
void hit_eval_compact([[resource_table]] HitEvalInput &payload,
                      [[resource_table]] HitEvalCompact &srt,
                      [[resource_table]] const Uniform &uni,
                      [[resource_table]] const draw::View &views,
                      [[work_group_id]] const uint3 group_id,
                      [[local_invocation_id]] const uint3 local_id)
{
  uint2 tile = unpackUvec2x16(payload.tiles_coord_buf[group_id.x]);
  int2 texel = int2(local_id.xy + tile * RAYTRACE_GROUP_SIZE);
  if (!in_image_range(texel, payload.ray_time_img) ||
      !(imageLoadFast(payload.ray_time_img, texel).x > 0.0f))
  {
    return;
  }
  uint4 identity = imageLoadFast(payload.hit_identity_img, texel);
  uint user_id = identity.x;
  float3 screen_position;
  if (user_id >= uint(payload.scene_entry_count) ||
      !payload.primary_depth_load(uni.raytrace_buf, texel, screen_position))
  {
    return;
  }
  float3 primary_P = views.get(0).point_screen_to_world(screen_position);
  uint record_index = srt.hit_eval_offset_buf[user_id] +
                      atomicAdd(srt.hit_eval_cursor_buf[user_id], 1u);
  HardwareHitEvalRecord record = {};
  record.packed_texel = packUvec2x16(uint2(texel));
  record.resource_id_raw = srt.hit_eval_resource_id_buf[user_id];
  record.primitive_id = identity.y;
  record.flags = (identity.z & 1u) != 0u ? HIT_EVAL_FLAG_FRONT_FACING : 0u;
  record.barycentric_coords = imageLoadFast(srt.hit_barycentric_img, texel).xy;
  record.view_origin = srt.view_origin_load(texel, primary_P);
  imageStoreFast(payload.hit_identity_img, texel, uint4(identity.xyz, record.resource_id_raw));
  srt.hit_eval_list_buf[record_index] = record;
}

struct ShadowTintInput {
  [[push_constant]] int shadow_tint_capacity;
  [[storage(0, read)]] const uint (&shadow_tint_count)[];
  [[storage(1, read)]] const HardwareShadowTintHit (&shadow_tint_hits)[];
  [[storage(2, read)]] const HardwareShadowTintRay (&shadow_tint_rays)[];
  [[storage(3, read)]] const uint2 (&shadow_tint_entries)[];

  bool hit_entry(uint index, uint &entry) const
  {
    if (index >= min(uint(shadow_tint_capacity), shadow_tint_count[0])) {
      return false;
    }
    uint4 identity = shadow_tint_hits[index].identity;
    if (shadow_tint_rays[identity.z >> 2u].range.x == 0u) {
      return false;
    }
    entry = shadow_tint_entries[identity.x].x;
    return true;
  }
};

struct ShadowTintCount {
  [[storage(4, read_write)]] uint (&hit_eval_count_buf)[];
};

[[compute, local_size(64)]]
void shadow_tint_count([[resource_table]] const ShadowTintInput &payload,
                       [[resource_table]] ShadowTintCount &srt,
                       [[global_invocation_id]] const uint3 id)
{
  uint entry;
  if (payload.hit_entry(id.x, entry)) {
    atomicAdd(srt.hit_eval_count_buf[entry], 1u);
  }
}

struct ShadowTintCompact {
  [[storage(4, read)]] const uint (&hit_eval_offset_buf)[];
  [[storage(5, read_write)]] uint (&hit_eval_cursor_buf)[];
  [[storage(6, write)]] uint (&shadow_tint_order)[];
};

[[compute, local_size(64)]]
void shadow_tint_compact([[resource_table]] const ShadowTintInput &payload,
                         [[resource_table]] ShadowTintCompact &srt,
                         [[global_invocation_id]] const uint3 id)
{
  uint entry;
  if (payload.hit_entry(id.x, entry)) {
    uint slot = srt.hit_eval_offset_buf[entry] + atomicAdd(srt.hit_eval_cursor_buf[entry], 1u);
    srt.shadow_tint_order[slot] = id.x;
  }
}

struct ShadowTintResolve {
  [[push_constant]] int4 shadow_tint_rect;
  [[push_constant]] int shadow_tint_samples;
  [[push_constant]] int shadow_tint_layer;
  [[push_constant]] float shadow_tint_saturation;
  [[storage(0, read)]] const HardwareShadowTintHit (&shadow_tint_hits)[];
  [[storage(1, read)]] const HardwareShadowTintRay (&shadow_tint_rays)[];
  [[image(0, read_write, SFLOAT_16_16_16_16)]] image2DArray shadow_tint_visibility;
};

[[compute, local_size(8, 8)]]
void shadow_tint_resolve([[resource_table]] ShadowTintResolve &srt,
                         [[global_invocation_id]] const uint3 id)
{
  int2 local = int2(id.xy);
  if (any(greaterThanEqual(local, srt.shadow_tint_rect.zw))) {
    return;
  }
  uint first_ray = uint((local.y * srt.shadow_tint_rect.z + local.x) * srt.shadow_tint_samples);
  float4 correction = float4(0.0f);
  const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
  for (int i = 0; i < srt.shadow_tint_samples; i++) {
    HardwareShadowTintRay ray = srt.shadow_tint_rays[first_ray + uint(i)];
    uint next = ray.range.x;
    if (next == 0u) {
      continue;
    }
    float3 visibility = float3(1.0f);
    float3 self_shadow = float3(0.0f);
    while (next != 0u) {
      HardwareShadowTintHit hit = srt.shadow_tint_hits[next - 1u];
      self_shadow += visibility * hit.before_self.xyz;
      visibility *= hit.before_visibility.xyz;
      if ((hit.identity.z & SHADOW_TINT_HIT_SELF) != 0u) {
        self_shadow += visibility * (1.0f - hit.transmission.xyz);
      }
      visibility *= hit.transmission.xyz;
      next = hit.identity.w;
    }
    self_shadow += visibility * ray.tail_self.xyz;
    visibility *= ray.tail_visibility.xyz;
    float3 tint = mix(float3(dot(visibility, luma)), visibility, srt.shadow_tint_saturation);
    correction += (float4(tint, dot(self_shadow, luma)) - ray.fallback) / float(ray.range.y);
  }
  if (any(notEqual(correction, float4(0.0f)))) {
    int3 texel = int3(local + srt.shadow_tint_rect.xy, srt.shadow_tint_layer);
    float4 visibility = imageLoadFast(srt.shadow_tint_visibility, texel);
    imageStoreFast(srt.shadow_tint_visibility, texel, saturate(visibility + correction));
  }
}

struct HitDenoiseGuides {
  [[image(0, read, SFLOAT_16_16_16_16)]] image2D hit_albedo_img;
  [[image(1, read, SFLOAT_16_16_16_16)]] image2D hit_normal_img;
  [[image(2, write, SFLOAT_16_16_16_16)]] image2D denoise_albedo_img;
  [[image(3, write, SFLOAT_16_16_16_16)]] image2D denoise_normal_img;
};

[[compute, local_size(RAYTRACE_GROUP_SIZE, RAYTRACE_GROUP_SIZE)]]
void hit_denoise_guides([[resource_table]] HitDenoiseGuides &srt,
                        [[global_invocation_id]] const uint3 global_id)
{
  int2 texel = int2(global_id.xy);
  if (!in_image_range(texel, srt.hit_normal_img)) {
    return;
  }
  float4 normal = imageLoadFast(srt.hit_normal_img, texel);
  /* Each closure pixel belongs to only one traced family. A later phase's empty payload must
   * not erase the earlier phase's guide. Misses and unsupported families retain cleared guides. */
  if (dot(normal.xyz, normal.xyz) > 1.0e-10f) {
    imageStoreFast(srt.denoise_albedo_img, texel, imageLoadFast(srt.hit_albedo_img, texel));
    imageStoreFast(srt.denoise_normal_img, texel, normal);
  }
}

}  // namespace eevee::raytracing

PipelineCompute eevee_ray_hit_eval_count(eevee::raytracing::hit_eval_count);
PipelineCompute eevee_ray_hit_eval_prefix(eevee::raytracing::hit_eval_prefix);
PipelineCompute eevee_ray_hit_eval_compact(eevee::raytracing::hit_eval_compact);
PipelineCompute eevee_ray_shadow_tint_count(eevee::raytracing::shadow_tint_count);
PipelineCompute eevee_ray_shadow_tint_compact(eevee::raytracing::shadow_tint_compact);
PipelineCompute eevee_ray_shadow_tint_resolve(eevee::raytracing::shadow_tint_resolve);
PipelineCompute eevee_ray_hit_denoise_guides(eevee::raytracing::hit_denoise_guides);
