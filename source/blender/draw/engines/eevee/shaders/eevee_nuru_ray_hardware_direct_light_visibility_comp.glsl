/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Sample a bounded many-light direct subset from the queued tiles and resolve their RT visibility
 * from the already generated primary-surface Hardware RT shadow atlas.
 */

#include "infos/eevee_nuru_direct_light_infos.hh"

#define HWRT_DIRECT_VISIBILITY
COMPUTE_SHADER_CREATE_INFO(eevee_ray_hardware_direct_light_visibility)

#include "eevee_nuru_direct_light_lib.glsl"
#include "eevee_nuru_direct_light_importance_lib.glsl"
#include "eevee_reverse_z_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

bool select_local_light(HardwareDirectLightWorkTile work_tile,
                        float selector,
                        float cluster_selector,
                        float3 P,
                        bool P_valid,
                        uint &r_light_index,
                        float &r_importance)
{
  /* Nuru NIS two-stage pick: cluster proportional to s_c * m_c, then light inside the cluster
   * proportional to its importance. See eevee_nuru_direct_light_importance_lib.glsl for the
   * estimator contract with the accumulation kernel. */
  float cluster_sums[HWRT_LIGHT_CLUSTER_COUNT];
  hardware_light_cluster_sums(work_tile, P, P_valid, cluster_sums);
  float cluster_multipliers[HWRT_LIGHT_CLUSTER_COUNT];
  hardware_light_cluster_multipliers(P, P_valid, cluster_multipliers);
  const float weighted_total = hardware_light_cluster_weighted_total(cluster_sums,
                                                                     cluster_multipliers);
  if (!(weighted_total > 0.0f)) {
    r_light_index = 0xFFFFFFFFu;
    r_importance = 0.0f;
    return false;
  }

  uint picked_cluster = 0u;
  {
    const float target = cluster_selector * weighted_total;
    float accum = 0.0f;
    for (int c = 0; c < HWRT_LIGHT_CLUSTER_COUNT; c++) {
      const float weighted = cluster_sums[c] * cluster_multipliers[c];
      accum += weighted;
      if (accum >= target && weighted > 0.0f) {
        picked_cluster = uint(c);
        break;
      }
      /* Numerical tail: keep the last non-empty cluster as fallback. */
      if (weighted > 0.0f) {
        picked_cluster = uint(c);
      }
    }
  }

  const float cluster_sum = cluster_sums[picked_cluster];
  if (!(cluster_sum > 0.0f)) {
    r_light_index = 0xFFFFFFFFu;
    r_importance = 0.0f;
    return false;
  }

  const float target = selector * cluster_sum;
  float accum_importance = 0.0f;
  uint fallback_index = 0xFFFFFFFFu;
  float fallback_importance = 0.0f;
  for (uint word_index = 0u; word_index < work_tile.candidate_word_count; word_index++) {
    uint word = light_tile_buf[work_tile.candidate_word_offset + word_index];
    int bit_index;
    while ((bit_index = findLSB(word)) != -1) {
      word &= ~(1u << uint(bit_index));
      const uint light_index = word_index * 32u + uint(bit_index);
      if (light_index >= raytrace_buf.hardware_direct_light.local_lights_len) {
        continue;
      }
      if (uint(light_buf[light_index].hwrt_cluster_id) % uint(HWRT_LIGHT_CLUSTER_COUNT) !=
          picked_cluster)
      {
        continue;
      }
      const float importance = hardware_direct_light_local_importance(light_index, P, P_valid);
      accum_importance += importance;
      fallback_index = light_index;
      fallback_importance = importance;
      if (accum_importance >= target) {
        r_light_index = light_index;
        r_importance = importance;
        return true;
      }
    }
  }

  /* Numerical tail of the CDF walk: return the last candidate of the cluster. */
  r_light_index = fallback_index;
  r_importance = fallback_importance;
  return fallback_index != 0xFFFFFFFFu;
}

bool select_sun_light(float selector, uint &r_sun_index, float &r_importance)
{
  const uint sun_lights_len = raytrace_buf.hardware_direct_light.sun_lights_len;
  if (sun_lights_len == 0u ||
      !raytrace_buf.hardware_direct_light.trace_sun_lights_separately)
  {
    r_sun_index = 0xFFFFFFFFu;
    r_importance = 0.0f;
    return false;
  }

  float total_importance = 0.0f;
  for (uint sun_index = 0u; sun_index < sun_lights_len; sun_index++) {
    total_importance += hardware_direct_light_sun_importance(sun_index);
  }

  if (!(total_importance > 0.0f)) {
    r_sun_index = 0xFFFFFFFFu;
    r_importance = 0.0f;
    return false;
  }

  const float target = selector * total_importance;
  float accum_importance = 0.0f;
  for (uint sun_index = 0u; sun_index < sun_lights_len; sun_index++) {
    const float importance = hardware_direct_light_sun_importance(sun_index);
    accum_importance += importance;
    if (accum_importance >= target) {
      r_sun_index = sun_index;
      r_importance = importance;
      return true;
    }
  }

  r_sun_index = 0xFFFFFFFFu;
  r_importance = 0.0f;
  return false;
}

