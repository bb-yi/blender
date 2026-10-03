/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Nuru NIS stage N2: online KL-divergence training pass.
 *
 * One thread per direct-light work tile. Recomputes the deterministic forward state for the
 * tile (same inputs as the lighting kernels), forms the KL gradient at the output logits from
 * the realized contribution estimate `train_luma` (= L/p, written back by the accumulation
 * kernel), backpropagates by hand through the two hidden layers, and accumulates parameter
 * gradients into `hardware_nis_grads_buf` with CAS float atomics. The Adam kernel then applies
 * the averaged step once per frame.
 *
 * Gradient at the logits (residual softmax form, paper Sec. 4.2):
 *   q_c = s_c * m_c / sum(s * m)   with m_c = exp(z_c)
 *   dKL/dz_j = -(L/p) * (delta_{j,c_picked} - q_j)
 */

#include "infos/eevee_nuru_direct_light_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_nuru_nis_train)

#include "eevee_nuru_direct_light_lib.glsl"
#include "eevee_nuru_direct_light_importance_lib.glsl"
#include "eevee_reverse_z_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

/** Fixed-point gradient accumulation: portable signed atomicAdd on int (float atomics and
 * compare-swap are not available across all our GLSL backends). Scale chosen for headroom:
 * |grad| per sample is clamped to 64, giving < 2^19 ticks/sample; thousands of samples stay
 * far from int32 overflow. */
#define HWRT_NIS_GRAD_FIXED_SCALE 4096.0f

void hardware_nis_grad_add(uint param_index, float value)
{
  const float clamped = clamp(value, -64.0f, 64.0f);
  atomicAdd(hardware_nis_grads_buf[param_index], int(clamped * HWRT_NIS_GRAD_FIXED_SCALE));
}

void main()
{
  const uint queue_index = gl_GlobalInvocationID.x;
  if (queue_index >= uint(hardware_direct_light_tile_capacity)) {
    return;
  }
  /* Training budget: stride the tiles, rotating the offset per frame so all tiles train. */
  const uint stride = max(uint(hardware_nis_train_stride), 1u);
  if ((queue_index % stride) != (uint(hardware_nis_train_offset) % stride)) {
    return;
  }
  if (!raytrace_buf.hardware_nis_enable) {
    return;
  }

  const HardwareDirectLightVisibilitySample visibility_record =
      hardware_direct_light_visibility_samples_buf[queue_index];
  if (visibility_record.local_light_index == 0xFFFFFFFFu ||
      !(visibility_record.local_importance > 0.0f))
  {
    return;
  }
  const int2 texel = int2(unpackUvec2x16(visibility_record.packed_sample_texel));
  if (any(lessThan(texel, int2(0))) || any(greaterThanEqual(texel, textureSize(depth_tx, 0)))) {
    return;
  }
  const float depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
  if (!(depth > 0.0f && depth < 1.0f)) {
    return;
  }
  const float2 uv = (float2(texel) + 0.5f) * raytrace_buf.full_resolution_inv;
  const float3 P = drw_point_screen_to_world(float3(uv, depth));

  const HardwareDirectLightWorkTile work_tile = hardware_direct_light_work_tiles_buf[queue_index];
  float cluster_sums[HWRT_LIGHT_CLUSTER_COUNT];
  hardware_light_cluster_sums(work_tile, P, true, cluster_sums);

  /* Forward pass (activations kept for backprop). */
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

  const uint picked_cluster =
      uint(light_buf[visibility_record.local_light_index].hwrt_cluster_id) %
      uint(HWRT_LIGHT_CLUSTER_COUNT);

  /* Clamp the target weight: a single firefly must not destabilize the optimizer. */
  const float target_weight = min(visibility_record.train_luma, 32.0f);

  /* Output-layer gradient. */
  float grad_logits[HWRT_NIS_OUT];
  for (int j = 0; j < HWRT_NIS_OUT; j++) {
    const float indicator = (uint(j) == picked_cluster) ? 1.0f : 0.0f;
    float g = -target_weight * (indicator - q[j]);
    /* Logit clamp boundary: stop pushing past the clamp. */
    if ((logits[j] >= 4.0f && g < 0.0f) || (logits[j] <= -4.0f && g > 0.0f)) {
      g = 0.0f;
    }
    grad_logits[j] = g;
  }

  /* Backprop: layer 3. */
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

  /* Layer 2 (ReLU mask). */
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

  /* Layer 1 (ReLU mask; inputs re-encoded). */
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
