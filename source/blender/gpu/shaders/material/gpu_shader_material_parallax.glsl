/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#define PARALLAX_MODE_PLANE_OFFSET 0
#define PARALLAX_MODE_STEEP 1
#define PARALLAX_MODE_OCCLUSION 2

float3 parallax_safe_normalize(float3 v)
{
  float length_squared = dot(v, v);
  if (length_squared <= 1.0e-35f) {
    return float3(0.0f);
  }
  return v * inversesqrt(length_squared);
}

bool parallax_view_data(float2 uv, out float2 view_dir, out float view_z)
{
#ifdef GPU_FRAGMENT_SHADER
  float3 N = parallax_safe_normalize(g_data.N);
  float3 dPdx = gpu_dfdx(g_data.P) * derivative_scale_get();
  float3 dPdy = gpu_dfdy(g_data.P) * derivative_scale_get();
  float2 dUVdx = gpu_dfdx(uv);
  float2 dUVdy = gpu_dfdy(uv);

  float det = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;
  if (abs(det) < 1.0e-8f) {
    view_dir = float2(0.0f);
    view_z = 1.0f;
    return false;
  }

  float inv_det = 1.0f / det;
  float3 T = parallax_safe_normalize((dPdx * dUVdy.y - dPdy * dUVdx.y) * inv_det);
  float3 B = parallax_safe_normalize((-dPdx * dUVdy.x + dPdy * dUVdx.x) * inv_det);

  float3 V = parallax_safe_normalize(-drw_world_incident_vector(g_data.P));
  float3 Vt = float3(dot(V, T), dot(V, B), dot(V, N));
  view_z = max(abs(Vt.z), 0.08f);
  view_dir = Vt.xy / view_z;
  return dot(view_dir, view_dir) > 1.0e-12f;
#else
  view_dir = float2(0.0f);
  view_z = 1.0f;
  return false;
#endif
}

float2 parallax_offset_direction(float2 uv)
{
  float2 view_dir;
  float view_z;
  if (!parallax_view_data(uv, view_dir, view_z)) {
    return float2(0.0f);
  }
  return view_dir;
}

[[node]]
void node_parallax_plane_offset(float3 uv_in, float scale, out float3 uv_out)
{
  float2 direction = parallax_offset_direction(uv_in.xy);
  uv_out = float3(uv_in.xy - direction * scale, uv_in.z);
}

float parallax_height_image(sampler2D height_image, float2 uv)
{
#ifdef GPU_FRAGMENT_SHADER
  float2 dx = gpu_dfdx(uv) * texture_lod_bias_get();
  float2 dy = gpu_dfdy(uv) * texture_lod_bias_get();
  return textureGrad(height_image, uv, dx, dy).r;
#else
  return texture(height_image, uv).r;
#endif
}

void parallax_march_init(float2 uv,
                         float min_steps,
                         float max_steps,
                         out float2 direction,
                         out float layer_count,
                         out float layer_depth)
{
  float view_z;
  if (!parallax_view_data(uv, direction, view_z)) {
    layer_count = 1.0f;
    layer_depth = 1.0f;
    return;
  }
  float min_count = clamp(floor(min_steps + 0.5f), 1.0f, 128.0f);
  float max_count = clamp(floor(max_steps + 0.5f), min_count, 128.0f);
  layer_count = mix(max_count, min_count, clamp(view_z, 0.0f, 1.0f));
  layer_depth = 1.0f / max(layer_count, 1.0f);
}

void parallax_steep_image(sampler2D height_image,
                          float3 uv_in,
                          float scale,
                          float midlevel,
                          float min_steps,
                          float max_steps,
                          bool refine,
                          float refinement_steps,
                          out float3 uv_out)
{
  float2 direction;
  float layer_count;
  float layer_depth;
  parallax_march_init(uv_in.xy, min_steps, max_steps, direction, layer_count, layer_depth);

  float2 delta_uv = direction * scale / max(layer_count, 1.0f);
  float2 current_uv = uv_in.xy;
  float2 previous_uv = current_uv;
  float current_depth = 0.0f;
  float previous_depth = 0.0f;
  float current_height = parallax_height_image(height_image, current_uv);

  for (int i = 0; i < 128; i++) {
    if (i >= int(layer_count) || current_depth >= current_height) {
      break;
    }
    previous_uv = current_uv;
    previous_depth = current_depth;
    current_uv -= delta_uv;
    current_depth += layer_depth;
    current_height = parallax_height_image(height_image, current_uv);
  }

  float2 result_uv = current_uv;
  if (refine) {
    float2 low_uv = previous_uv;
    float2 high_uv = current_uv;
    float low_depth = previous_depth;
    float high_depth = current_depth;
    int refine_count = int(clamp(floor(refinement_steps + 0.5f), 0.0f, 8.0f));
    for (int i = 0; i < 8; i++) {
      if (i >= refine_count) {
        break;
      }
      float2 mid_uv = (low_uv + high_uv) * 0.5f;
      float mid_depth = (low_depth + high_depth) * 0.5f;
      float mid_height = parallax_height_image(height_image, mid_uv);
      if (mid_depth < mid_height) {
        low_uv = mid_uv;
        low_depth = mid_depth;
      }
      else {
        high_uv = mid_uv;
        high_depth = mid_depth;
      }
    }
    result_uv = (low_uv + high_uv) * 0.5f;
  }

  float2 plane_correction = direction * (midlevel * scale);
  uv_out = float3(result_uv + plane_correction, uv_in.z);
}

void node_parallax_image_mode(sampler2D height_image,
                              float3 uv_in,
                              float scale,
                              float midlevel,
                              float min_steps,
                              float max_steps,
                              float refinement_steps,
                              int mode,
                              out float3 uv_out)
{
  if (scale == 0.0f) {
    uv_out = uv_in;
    return;
  }
  if (mode == PARALLAX_MODE_PLANE_OFFSET) {
    node_parallax_plane_offset(uv_in, scale, uv_out);
    return;
  }
  if (mode == PARALLAX_MODE_OCCLUSION) {
    parallax_steep_image(
        height_image, uv_in, scale, midlevel, min_steps, max_steps, true, refinement_steps, uv_out);
    return;
  }
  parallax_steep_image(
      height_image, uv_in, scale, midlevel, min_steps, max_steps, false, refinement_steps, uv_out);
}
