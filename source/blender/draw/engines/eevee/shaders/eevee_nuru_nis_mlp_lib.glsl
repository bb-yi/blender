/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Nuru NIS stage N2: tiny MLP predicting per-shading-point light-cluster multipliers.
 *
 * Follows "Neural Importance Sampling of Many Lights" (Figueiredo et al., SIGGRAPH 2025):
 * the network learns a RESIDUAL over the analytic per-tile cluster importance sums, trained
 * online by minimizing KL divergence against realized contributions. The output multiplier
 * `m_c = exp(logit_c)` is clamped strictly positive, so the learned distribution can only
 * redistribute samples between clusters - it can never zero the PMF support and therefore
 * never bias the image (worst case: more noise than the analytic baseline).
 *
 * Architecture (HWRT_NIS_* in eevee_raytrace_shared.hh):
 *   input  24 = world position, frequency-encoded (4 octaves x sin/cos x 3 axes)
 *   hidden 32, ReLU
 *   hidden 32, ReLU
 *   output 32 logits (HWRT_LIGHT_CLUSTER_COUNT)
 *
 * Weight layout in `hardware_nis_weights_buf` (float, HWRT_NIS_PARAM_COUNT):
 *   [W1: 24x32][b1: 32][W2: 32x32][b2: 32][W3: 32x32][b3: 32]
 * Row-major: W[layer][out_idx * in_dim + in_idx]. The last layer is zero-initialized so the
 * untrained network is exactly neutral (logits 0 -> m = 1 -> stage-N1 estimator).
 *
 * Consumers (selection + accumulation + trainer) must produce IDENTICAL outputs for the same
 * tile: all evaluate from the same weights buffer and the same reconstructed P.
 */

#define HWRT_NIS_IN 24
#define HWRT_NIS_HIDDEN 32
#define HWRT_NIS_OUT HWRT_LIGHT_CLUSTER_COUNT

#define HWRT_NIS_W1_OFFSET 0
#define HWRT_NIS_B1_OFFSET (HWRT_NIS_W1_OFFSET + HWRT_NIS_IN * HWRT_NIS_HIDDEN)
#define HWRT_NIS_W2_OFFSET (HWRT_NIS_B1_OFFSET + HWRT_NIS_HIDDEN)
#define HWRT_NIS_B2_OFFSET (HWRT_NIS_W2_OFFSET + HWRT_NIS_HIDDEN * HWRT_NIS_HIDDEN)
#define HWRT_NIS_W3_OFFSET (HWRT_NIS_B2_OFFSET + HWRT_NIS_HIDDEN)
#define HWRT_NIS_B3_OFFSET (HWRT_NIS_W3_OFFSET + HWRT_NIS_HIDDEN * HWRT_NIS_OUT)
#define HWRT_NIS_PARAM_COUNT (HWRT_NIS_B3_OFFSET + HWRT_NIS_OUT)

/** Frequency encoding of the shading position. Scene scale is normalized by the encoding
 * frequencies themselves (multi-octave); no bounds tracking needed. */
void hardware_nis_encode_position(float3 P, out float r_encoded[HWRT_NIS_IN])
{
  int write_index = 0;
  float frequency = 0.05f;
  for (int octave = 0; octave < 4; octave++) {
    const float3 phase = P * frequency;
    r_encoded[write_index++] = sin(phase.x);
    r_encoded[write_index++] = sin(phase.y);
    r_encoded[write_index++] = sin(phase.z);
    r_encoded[write_index++] = cos(phase.x);
    r_encoded[write_index++] = cos(phase.y);
    r_encoded[write_index++] = cos(phase.z);
    frequency *= 4.0f;
  }
}

/** Forward pass returning the raw logits. `r_h1`/`r_h2` expose the post-ReLU activations for
 * the training kernel's backprop; lighting kernels can ignore them. */
void hardware_nis_forward(float3 P,
                          out float r_h1[HWRT_NIS_HIDDEN],
                          out float r_h2[HWRT_NIS_HIDDEN],
                          out float r_logits[HWRT_NIS_OUT])
{
  float encoded[HWRT_NIS_IN];
  hardware_nis_encode_position(P, encoded);

  for (int j = 0; j < HWRT_NIS_HIDDEN; j++) {
    float acc = hardware_nis_weights_buf[HWRT_NIS_B1_OFFSET + j];
    for (int i = 0; i < HWRT_NIS_IN; i++) {
      acc += hardware_nis_weights_buf[HWRT_NIS_W1_OFFSET + j * HWRT_NIS_IN + i] * encoded[i];
    }
    r_h1[j] = max(acc, 0.0f);
  }
  for (int j = 0; j < HWRT_NIS_HIDDEN; j++) {
    float acc = hardware_nis_weights_buf[HWRT_NIS_B2_OFFSET + j];
    for (int i = 0; i < HWRT_NIS_HIDDEN; i++) {
      acc += hardware_nis_weights_buf[HWRT_NIS_W2_OFFSET + j * HWRT_NIS_HIDDEN + i] * r_h1[i];
    }
    r_h2[j] = max(acc, 0.0f);
  }
  for (int j = 0; j < HWRT_NIS_OUT; j++) {
    float acc = hardware_nis_weights_buf[HWRT_NIS_B3_OFFSET + j];
    for (int i = 0; i < HWRT_NIS_HIDDEN; i++) {
      acc += hardware_nis_weights_buf[HWRT_NIS_W3_OFFSET + j * HWRT_NIS_HIDDEN + i] * r_h2[i];
    }
    r_logits[j] = acc;
  }
}

/** Per-tile cluster multipliers `m_c = exp(clamp(logit_c))`. Neutral (all ones) when the
 * position is invalid or the network is disabled. */
void hardware_nis_cluster_multipliers(float3 P,
                                      bool P_valid,
                                      out float r_multipliers[HWRT_NIS_OUT])
{
  if (!P_valid || !raytrace_buf.hardware_nis_enable) {
    for (int c = 0; c < HWRT_NIS_OUT; c++) {
      r_multipliers[c] = 1.0f;
    }
    return;
  }
  float h1[HWRT_NIS_HIDDEN];
  float h2[HWRT_NIS_HIDDEN];
  float logits[HWRT_NIS_OUT];
  hardware_nis_forward(P, h1, h2, logits);
  for (int c = 0; c < HWRT_NIS_OUT; c++) {
    /* Clamp keeps the PMF support strictly positive (unbiasedness guard) and bounds the
     * dynamic range during early training. */
    r_multipliers[c] = exp(clamp(logits[c], -4.0f, 4.0f));
  }
}
