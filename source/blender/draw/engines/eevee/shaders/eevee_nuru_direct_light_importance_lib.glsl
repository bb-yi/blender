/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Nuru many-light direct sampling importance (NIS workstream, stage N0).
 *
 * Shared between the direct-light selection kernel (which picks one local light per tile
 * proportionally to this importance) and the accumulation kernel (which weights the picked
 * light by `total_importance / picked_importance`). Both kernels MUST evaluate the exact same
 * function with the exact same inputs, otherwise the estimator is biased. The shading position
 * is reconstructed deterministically from the same `sample_texel` depth fetch on both sides.
 *
 * Unbiasedness rule: any light whose analytic evaluation can be non-zero must keep a strictly
 * positive importance. Distance attenuation uses the Lambert small-source form
 * `1 / (d^2 + r^2)` which never reaches zero inside the culling influence radius; no facing or
 * cone tests are applied here (they could zero the importance where the shaded result is
 * non-zero, e.g. normal-mapped surfaces or spot penumbra).
 */

#include "eevee_nuru_nis_mlp_lib.glsl"

float hardware_direct_light_color_importance(LightData light)
{
  return max(dot(abs(light.color), float3(0.2126f, 0.7152f, 0.0722f)), 1.0e-4f);
}

/**
 * Position-aware local-light importance. `P_valid` is false when the tile's representative
 * texel has no valid depth; both kernels derive it from the same depth fetch, so the
 * luminance-only fallback stays consistent.
 */
float hardware_direct_light_local_importance(uint light_index, float3 P, bool P_valid)
{
  LightData light = light_buf[light_index];
  float importance = hardware_direct_light_color_importance(light) *
                     raytrace_buf.hardware_direct_light.local_light_importance_scale;
  if (is_area_light(light.type)) {
    importance *= raytrace_buf.hardware_direct_light.area_light_importance_scale;
  }
  if (P_valid) {
    /* Diffuse power carries Eevee's `1/r^2` shape normalization; the unshadowed contribution
     * at distance `d` scales like `power / (d^2 + r^2)`. Using it makes nearby lights win the
     * pick over bright-but-distant ones (the previous importance was position-blind). */
    const float3 to_light = light.position() - P;
    const float dist_sqr = dot(to_light, to_light);
    const float radius = max(light.local().local.shape_radius, 1.0e-2f);
    const float power = max(abs(light.power[LIGHT_DIFFUSE]), 1.0e-6f);
    importance *= power / max(dist_sqr + radius * radius, 1.0e-4f);
  }
  return max(importance, 1.0e-8f);
}

float hardware_direct_light_sun_importance(uint sun_index)
{
  const uint light_index = raytrace_buf.hardware_direct_light.local_lights_len + sun_index;
  LightData light = light_buf[light_index];
  return hardware_direct_light_color_importance(light) *
         raytrace_buf.hardware_direct_light.sun_light_importance_scale;
}

/* -------------------------------------------------------------------------------------------
 * Nuru NIS stage N1/N2: two-stage cluster sampling.
 *
 * Lights carry a stable `cluster_id` in [0, HWRT_LIGHT_CLUSTER_COUNT). Selection first picks a
 * cluster proportionally to `s_c * m_c` (s_c = per-tile sum of candidate importances in the
 * cluster, m_c = learned multiplier from `hardware_light_cluster_weight_buf`, all ones until
 * the network trains), then picks a light inside the cluster proportionally to its importance.
 * The total selection PMF is p(y) = m_c * imp_y / sum_c'(s_c' * m_c'), compensated by the
 * accumulation kernel with weight = sum_c'(s_c' * m_c') / (m_c * imp_y). With m == 1 this
 * reduces exactly to the N0 single-stage estimator. m_c is clamped strictly positive, so the
 * PMF support never shrinks: the learned distribution can only redistribute samples, never
 * bias the image. ------------------------------------------------------------------------- */

/** Per-tile multipliers: the N2 network when enabled, the (neutral) global weight buffer
 * otherwise. Wrapped so all consumers share one definition. */
void hardware_light_cluster_multipliers(float3 P,
                                        bool P_valid,
                                        out float r_m[HWRT_LIGHT_CLUSTER_COUNT])
{
  hardware_nis_cluster_multipliers(P, P_valid, r_m);
  for (int c = 0; c < HWRT_LIGHT_CLUSTER_COUNT; c++) {
    r_m[c] = clamp(r_m[c] * hardware_light_cluster_weight_buf[c], 1.0e-3f, 1.0e3f);
  }
}

/** Per-tile cluster importance sums over the candidate words. Deterministic: selection and
 * accumulation both call this with identical inputs. */
void hardware_light_cluster_sums(HardwareDirectLightWorkTile work_tile,
                                 float3 P,
                                 bool P_valid,
                                 out float r_sums[HWRT_LIGHT_CLUSTER_COUNT])
{
  for (int c = 0; c < HWRT_LIGHT_CLUSTER_COUNT; c++) {
    r_sums[c] = 0.0f;
  }
  for (uint word_index = 0u; word_index < work_tile.candidate_word_count; word_index++) {
    uint word = light_tile_buf[work_tile.candidate_word_offset + word_index];
    int bit_index;
    while ((bit_index = findLSB(word)) != -1) {
      word &= ~(1u << uint(bit_index));
      const uint light_index = word_index * 32u + uint(bit_index);
      if (light_index < raytrace_buf.hardware_direct_light.local_lights_len) {
        const uint cluster_id = uint(light_buf[light_index].hwrt_cluster_id) %
                                uint(HWRT_LIGHT_CLUSTER_COUNT);
        r_sums[cluster_id] += hardware_direct_light_local_importance(light_index, P, P_valid);
      }
    }
  }
}

/** Weighted total `sum_c s_c * m_c`; the denominator of the two-stage PMF. */
float hardware_light_cluster_weighted_total(float sums[HWRT_LIGHT_CLUSTER_COUNT],
                                            float multipliers[HWRT_LIGHT_CLUSTER_COUNT])
{
  float total = 0.0f;
  for (int c = 0; c < HWRT_LIGHT_CLUSTER_COUNT; c++) {
    total += sums[c] * multipliers[c];
  }
  return total;
}
