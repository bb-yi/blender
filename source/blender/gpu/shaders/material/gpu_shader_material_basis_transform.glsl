/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_safe_lib.glsl"

float3 basis_transform_fallback_result(float3 value, float fallback_mode)
{
  return (fallback_mode > 0.5f) ? float3(0.0f) : value;
}

void basis_transform_make_axes_raw(float3 x_axis,
                                   float3 y_axis,
                                   float3 z_axis,
                                   float basis_input,
                                   out float3 X,
                                   out float3 Y,
                                   out float3 Z)
{
  if (basis_input < 0.5f) {
    X = x_axis;
    Y = y_axis;
    Z = z_axis;
  }
  else if (basis_input < 1.5f) {
    X = x_axis;
    Y = y_axis;
    Z = cross(x_axis, y_axis);
  }
  else if (basis_input < 2.5f) {
    X = x_axis;
    Z = z_axis;
    Y = cross(z_axis, x_axis);
  }
  else {
    Y = y_axis;
    Z = z_axis;
    X = cross(y_axis, z_axis);
  }
}

void basis_transform_make_axes_orthonormal(float3 x_axis,
                                           float3 y_axis,
                                           float3 z_axis,
                                           float basis_input,
                                           out float3 X,
                                           out float3 Y,
                                           out float3 Z)
{
  if (basis_input < 0.5f) {
    X = safe_normalize(x_axis);
    Y = safe_normalize(y_axis - X * dot(X, y_axis));
    Z = safe_normalize(cross(X, Y));
    if (dot(Z, z_axis) < 0.0f) {
      Y *= -1.0f;
      Z *= -1.0f;
    }
    Y = safe_normalize(cross(Z, X));
  }
  else if (basis_input < 1.5f) {
    X = safe_normalize(x_axis);
    Z = safe_normalize(cross(X, y_axis));
    Y = safe_normalize(cross(Z, X));
  }
  else if (basis_input < 2.5f) {
    X = safe_normalize(x_axis);
    Y = safe_normalize(cross(z_axis, X));
    Z = safe_normalize(cross(X, Y));
  }
  else {
    Y = safe_normalize(y_axis);
    X = safe_normalize(cross(Y, z_axis));
    Z = safe_normalize(cross(X, Y));
  }
}

float basis_transform_determinant(float3 X, float3 Y, float3 Z)
{
  return dot(X, cross(Y, Z));
}

void basis_transform_reciprocal_basis(float3 X,
                                      float3 Y,
                                      float3 Z,
                                      out float3 rX,
                                      out float3 rY,
                                      out float3 rZ)
{
  const float det = basis_transform_determinant(X, Y, Z);
  rX = safe_divide(cross(Y, Z), det);
  rY = safe_divide(cross(Z, X), det);
  rZ = safe_divide(cross(X, Y), det);
}

float3 basis_transform_to_basis_vector(float3 value, float3 X, float3 Y, float3 Z)
{
  float3 rX, rY, rZ;
  basis_transform_reciprocal_basis(X, Y, Z, rX, rY, rZ);
  return float3(dot(rX, value), dot(rY, value), dot(rZ, value));
}

float3 basis_transform_from_basis_vector(float3 value, float3 X, float3 Y, float3 Z)
{
  return value.x * X + value.y * Y + value.z * Z;
}

float3 basis_transform_to_basis_normal(float3 value, float3 X, float3 Y, float3 Z)
{
  return float3(dot(X, value), dot(Y, value), dot(Z, value));
}

float3 basis_transform_from_basis_normal(float3 value, float3 X, float3 Y, float3 Z)
{
  float3 rX, rY, rZ;
  basis_transform_reciprocal_basis(X, Y, Z, rX, rY, rZ);
  return value.x * rX + value.y * rY + value.z * rZ;
}

[[node]]
void node_basis_transform(float3 value,
                          float3 origin,
                          float3 x_axis,
                          float3 y_axis,
                          float3 z_axis,
                          float vector_type,
                          float direction,
                          float basis_input,
                          float fallback_mode,
                          float orthonormalize,
                          out float3 result)
{
  float3 X, Y, Z;
  if (orthonormalize > 0.5f) {
    basis_transform_make_axes_orthonormal(x_axis, y_axis, z_axis, basis_input, X, Y, Z);
  }
  else {
    basis_transform_make_axes_raw(x_axis, y_axis, z_axis, basis_input, X, Y, Z);
  }

  const float det = basis_transform_determinant(X, Y, Z);
  if (abs(det) <= 1e-8f) {
    result = basis_transform_fallback_result(value, fallback_mode);
    return;
  }

  const bool is_point = (vector_type > 0.5f) && (vector_type < 1.5f);
  const bool is_normal = vector_type > 1.5f;
  const bool is_from_basis = direction > 0.5f;

  if (is_from_basis) {
    if (is_normal) {
      result = safe_normalize(basis_transform_from_basis_normal(value, X, Y, Z));
    }
    else {
      result = basis_transform_from_basis_vector(value, X, Y, Z);
      if (is_point) {
        result += origin;
      }
    }
  }
  else {
    float3 transformed_value = value;
    if (is_point) {
      transformed_value -= origin;
    }

    if (is_normal) {
      result = safe_normalize(basis_transform_to_basis_normal(transformed_value, X, Y, Z));
    }
    else {
      result = basis_transform_to_basis_vector(transformed_value, X, Y, Z);
    }
  }
}
