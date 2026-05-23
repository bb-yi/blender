/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#define PARALLAX_MODE_PLANE_OFFSET 0
#define PARALLAX_MODE_OCCLUSION 2
#define PARALLAX_MODE_RELIEF 3
#define PARALLAX_MODE_SECANT_RELIEF 4

float3 parallax_safe_normalize(float3 v)
{
  float length_squared = dot(v, v);
  if (length_squared <= 1.0e-35f) {
    return float3(0.0f);
  }
  return v * inversesqrt(length_squared);
}

bool parallax_view_from_tangent_direction(float3 tangent_view_direction,
                                          out float2 view_dir,
                                          out float view_z)
{
  float3 Vt = parallax_safe_normalize(tangent_view_direction);
  if (dot(Vt, Vt) <= 1.0e-12f) {
    view_dir = float2(0.0f);
    view_z = 1.0f;
    return false;
  }
  view_z = max(abs(Vt.z), 1.0e-5f);
  view_dir = Vt.xy / view_z;
  return dot(view_dir, view_dir) > 1.0e-12f;
}

bool parallax_tangent_basis(float4 tangent, out float3 T, out float3 B, out float3 N)
{
#ifdef GPU_FRAGMENT_SHADER
  if (all(equal(tangent, float4(0.0f, 0.0f, 0.0f, 1.0f)))) {
    T = float3(1.0f, 0.0f, 0.0f);
    B = float3(0.0f, 1.0f, 0.0f);
    N = g_data.Ni;
    return false;
  }

  tangent *= (FrontFacing ? 1.0f : -1.0f);
  N = parallax_safe_normalize(g_data.Ni);
  T = parallax_safe_normalize(tangent.xyz);
  T = cross(N, parallax_safe_normalize(cross(T, N)));
  B = tangent.w * parallax_safe_normalize(cross(N, T));
  B *= (drw_object_infos().flag & OBJECT_NEGATIVE_SCALE) != 0 ? -1.0f : 1.0f;
  return true;
#else
  T = float3(1.0f, 0.0f, 0.0f);
  B = float3(0.0f, 1.0f, 0.0f);
  N = g_data.N;
  return false;
#endif
}

bool parallax_view_data(float4 tangent, out float2 view_dir, out float view_z)
{
  float3 T;
  float3 B;
  float3 N;
  if (!parallax_tangent_basis(tangent, T, B, N)) {
    view_dir = float2(0.0f);
    view_z = 1.0f;
    return false;
  }

#ifdef GPU_FRAGMENT_SHADER
  float3 V = parallax_safe_normalize(drw_world_incident_vector(g_data.P));
  float3 Vt = float3(dot(V, T), dot(V, B), dot(V, N));
  return parallax_view_from_tangent_direction(Vt, view_dir, view_z);
#else
  view_dir = float2(0.0f);
  view_z = 1.0f;
  return false;
#endif
}

float3 parallax_base_normal(float4 tangent)
{
  float3 T;
  float3 B;
  float3 N;
  if (!parallax_tangent_basis(tangent, T, B, N)) {
    return g_data.N;
  }
  return N;
}

float3 parallax_tangent_normal_to_world(float4 tangent, float3 tangent_normal)
{
  float3 T;
  float3 B;
  float3 N;
  if (!parallax_tangent_basis(tangent, T, B, N)) {
    return g_data.N;
  }
  return parallax_safe_normalize(tangent_normal.x * T + tangent_normal.y * B + tangent_normal.z * N);
}

float3 parallax_world_direction_to_tangent(float4 tangent, float3 world_direction)
{
  float3 T;
  float3 B;
  float3 N;
  if (!parallax_tangent_basis(tangent, T, B, N)) {
    return float3(0.0f, 0.0f, 1.0f);
  }
  float3 direction = parallax_safe_normalize(world_direction);
  if (dot(direction, direction) <= 1.0e-12f) {
    return float3(0.0f, 0.0f, 1.0f);
  }
  return float3(dot(direction, T), dot(direction, B), dot(direction, N));
}

float3 parallax_normal_from_height_samples(float4 tangent,
                                           float center_height,
                                           float u_height,
                                           float v_height,
                                           float u_step,
                                           float v_step,
                                           float scale)
{
  float dheight_du = (u_height - center_height) / max(u_step, 1.0e-5f);
  float dheight_dv = (v_height - center_height) / max(v_step, 1.0e-5f);
  float3 tangent_normal = parallax_safe_normalize(
      float3(-dheight_du * scale, -dheight_dv * scale, 1.0f));
  return parallax_tangent_normal_to_world(tangent, tangent_normal);
}

