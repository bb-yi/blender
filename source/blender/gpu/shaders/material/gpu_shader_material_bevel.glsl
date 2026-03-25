/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float3 bevel_normalize_or(float3 value, float3 fallback)
{
  float value_len_sq = dot(value, value);
  if (value_len_sq > 1e-8f) {
    return value * inversesqrt(value_len_sq);
  }

  float fallback_len_sq = dot(fallback, fallback);
  if (fallback_len_sq > 1e-8f) {
    return fallback * inversesqrt(fallback_len_sq);
  }

  return float3(0.0f, 0.0f, 1.0f);
}

#if defined(MAT_RAYCAST)
#define BEVEL_PI 3.14159265358979323846f

float2 bevel_texel_to_uv(int2 texel, int2 tex_size)
{
  return (float2(texel) + 0.5f) / float2(tex_size);
}

float bevel_spatial_noise(float seed, float object_seed)
{
  return interleaved_gradient_noise(gl_FragCoord.xy, seed + object_seed, 0.0f);
}

float bevel_cubic_eval(float radius, float r)
{
  if (r >= radius) {
    return 0.0f;
  }

  float radius_sq = radius * radius;
  float radius_quartic = radius_sq * radius_sq;
  float radius_quintic = radius_quartic * radius;
  float falloff = radius - r;
  float numerator = falloff * falloff * falloff;
  return (10.0f * numerator) / max(radius_quintic * BEVEL_PI, 1e-8f);
}

float bevel_cubic_pdf(float radius, float r)
{
  return bevel_cubic_eval(radius, r);
}

float bevel_cubic_quintic_root_find(float xi)
{
  const float tolerance = 1e-6f;
  const int max_iteration_count = 10;
  float x = 0.25f;

  for (int iteration = 0; iteration < max_iteration_count; iteration++) {
    float x2 = x * x;
    float x3 = x2 * x;
    float one_minus_x = 1.0f - x;
    float f = 10.0f * x2 - 20.0f * x3 + 15.0f * x2 * x2 - 4.0f * x2 * x3 - xi;
    float f_derivative = 20.0f * (x * one_minus_x) * (one_minus_x * one_minus_x);

    if (abs(f) < tolerance || abs(f_derivative) <= 1e-8f) {
      break;
    }

    x = saturate(x - f / f_derivative);
  }

  return x;
}

float bevel_cubic_sample_radius(float radius, float xi)
{
  return bevel_cubic_quintic_root_find(xi) * radius;
}

int2 bevel_uv_to_texel(float2 uv, int2 tex_size)
{
  return clamp(int2(uv * float2(tex_size)), int2(0), tex_size - int2(1));
}

uint bevel_object_id_fetch(int2 texel)
{
  return texelFetch(object_id_tx, texel, 0).x;
}

float bevel_depth_fetch(float2 uv)
{
  return textureLod(hiz_tx, uv, 0.0f).r;
}

float3 bevel_prepass_normal_fetch(int2 texel, float3 fallback)
{
  float3 sample_normal = texelFetch(prepass_normal_tx, texel, 0).xyz * 2.0f - 1.0f;
  return bevel_normalize_or(sample_normal, fallback);
}

float bevel_projected_radius_pixels(float radius, int2 tex_size)
{
  if (radius <= 1e-8f) {
    return 0.0f;
  }

  float3 vP = drw_point_world_to_view(g_data.P);
  float homcoord = drw_view().winmat[2][3] * vP.z + drw_view().winmat[3][3];
  if (abs(homcoord) <= 1e-8f) {
    return 0.0f;
  }

  float2 sample_scale = abs(float2(drw_view().winmat[0][0], drw_view().winmat[1][1]) *
                            (0.5f * radius / homcoord));
  float2 pixel_radius = sample_scale * float2(tex_size);
  return max(pixel_radius.x, pixel_radius.y);
}

