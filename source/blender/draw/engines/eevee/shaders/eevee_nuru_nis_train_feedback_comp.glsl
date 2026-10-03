/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Nuru NIS stage G3: training pass for off-screen receiver feedback.
 *
 * The Metal receiver-GI kernel appends subsampled NEE outcomes (gather-hit position, picked
 * cluster, realized L/p luminance) into a small ring buffer; mirror-interior positions never
 * appear in the camera-tile trainer, so this pass is what teaches the network those regions.
 * One thread per feedback entry. The cluster importance sums are reconstructed with a direct
 * scan over the local lights (no tile context exists at gather hits); the gradient math is
 * identical to the tile trainer.
 *
 * Buffer layout (uint words): [0] = atomic entry counter, [1..7] reserved,
 * then 8 words per entry: P.xyz (float bits), luma (float bits), cluster (float bits), pad x3.
 */

#include "infos/eevee_nuru_direct_light_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_nuru_nis_train_feedback)

#include "eevee_nuru_direct_light_lib.glsl"
#include "eevee_nuru_direct_light_importance_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

#define HWRT_NIS_GRAD_FIXED_SCALE 4096.0f

void hardware_nis_grad_add(uint param_index, float value)
{
  const float clamped = clamp(value, -64.0f, 64.0f);
  atomicAdd(hardware_nis_grads_buf[param_index], int(clamped * HWRT_NIS_GRAD_FIXED_SCALE));
}

void main()
{
  const uint entry_index = gl_GlobalInvocationID.x;
  const uint entry_count = min(hardware_nis_feedback_buf[0], 4096u);
  if (entry_index >= entry_count) {
    return;
  }
  if (!raytrace_buf.hardware_nis_enable) {
    return;
  }
  const uint base = 8u + entry_index * 8u;
  const float3 P = float3(uintBitsToFloat(hardware_nis_feedback_buf[base + 0u]),
                          uintBitsToFloat(hardware_nis_feedback_buf[base + 1u]),
                          uintBitsToFloat(hardware_nis_feedback_buf[base + 2u]));
  const float target_weight = clamp(
      uintBitsToFloat(hardware_nis_feedback_buf[base + 3u]), 0.0f, 32.0f);
  const uint picked_cluster = uint(
                                  clamp(uintBitsToFloat(hardware_nis_feedback_buf[base + 4u]),
                                        0.0f,
                                        float(HWRT_LIGHT_CLUSTER_COUNT - 1)));
  if (!all(lessThan(abs(P), float3(1.0e8f)))) {
    return;
  }

  /* Cluster importance sums via a direct local-light scan (gather hits have no tile). */
  float cluster_sums[HWRT_LIGHT_CLUSTER_COUNT];
  for (int c = 0; c < HWRT_LIGHT_CLUSTER_COUNT; c++) {
    cluster_sums[c] = 0.0f;
  }
  for (uint l_idx = 0u; l_idx < light_cull_buf.local_lights_len; l_idx++) {
    const uint cluster_id = uint(light_buf[l_idx].hwrt_cluster_id) % uint(HWRT_LIGHT_CLUSTER_COUNT);
    cluster_sums[cluster_id] += hardware_direct_light_local_importance(l_idx, P, true);
  }

  /* Forward pass + softmax-residual PMF; same math as the tile trainer. */
  float h1[HWRT_NIS_HIDDEN];
  float h2[HWRT_NIS_HIDDEN];
  float logits[HWRT_NIS_OUT];
  hardware_nis_forward(P, h1, h2, logits);

  float q[HWRT_NIS_OUT];
  float weighted_total = 0.0f;
  for (int c = 0; c < HWRT_NIS_OUT; c++) {
    const float m = exp(clamp(logits[c], -4.0f, 4.0f)) *
                    clamp(hardware_light_cluster_weight_buf[c], 1.0e-3f, 1.0e3f);
    q[c] = cluster_sums[c] * clamp(m, 1.0e-3f, 1.0e3f);
    weighted_total += q[c];
  }
  if (!(weighted_total > 0.0f)) {
    return;
  }
  for (int c = 0; c < HWRT_NIS_OUT; c++) {
    q[c] /= weighted_total;
  }

  float grad_logits[HWRT_NIS_OUT];
  for (int j = 0; j < HWRT_NIS_OUT; j++) {
    const float indicator = (uint(j) == picked_cluster) ? 1.0f : 0.0f;
    float g = -target_weight * (indicator - q[j]);
    if ((logits[j] >= 4.0f && g < 0.0f) || (logits[j] <= -4.0f && g > 0.0f)) {
      g = 0.0f;
    }
    grad_logits[j] = g;
  }

  float grad_h2[HWRT_NIS_HIDDEN];
  for (int i = 0; i < HWRT_NIS_HIDDEN; i++) {
    grad_h2[i] = 0.0f;
  }
  for (int j = 0; j < HWRT_NIS_OUT; j++) {
    const float gj = grad_logits[j];
    if (gj == 0.0f) {
      continue;
    }
    hardware_nis_grad_add(uint(HWRT_NIS_B3_OFFSET + j), gj);
    for (int i = 0; i < HWRT_NIS_HIDDEN; i++) {
      hardware_nis_grad_add(uint(HWRT_NIS_W3_OFFSET + j * HWRT_NIS_HIDDEN + i), gj * h2[i]);
      grad_h2[i] += gj * hardware_nis_weights_buf[HWRT_NIS_W3_OFFSET + j * HWRT_NIS_HIDDEN + i];
    }
  }

  float grad_h1[HWRT_NIS_HIDDEN];
  for (int i = 0; i < HWRT_NIS_HIDDEN; i++) {
    grad_h1[i] = 0.0f;
  }
  for (int j = 0; j < HWRT_NIS_HIDDEN; j++) {
    const float gj = (h2[j] > 0.0f) ? grad_h2[j] : 0.0f;
    if (gj == 0.0f) {
      continue;
    }
    hardware_nis_grad_add(uint(HWRT_NIS_B2_OFFSET + j), gj);
    for (int i = 0; i < HWRT_NIS_HIDDEN; i++) {
      hardware_nis_grad_add(uint(HWRT_NIS_W2_OFFSET + j * HWRT_NIS_HIDDEN + i), gj * h1[i]);
      grad_h1[i] += gj * hardware_nis_weights_buf[HWRT_NIS_W2_OFFSET + j * HWRT_NIS_HIDDEN + i];
    }
  }

  float encoded[HWRT_NIS_IN];
  hardware_nis_encode_position(P, encoded);
  for (int j = 0; j < HWRT_NIS_HIDDEN; j++) {
    const float gj = (h1[j] > 0.0f) ? grad_h1[j] : 0.0f;
    if (gj == 0.0f) {
      continue;
    }
    hardware_nis_grad_add(uint(HWRT_NIS_B1_OFFSET + j), gj);
    for (int i = 0; i < HWRT_NIS_IN; i++) {
      hardware_nis_grad_add(uint(HWRT_NIS_W1_OFFSET + j * HWRT_NIS_IN + i), gj * encoded[i]);
    }
  }

  atomicAdd(hardware_nis_train_count_buf[0], 1u);
}