float2 parallax_offset_direction(float4 tangent)
{
  float2 view_dir;
  float view_z;
  if (!parallax_view_data(tangent, view_dir, view_z)) {
    return float2(0.0f);
  }
  return view_dir;
}

[[node]]
void node_parallax_plane_offset(float3 uv_in,
                                float scale,
                                float4 tangent,
                                out float3 uv_out,
                                out float3 normal_out)
{
  float2 direction = parallax_offset_direction(tangent);
  uv_out = float3(uv_in.xy - direction * scale, uv_in.z);
  normal_out = parallax_base_normal(tangent);
}

void parallax_uv_gradients(float3 uv_in, out float2 uv_dx, out float2 uv_dy)
{
#ifdef GPU_FRAGMENT_SHADER
  uv_dx = gpu_dfdx(uv_in.xy) * texture_lod_bias_get();
  uv_dy = gpu_dfdy(uv_in.xy) * texture_lod_bias_get();
#else
  uv_dx = float2(0.0f);
  uv_dy = float2(0.0f);
#endif
}

float parallax_height_image(sampler2D height_image, float2 uv, float2 uv_dx, float2 uv_dy)
{
#ifdef GPU_FRAGMENT_SHADER
  return textureGrad(height_image, uv, uv_dx, uv_dy).r;
#else
  return texture(height_image, uv).r;
#endif
}

float parallax_height_offset(float height, float offset)
{
  return height + offset;
}

float2 parallax_interpolate_uv(float2 after_uv,
                               float after_height,
                               float after_ray_height,
                               float2 before_uv,
                               float before_height,
                               float before_ray_height)
{
  float after_delta = after_height - after_ray_height;
  float before_delta = before_height - before_ray_height;
  if (after_delta >= 0.0f && before_delta <= 0.0f) {
    float denom = after_delta - before_delta;
    float weight = denom > 1.0e-5f ? clamp(after_delta / denom, 0.0f, 1.0f) : 0.5f;
    return mix(after_uv, before_uv, weight);
  }
  return abs(after_delta) <= abs(before_delta) ? after_uv : before_uv;
}

float3 parallax_normal_image(sampler2D height_image,
                             float2 uv,
                             float scale,
                             float height_offset,
                             float4 tangent,
                             float2 uv_dx,
                             float2 uv_dy)
{
  float2 texture_size = max(float2(textureSize(height_image, 0).xy), float2(1.0f));
  float2 texel_size = 1.0f / texture_size;
  float center_height = parallax_height_offset(parallax_height_image(height_image, uv, uv_dx, uv_dy),
                                              height_offset);
  float u_height = parallax_height_offset(
      parallax_height_image(height_image, uv + float2(texel_size.x, 0.0f), uv_dx, uv_dy),
      height_offset);
  float v_height = parallax_height_offset(
      parallax_height_image(height_image, uv + float2(0.0f, texel_size.y), uv_dx, uv_dy),
      height_offset);
  return parallax_normal_from_height_samples(
      tangent, center_height, u_height, v_height, texel_size.x, texel_size.y, scale);
}

void parallax_march_init(float4 tangent,
                         float min_steps,
                         float max_steps,
                         out float2 direction,
                         out float layer_count,
                         out float layer_depth)
{
  float view_z;
  if (!parallax_view_data(tangent, direction, view_z)) {
    layer_count = 1.0f;
    layer_depth = 1.0f;
    return;
  }
  float min_count = clamp(floor(min_steps + 0.5f), 1.0f, 128.0f);
  float max_count = clamp(floor(max_steps + 0.5f), min_count, 128.0f);
  layer_count = clamp(ceil(mix(max_count, min_count, clamp(view_z, 0.0f, 1.0f))),
                      min_count,
                      max_count);
  layer_depth = 1.0f / max(layer_count, 1.0f);
}