void bevel_accumulate_sample(uint self_id,
                             int2 tex_size,
                             float2 sample_uv,
                             float radius,
                             float disk_radius,
                             float3 center_position,
                             float3 fallback_normal,
                             inout float3 accum,
                             inout float accum_weight)
{
  if (sample_uv.x <= 0.0f || sample_uv.x >= 1.0f || sample_uv.y <= 0.0f || sample_uv.y >= 1.0f) {
    return;
  }

  int2 sample_texel = bevel_uv_to_texel(sample_uv, tex_size);
  if (bevel_object_id_fetch(sample_texel) != self_id) {
    return;
  }

  float2 sample_uv_center = bevel_texel_to_uv(sample_texel, tex_size);
  float sample_depth = bevel_depth_fetch(sample_uv_center);
  if (sample_depth >= 1.0f) {
    return;
  }

  float3 sample_position = drw_point_screen_to_world(float3(sample_uv_center, sample_depth));
  float sample_distance = distance(center_position, sample_position);
  float disk_pdf = bevel_cubic_pdf(radius, disk_radius);
  float sample_pdf = bevel_cubic_pdf(radius, sample_distance);
  float weight = (disk_pdf > 1e-8f) ? (sample_pdf / disk_pdf) : 0.0f;
  if (weight <= 0.0f) {
    return;
  }

  float3 sample_normal = bevel_prepass_normal_fetch(sample_texel, fallback_normal);
  accum += sample_normal * weight;
  accum_weight += weight;
}

#undef BEVEL_PI
#endif

[[node]]
void node_bevel(float radius, float3 N, float samples, float3 &result)
{
  float3 base_normal = bevel_normalize_or(g_data.N, g_data.Ng);
  float3 ref_normal = bevel_normalize_or(N, base_normal);

#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER)) && defined(MAT_RAYCAST)
  int2 tex_size = textureSize(object_id_tx, 0);
  if (radius <= 1e-8f || tex_size.x <= 0 || tex_size.y <= 0) {
    result = ref_normal;
    return;
  }

  int2 center_texel = clamp(int2(gl_FragCoord.xy), int2(0), tex_size - int2(1));
  float2 center_uv = bevel_texel_to_uv(center_texel, tex_size);
  uint center_id = bevel_object_id_fetch(center_texel);
  uint self_id = (center_id != 0u) ? center_id : (drw_resource_id() & 0xFFFFu);

  float pixel_radius = bevel_projected_radius_pixels(radius, tex_size);
  if (pixel_radius <= 0.75f) {
    result = ref_normal;
    return;
  }

  float center_depth = bevel_depth_fetch(center_uv);
  if (center_depth >= 1.0f) {
    result = ref_normal;
    return;
  }

  const int max_sample_count = 64;
  const float golden_angle = 2.39996322973f;
  int sample_count = clamp(max(1, int(samples + 0.5f)) * 8, 16, max_sample_count);
  float2 texel_size = 1.0f / float2(tex_size);
  float object_seed = float(self_id & 255u) * 0.06711056f;

  float3 accum = base_normal;
  float accum_weight = 1.0f;

  for (int sample_index = 0; sample_index < max_sample_count; sample_index++) {
    if (sample_index >= sample_count) {
      break;
    }

    float sample_id = float(sample_index);
    float angle_jitter = bevel_spatial_noise(11.0f + sample_id * 2.0f, object_seed);
    float radius_jitter = bevel_spatial_noise(29.0f + sample_id * 2.0f, object_seed);
    float sample_xi = clamp((sample_id + 0.5f + radius_jitter * 0.75f) / float(sample_count),
                            1e-4f,
                            1.0f - 1e-4f);
    float disk_radius = bevel_cubic_sample_radius(radius, sample_xi);
    float projected_ratio = disk_radius / max(radius, 1e-8f);
    float angle = (sample_id + angle_jitter) * golden_angle;
    float2 direction = float2(cos(angle), sin(angle));
    float2 uv_offset = direction * (pixel_radius * projected_ratio) * texel_size;

    bevel_accumulate_sample(self_id,
                            tex_size,
                            center_uv + uv_offset,
                            radius,
                            disk_radius,
                            g_data.P,
                            base_normal,
                            accum,
                            accum_weight);
  }

  if (accum_weight > 0.0f) {
    float3 bevel_normal = bevel_normalize_or(accum / accum_weight, base_normal);
    result = bevel_normalize_or(ref_normal + (bevel_normal - base_normal), ref_normal);
    return;
  }
#endif

  result = ref_normal;
}
