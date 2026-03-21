/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_ddx(float value_float,
              float3 value_vector,
              float4 value_color,
              out float out_float,
              out float3 out_vector,
              out float4 out_color)
{
  float derivative_scale = derivative_scale_get();
  out_float = gpu_dfdx(value_float) * derivative_scale;
  out_vector = gpu_dfdx(value_vector) * derivative_scale;
  out_color = gpu_dfdx(value_color) * derivative_scale;
}

[[node]]
void node_ddy(float value_float,
              float3 value_vector,
              float4 value_color,
              out float out_float,
              out float3 out_vector,
              out float4 out_color)
{
  float derivative_scale = derivative_scale_get();
  out_float = gpu_dfdy(value_float) * derivative_scale;
  out_vector = gpu_dfdy(value_vector) * derivative_scale;
  out_color = gpu_dfdy(value_color) * derivative_scale;
}

[[node]]
void node_ddxy(float value_float,
               float3 value_vector,
               float4 value_color,
               out float out_float,
               out float3 out_vector,
               out float4 out_color)
{
  float derivative_scale = derivative_scale_get();
  out_float = (gpu_dfdx(value_float) + gpu_dfdy(value_float)) * derivative_scale;
  out_vector = (gpu_dfdx(value_vector) + gpu_dfdy(value_vector)) * derivative_scale;
  out_color = (gpu_dfdx(value_color) + gpu_dfdy(value_color)) * derivative_scale;
}