void parallax_trace_image(sampler2D height_image,
                          float3 uv_in,
                          float scale,
                          float height_offset,
                          float min_steps,
                          float max_steps,
                          float refinement_steps,
                          int mode,
                          float4 tangent,
                          out float3 uv_out,
                          out float hit_height)
{
  float2 direction;
  float layer_count;
  float layer_depth;
  parallax_march_init(tangent, min_steps, max_steps, direction, layer_count, layer_depth);

  float2 uv_dx;
  float2 uv_dy;
  parallax_uv_gradients(uv_in, uv_dx, uv_dy);

  float2 delta_uv = direction * scale / max(layer_count, 1.0f);
  float2 previous_uv = uv_in.xy;
  float previous_ray_height = 1.0f;
  float2 current_uv = uv_in.xy - delta_uv;
  float current_height = parallax_height_offset(parallax_height_image(height_image,
                                                                      current_uv,
                                                                      uv_dx,
                                                                      uv_dy),
                                               height_offset);
  float previous_height = parallax_height_offset(parallax_height_image(height_image,
                                                                       uv_in.xy,
                                                                       uv_dx,
                                                                       uv_dy),
                                                height_offset);
  float ray_height = 1.0f - layer_depth;

  for (int i = 0; i < 128; i++) {
    if (i >= int(layer_count) || current_height > ray_height) {
      break;
    }
    previous_uv = current_uv;
    previous_ray_height = ray_height;
    previous_height = current_height;
    ray_height -= layer_depth;
    current_uv -= delta_uv;
    current_height = parallax_height_offset(parallax_height_image(height_image,
                                                                  current_uv,
                                                                  uv_dx,
                                                                  uv_dy),
                                           height_offset);
  }

  if (mode == PARALLAX_MODE_OCCLUSION) {
    uv_out = float3(parallax_interpolate_uv(current_uv,
                                            current_height,
                                            ray_height,
                                            previous_uv,
                                            previous_height,
                                            previous_ray_height),
                    uv_in.z);
  }
  else if (mode == PARALLAX_MODE_RELIEF) {
    float2 after_uv = current_uv;
    float after_height = current_height;
    float after_ray_height = ray_height;
    float2 before_uv = previous_uv;
    float before_height = previous_height;
    float before_ray_height = previous_ray_height;
    int refine_count = int(clamp(floor(refinement_steps + 0.5f), 0.0f, 8.0f));
    for (int i = 0; i < 8; i++) {
      if (i >= refine_count) {
        break;
      }
      float2 middle_uv = (after_uv + before_uv) * 0.5f;
      float middle_ray_height = (after_ray_height + before_ray_height) * 0.5f;
      float middle_height = parallax_height_offset(parallax_height_image(height_image,
                                                                         middle_uv,
                                                                         uv_dx,
                                                                         uv_dy),
                                                  height_offset);
      float delta = middle_height - middle_ray_height;
      if (delta > 0.0f) {
        after_uv = middle_uv;
        after_height = middle_height;
        after_ray_height = middle_ray_height;
      }
      else {
        before_uv = middle_uv;
        before_height = middle_height;
        before_ray_height = middle_ray_height;
      }
      if (abs(delta) <= 0.01f) {
        break;
      }
    }
    uv_out = float3(parallax_interpolate_uv(after_uv,
                                            after_height,
                                            after_ray_height,
                                            before_uv,
                                            before_height,
                                            before_ray_height),
                    uv_in.z);
  }
  else if (mode == PARALLAX_MODE_SECANT_RELIEF) {
    float2 before_uv = previous_uv;
    float before_ray_height = previous_ray_height;
    float before_height = previous_height;
    float before_delta = before_ray_height - before_height;
    float2 after_uv = current_uv;
    float after_ray_height = ray_height;
    float after_height = current_height;
    float after_delta = after_ray_height - after_height;
    int refine_count = int(clamp(floor(refinement_steps + 0.5f), 0.0f, 8.0f));
    for (int i = 0; i < 8; i++) {
      if (i >= refine_count || abs(after_delta - before_delta) <= 1.0e-5f) {
        break;
      }
      float intersection_ray_height = (before_ray_height * after_delta -
                                       after_ray_height * before_delta) /
                                      (after_delta - before_delta);
      float2 intersection_uv = uv_in.xy -
                               (1.0f - intersection_ray_height) * delta_uv * layer_count;
      float intersection_height = parallax_height_offset(
          parallax_height_image(height_image, intersection_uv, uv_dx, uv_dy), height_offset);
      float delta = intersection_ray_height - intersection_height;
      if (delta < 0.0f) {
        after_uv = intersection_uv;
        after_ray_height = intersection_ray_height;
        after_height = intersection_height;
        after_delta = delta;
      }
      else {
        before_uv = intersection_uv;
        before_ray_height = intersection_ray_height;
        before_height = intersection_height;
        before_delta = delta;
      }
      if (abs(delta) <= 0.01f) {
        break;
      }
    }
    uv_out = float3(parallax_interpolate_uv(after_uv,
                                            after_height,
                                            after_ray_height,
                                            before_uv,
                                            before_height,
                                            before_ray_height),
                    uv_in.z);
  }
  else {
    uv_out = float3(current_uv, uv_in.z);
  }
  hit_height = parallax_height_offset(parallax_height_image(height_image, uv_out.xy, uv_dx, uv_dy),
                                      height_offset);
}

