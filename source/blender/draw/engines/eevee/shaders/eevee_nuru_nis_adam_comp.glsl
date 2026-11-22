/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Nuru NIS stage N2: Adam optimizer step.
 *
 * One thread per network parameter. Consumes the gradients accumulated by the training pass
 * (averaged over the trained tile count), applies Adam, and clears the gradient slot for the
 * next frame. Thread 0 advances the shared step counter; the bias-correction term uses the
 * pre-increment count so every thread sees the same `t`.
 */

#include "infos/eevee_nuru_direct_light_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_nuru_nis_adam)

void main()
{
  const uint param_index = gl_GlobalInvocationID.x;
  if (hardware_nis_finalize) {
    if (param_index == 0u && hardware_nis_train_count_buf[0] > 0u) {
      hardware_nis_train_count_buf[0] = 0u;
      hardware_nis_train_count_buf[1] += 1u;
    }
    return;
  }
  if (param_index >= uint(hardware_nis_param_count)) {
    return;
  }
  const uint sample_count = hardware_nis_train_count_buf[0];
  if (sample_count == 0u) {
    return;
  }

  const float gradient = (float(hardware_nis_grads_buf[param_index]) / 4096.0f) /
                         float(sample_count);
  hardware_nis_grads_buf[param_index] = 0;
  /* Finalization is a separate dispatch, so all workgroups observe the same count and step. */
  const float t = float(hardware_nis_train_count_buf[1] + 1u);

  if (!(abs(gradient) < 1.0e16f)) {
    /* NaN/Inf guard: drop the step entirely for this parameter. */
    return;
  }

  const float learning_rate = 1.0e-2f;
  const float beta1 = 0.9f;
  const float beta2 = 0.999f;
  const float epsilon = 1.0e-8f;

  float m = hardware_nis_adam_m_buf[param_index];
  float v = hardware_nis_adam_v_buf[param_index];
  m = beta1 * m + (1.0f - beta1) * gradient;
  v = beta2 * v + (1.0f - beta2) * gradient * gradient;
  hardware_nis_adam_m_buf[param_index] = m;
  hardware_nis_adam_v_buf[param_index] = v;

  const float m_hat = m / (1.0f - pow(beta1, t));
  const float v_hat = v / (1.0f - pow(beta2, t));
  const float step = learning_rate * m_hat / (sqrt(v_hat) + epsilon);

  hardware_nis_weights_buf[param_index] -= clamp(step, -0.05f, 0.05f);
}
