/* SPDX-FileCopyrightText: 2019-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_common_math.glsl"
#include "gpu_shader_math_fast_lib.glsl"
#include "gpu_shader_math_vector_safe_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

float3 tint_from_color(float3 color)
{
  float lum = dot(color, float3(0.3f, 0.6f, 0.1f)); /* luminance approx. */
  return (lum > 0.0f) ? color / lum : float3(1.0f); /* normalize lum. to isolate hue+sat */
}

float principled_sheen(float NV, float rough)
{
  /* Empirical approximation (manual curve fitting) to the sheen_weight albedo. Can be refined. */
  float den = 35.6694f * rough * rough - 24.4269f * rough * NV - 0.1405f * NV * NV +
              6.1211f * rough + 0.28105f * NV - 0.1405f;
  float num = 58.5299f * rough * rough - 85.0941f * rough * NV + 9.8955f * NV * NV +
              1.9250f * rough + 74.2268f * NV - 0.2246f;
  return saturate(den / num);
}

float ior_from_F0(float F0)
{
  float f = sqrt(clamp(F0, 0.0f, 0.99f));
  return (-f - 1.0f) / (f - 1.0f);
}

float thin_glass_transmission_roughness(float roughness, float ior)
{
  return saturate(roughness *
                  sqrt(sqrt(3.4f * (ior - 1.0f) * square(ior - 0.5f) / (square(ior) * ior))));
}

/* Given the transmittance through a slab at normal incidence, compute the transmittance at a
 * certain incident angle, based on Beer-Lambert law. */
float3 slab_transmittance_at_angle(float3 color, float cos_theta_i, float ior)
{
  const float inv_cos_theta_t = ior / sqrt_fast(square(ior) - (1.0f - square(cos_theta_i)));
  return pow(color, float3(inv_cos_theta_t));
}