float parallax_shadow_image(sampler2D height_image,
                            float2 uv,
                            float hit_height,
                            float scale,
                            float height_offset,
                            float min_steps,
                            float max_steps,
                            float3 sun_direction,
                            float2 uv_dx,
                            float2 uv_dy)
{
  float3 L = parallax_safe_normalize(sun_direction);
  if (dot(L, L) <= 1.0e-12f) {
    return 1.0f;
  }
  if (L.z <= 1.0e-5f) {
    return L.z <= 1.0e-5f ? 0.0f : 1.0f;
  }

  float min_count = clamp(floor(min_steps + 0.5f), 1.0f, 128.0f);
  float max_count = clamp(floor(max_steps + 0.5f), min_count, 128.0f);
  float layer_count = clamp(ceil(mix(max_count, min_count, clamp(abs(L.z), 0.0f, 1.0f))),
                            min_count,
                            max_count);
  float layer_depth = (1.0f - hit_height) / max(layer_count, 1.0f);
  if (layer_depth <= 1.0e-6f) {
    return 1.0f;
  }
  float2 delta_uv = scale * L.xy / max(L.z * layer_count, 1.0e-5f);
  float2 current_uv_offset = delta_uv;
  float current_height = parallax_height_offset(parallax_height_image(height_image,
                                                                      uv + current_uv_offset,
                                                                      uv_dx,
                                                                      uv_dy),
                                               height_offset);
  float ray_height = hit_height + layer_depth;

  for (int i = 0; i < 128; i++) {
    if (i >= int(layer_count) || ray_height >= 1.0f) {
      break;
    }
    if (current_height > ray_height) {
      return 0.0f;
    }
    ray_height += layer_depth;
    current_uv_offset += delta_uv;
    current_height = parallax_height_offset(parallax_height_image(height_image,
                                                                  uv + current_uv_offset,
                                                                  uv_dx,
                                                                  uv_dy),
                                           height_offset);
  }
  return 1.0f;
}

void node_parallax_image_mode(sampler2D height_image,
                              float3 uv_in,
                              float scale,
                              float height_offset,
                              float min_steps,
                              float max_steps,
                              float refinement_steps,
                              int mode,
                              bool use_normal,
                              bool use_shadow,
                              float3 sun_direction,
                              float4 tangent,
                              out float3 uv_out,
                              out float3 normal_out,
                              out float shadow_out)
{
  shadow_out = 1.0f;
  normal_out = parallax_base_normal(tangent);
  if (scale == 0.0f) {
    uv_out = uv_in;
    return;
  }
  if (mode == PARALLAX_MODE_PLANE_OFFSET) {
    node_parallax_plane_offset(uv_in, scale, tangent, uv_out, normal_out);
    return;
  }
  float hit_height;
  parallax_trace_image(height_image,
                       uv_in,
                       scale,
                       height_offset,
                       min_steps,
                       max_steps,
                       refinement_steps,
                       mode,
                       tangent,
                       uv_out,
                       hit_height);
  float2 uv_dx;
  float2 uv_dy;
  parallax_uv_gradients(uv_in, uv_dx, uv_dy);
  if (use_normal) {
    normal_out = parallax_normal_image(
        height_image, uv_out.xy, scale, height_offset, tangent, uv_dx, uv_dy);
  }
  if (use_shadow) {
    float3 sun_direction_ts = parallax_world_direction_to_tangent(tangent, sun_direction);
    shadow_out = parallax_shadow_image(height_image,
                                       uv_out.xy,
                                       hit_height,
                                       scale,
                                       height_offset,
                                       min_steps,
                                       max_steps,
                                       sun_direction_ts,
                                       uv_dx,
                                       uv_dy);
  }
}