void main()
{
  const uint queue_index = gl_GlobalInvocationID.x;
  /* The compacted count can exceed the host allocation when the culling grid drifts from the
   * size captured at sync; both buffers are `hardware_direct_light_tile_capacity` entries. */
  if (queue_index >= uint(hardware_direct_light_tile_capacity)) {
    return;
  }
  const HardwareDirectLightWorkTile work_tile = hardware_direct_light_work_tiles_buf[queue_index];
  const uint2 tile_coord = unpackUvec2x16(work_tile.packed_tile_coord);
  const uint tile_size_px = max(raytrace_buf.hardware_direct_light.tile_size_px, 1u);

  const float2 noise = interleaved_gradient_noise(
      float2(tile_coord * tile_size_px) + 0.5f,
      float2(0.0f, 1.0f),
      float2(sampling_rng_1D_get(SAMPLING_SHADOW_U), sampling_rng_1D_get(SAMPLING_SHADOW_X)));
  const uint2 sample_offset = min(uint2(noise * float(tile_size_px)), uint2(tile_size_px - 1u));
  uint2 sample_texel = tile_coord * tile_size_px + sample_offset;
  const int2 visibility_extent = textureSize(hardware_rt_shadow_visibility_tx, 0).xy;
  sample_texel = clamp(sample_texel,
                       uint2(0u),
                       uint2(max(visibility_extent.x - 1, 0), max(visibility_extent.y - 1, 0)));

  HardwareDirectLightVisibilitySample visibility_record;
  visibility_record.packed_tile_coord = work_tile.packed_tile_coord;
  visibility_record.packed_sample_texel = packUvec2x16(sample_texel);
  visibility_record.local_light_index = 0xFFFFFFFFu;
  visibility_record.sun_light_index = 0xFFFFFFFFu;
  visibility_record.local_visibility = float3(0.0f);
  visibility_record.local_importance = 0.0f;
  visibility_record.sun_visibility = float3(0.0f);
  visibility_record.sun_importance = 0.0f;

  /* Nuru N0: position-aware importance. The accumulation kernel reconstructs the same P from
   * the same texel and depth texture, so the selection PDF and the compensation weight match
   * exactly (see eevee_nuru_direct_light_importance_lib.glsl). */
  const float sample_depth = reverse_z::read(texelFetch(depth_tx, int2(sample_texel), 0).r);
  const bool sample_P_valid = (sample_depth > 0.0f && sample_depth < 1.0f);
  float3 sample_P = float3(0.0f);
  if (sample_P_valid) {
    const float2 sample_uv = (float2(sample_texel) + 0.5f) * raytrace_buf.full_resolution_inv;
    sample_P = drw_point_screen_to_world(float3(sample_uv, sample_depth));
  }

  const float local_selector = interleaved_gradient_noise(
      float2(sample_texel) + 0.5f, 2.0f, sampling_rng_1D_get(SAMPLING_RAYTRACE_U));
  const float cluster_selector = interleaved_gradient_noise(
      float2(sample_texel) + 0.5f, 4.0f, sampling_rng_1D_get(SAMPLING_SHADOW_I));
  if (select_local_light(
          work_tile,
          local_selector,
          cluster_selector,
          sample_P,
          sample_P_valid,
          visibility_record.local_light_index,
          visibility_record.local_importance))
  {
    /* Nuru: `local_light_index` is into the culled+sorted `light_buf`, but the visibility
     * texture is laid out in unsorted per-type order. Read the stable layer from the light.
     * The `.rgb` carries the transparent-shadow attenuation produced by the MSL trace. */
    const LightData light = light_buf[visibility_record.local_light_index];
    visibility_record.local_visibility =
        hardware_direct_visibility(int2(sample_texel), light.hwrt_shadow_layer);
  }

  const float sun_selector = interleaved_gradient_noise(
      float2(sample_texel) + 0.5f, 3.0f, sampling_rng_1D_get(SAMPLING_RAYTRACE_X));
  if (select_sun_light(
          sun_selector, visibility_record.sun_light_index, visibility_record.sun_importance))
  {
    const uint light_index = raytrace_buf.hardware_direct_light.local_lights_len +
                             visibility_record.sun_light_index;
    const LightData sun_light = light_buf[light_index];
    visibility_record.sun_visibility = hardware_direct_visibility(int2(sample_texel), sun_light.hwrt_shadow_layer);
  }

  hardware_direct_light_visibility_samples_buf[queue_index] = visibility_record;
}
