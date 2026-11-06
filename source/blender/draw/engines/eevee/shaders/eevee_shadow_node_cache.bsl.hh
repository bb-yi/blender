/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "eevee_light_data.bsl.hh"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_shadow_tracing.bsl.hh"

namespace eevee::shadow::node_cache {

struct Surface {
  [[sampler(0)]] sampler2DDepth node_depth_tx;
  [[sampler(1)]] sampler2D node_normal_tx;
  [[sampler(3)]] usampler2D node_id_tx;
  [[push_constant]] int cache_layer;
  [[push_constant]] int cache_quality;
  [[resource_table]] srt_t<draw::View> views;
  [[resource_table]] srt_t<LightRenderData> lights;
  [[shared]] int selected_light_index;

  int2 extent() const
  {
    return textureSize(node_depth_tx, 0);
  }

  float3 view_position(int2 pixel) const
  {
    [[resource_table]] const draw::View &v = views;
    const ViewMatrices view = v.get(0);
    float depth = texelFetch(node_depth_tx, pixel, 0).r;
    float2 ndc = (float2(pixel) + 0.5f) / float2(extent()) * 2.0f - 1.0f;
    /* Preserve the precision of reversed depth. Forming 1-depth before inverse projection
     * loses the low bits needed to reconstruct a receiver plane with a small near clip. */
    if (view.is_perspective()) {
      float z = -view.winmat[3][2] / (view.winmat[2][2] + 1.0f - 2.0f * depth);
      float2 xy = (ndc + view.winmat[2].xy) * (-z) / float2(view.winmat[0][0], view.winmat[1][1]);
      return float3(xy, z);
    }
    return float3((ndc - view.winmat[3].xy) / float2(view.winmat[0][0], view.winmat[1][1]),
                  (1.0f - view.winmat[3][2]) / view.winmat[2][2] -
                      2.0f * depth / view.winmat[2][2]);
  }

  float3 position(int2 pixel) const
  {
    [[resource_table]] const draw::View &v = views;
    return v.get(0).point_view_to_world(view_position(pixel));
  }

  float3 normal(int2 pixel) const
  {
    return safe_normalize(texelFetch(node_normal_tx, pixel, 0).xyz * 2.0f - 1.0f);
  }

  uint id(int2 pixel) const
  {
    return texelFetch(node_id_tx, pixel, 0).r;
  }

  bool valid(int2 pixel) const
  {
    return all(greaterThanEqual(pixel, int2(0))) && all(lessThan(pixel, extent())) &&
           texelFetch(node_depth_tx, pixel, 0).r > 0.0f;
  }

  float3 geometric_normal(int2 pixel, float3 P, float3 N) const
  {
    [[resource_table]] const draw::View &v = views;
    float3 center = view_position(pixel);
    int span = 16;
    int2 lo = max(pixel - span, int2(0)), hi = min(pixel + span, extent() - 1);
    int2 left = int2(lo.x, pixel.y), right = int2(hi.x, pixel.y);
    int2 bottom = int2(pixel.x, lo.y), top = int2(pixel.x, hi.y);
    /* Use a wider baseline only on a locally planar receiver. Adjacent depth samples can
     * have too few significant bits when the near clip is small compared with view depth. */
    bool planar = !(id(left) != id(pixel) || id(right) != id(pixel) || id(bottom) != id(pixel) ||
                    id(top) != id(pixel) || dot(N, normal(left)) < 0.9995f ||
                    dot(N, normal(right)) < 0.9995f || dot(N, normal(bottom)) < 0.9995f ||
                    dot(N, normal(top)) < 0.9995f);
    if (!planar) {
      lo = max(pixel - 1, int2(0));
      hi = min(pixel + 1, extent() - 1);
    }
    float3 dx0 = center - view_position(int2(lo.x, pixel.y));
    float3 dx1 = view_position(int2(hi.x, pixel.y)) - center;
    float3 dy0 = center - view_position(int2(pixel.x, lo.y));
    float3 dy1 = view_position(int2(pixel.x, hi.y)) - center;
    if (planar) {
      float3 vN = v.get(0).normal_world_to_view(N);
      if (abs(dot(vN, dx0)) > length(dx0) * 0.01f || abs(dot(vN, dx1)) > length(dx1) * 0.01f ||
          abs(dot(vN, dy0)) > length(dy0) * 0.01f || abs(dot(vN, dy1)) > length(dy1) * 0.01f)
      {
        /* Parallel faces can share an object ID and shading normal. A depth discontinuity
         * is not a receiver plane; leave that pixel to direct evaluation. */
        return float3(0.0f);
      }
    }
    float lx0 = length_squared(dx0), lx1 = length_squared(dx1);
    float ly0 = length_squared(dy0), ly1 = length_squared(dy1);
    float3 dx = lx0 == 0.0f                      ? dx1 :
                lx1 == 0.0f                      ? dx0 :
                (planar ? lx0 > lx1 : lx0 < lx1) ? dx0 :
                                                   dx1;
    float3 dy = ly0 == 0.0f                      ? dy1 :
                ly1 == 0.0f                      ? dy0 :
                (planar ? ly0 > ly1 : ly0 < ly1) ? dy0 :
                                                   dy1;
    float3 Ng = safe_normalize(v.get(0).normal_view_to_world(cross(dx, dy)));
    return dot(Ng, N) < 0.0f ? -Ng : Ng;
  }

  float3 extended_position(int2 pixel) const
  {
    int2 base = clamp(pixel, int2(0), extent() - 1);
    float3 P = position(base);
    if (all(equal(pixel, base))) {
      return P;
    }
    [[resource_table]] const draw::View &v = views;
    const ViewMatrices view = v.get(0);
    float3 Ng = geometric_normal(base, P, normal(base));
    float2 ndc = (float2(pixel) + 0.5f) / float2(extent()) * 2.0f - 1.0f;
    float3 origin, direction;
    if (view.is_perspective()) {
      origin = view.viewinv[3].xyz;
      float2 xy = (ndc + view.winmat[2].xy) / float2(view.winmat[0][0], view.winmat[1][1]);
      direction = view.normal_view_to_world(float3(xy, -1.0f));
    }
    else {
      float2 xy = (ndc - view.winmat[3].xy) / float2(view.winmat[0][0], view.winmat[1][1]);
      origin = view.point_view_to_world(float3(xy, 0.0f));
      direction = -view.viewinv[2].xyz;
    }
    float denominator = dot(Ng, direction);
    return abs(denominator) > 1e-8f ? origin + direction * (dot(Ng, P - origin) / denominator) : P;
  }

  int select_light_index() const
  {
    [[resource_table]] const LightRenderData &lrd = lights;
    int suns = int(lrd.light_cull_buf.sun_lights_len);
    int selected = 0;
    for (int i = 0; i < suns + int(lrd.light_cull_buf.visible_count); i++) {
      int index = i < suns ? int(lrd.light_cull_buf.local_lights_len) + i : i - suns;
      LightData light = lrd.light_buf[index];
      if (i < suns && i < WORLD_SUN_MAX) {
        LightData world_sun = lrd.sunlight_buf[i];
        if (reduce_max(abs(world_sun.color)) > 0.0f &&
            length(light.color - world_sun.color) < 1e-4f &&
            dot(light.sun().direction, world_sun.object_to_world.z_axis()) > 0.9999f)
        {
          continue;
        }
      }
      if (light.tilemap_index == LIGHT_NO_SHADOW || reduce_max(abs(light.color)) == 0.0f) {
        continue;
      }
      bool soft = is_sun_light(light.type) ?
                      light.sun().shadow_angle > 0.0f :
                  is_area_light(light.type) ?
                      light.area().shadow_scale > 0.0f :
                      (light.type == LIGHT_OMNI_SPHERE || light.type == LIGHT_OMNI_DISK) &&
                          light.local().local.shadow_radius > 0.0f;
      if (soft && selected++ == cache_layer) {
        return index;
      }
    }
    return -1;
  }

  float plane_weight(int2 a, int2 b, float3 P, float3 Ng, float pixel_size) const
  {
    if (!valid(b) || id(a) != id(b)) {
      return 0.0f;
    }
    float normal_weight = pow(saturate(dot(normal(a), normal(b))), 32.0f);
    float plane_error = abs(dot(Ng, position(b) - P));
    return normal_weight * exp2(-plane_error / max(pixel_size * 0.5f, 1e-16f));
  }
};

struct Estimate {
  [[image(0, write, SFLOAT_32_32)]] image2D node_stats_img;
};

[[compute, local_size(8, 8)]] [[texture_atomic]]
void estimate_main([[resource_table]] Surface &surface,
                   [[resource_table]] Estimate &out_data,
                   [[resource_table]] ShadowRenderData &srd,
                   [[global_invocation_id]] const uint3 global_id,
                   [[local_invocation_index]] const uint local_index)
{
  if (local_index == 0u) {
    surface.selected_light_index = surface.select_light_index();
  }
  barrier();
  if (surface.selected_light_index < 0) {
    return;
  }
  int2 pixel = int2(global_id.xy);
  if (any(greaterThanEqual(pixel, surface.extent()))) {
    return;
  }
  imageStore(out_data.node_stats_img, pixel, float4(-1.0f));
  int index = surface.selected_light_index;
  if (index < 0 || !surface.valid(pixel)) {
    return;
  }
  [[resource_table]] const LightRenderData &lrd = surface.lights;
  LightData light = lrd.light_buf[index];
  if (!(is_sun_light(light.type) || light.type == LIGHT_OMNI_SPHERE ||
        light.type == LIGHT_OMNI_DISK || is_area_light(light.type)))
  {
    return;
  }
  float3 P = surface.position(pixel), N = surface.normal(pixel);
  float3 Ng = surface.geometric_normal(pixel, P, N);
  if (length_squared(Ng) < 0.5f) {
    return;
  }
  float4 stats = shadow_node_pcss_core(srd,
                                       light,
                                       is_sun_light(light.type),
                                       P,
                                       Ng,
                                       N,
                                       0.0f,
                                       0.0f,
                                       surface.cache_quality,
                                       false,
                                       surface.id(pixel),
                                       -1.0f,
                                       true,
                                       float2(1.0f, 0.0f),
                                       8);
  imageStore(out_data.node_stats_img, pixel, stats);
}

struct Resolve {
  [[compilation_constant]] bool node_cache_classify;
  [[sampler(12)]] sampler2D node_stats_tx;
  [[image(0, write, SFLOAT_16_16_16_16)]] image2D node_raw_img;
};

[[compute, local_size(8, 8)]] [[texture_atomic]]
void resolve_main([[resource_table]] Surface &surface,
                  [[resource_table]] Resolve &data,
                  [[resource_table]] ShadowRenderData &srd,
                  [[resource_table]] const UtilityTexture &utility,
                  [[global_invocation_id]] const uint3 global_id,
                  [[local_invocation_index]] const uint local_index)
{
  if (local_index == 0u) {
    surface.selected_light_index = surface.select_light_index();
  }
  barrier();
  if (surface.selected_light_index < 0) {
    return;
  }
  int2 output_pixel = int2(global_id.xy);
  if (any(greaterThanEqual(output_pixel, surface.extent() + 4))) {
    return;
  }
  int2 pixel = output_pixel - 2;
  int2 base = clamp(pixel, int2(0), surface.extent() - 1);
  imageStore(data.node_raw_img, output_pixel, float4(-1.0f));
  float2 stats = texelFetch(data.node_stats_tx, base, 0).xy;
  int index = surface.selected_light_index;
  if (stats.x < 0.0f || index < 0) {
    return;
  }
  [[resource_table]] const LightRenderData &lrd = surface.lights;
  [[resource_table]] const draw::View &views = surface.views;
  [[resource_table]] const Uniform &uni = srd.uniforms;
  const ViewMatrices view = views.get(0);
  LightData light = lrd.light_buf[index];
  float3 P = surface.extended_position(pixel), N = surface.normal(base);
  float3 Ng = surface.geometric_normal(base, surface.position(base), N);
  float pixel_size = shadow_screen_pixel_size(
      uni.uniform_buf.shadow.film_pixel_radius, view.is_perspective(), view.z_distance(P));
  float radius_px = stats.x / max(2.0f * pixel_size, 1e-16f);
  float radius = stats.x;
  if (radius_px > 1.0f && stats.y > 0.0f) {
    int stride = clamp(int(sqrt(radius_px)), 1, 8);
    float weighted_radius = radius * max(stats.y, 0.125f) * 2.0f;
    float weight_sum = max(stats.y, 0.125f) * 2.0f;
    for (int y = -1; y <= 1; y++) {
      for (int x = -1; x <= 1; x++) {
        if (x == 0 && y == 0) {
          continue;
        }
        int2 neighbor = pixel + int2(x, y) * stride;
        if (!surface.valid(neighbor)) {
          continue;
        }
        float2 value = texelFetch(data.node_stats_tx, neighbor, 0).xy;
        if (value.x <= 0.0f || value.y <= 0.0f) {
          continue;
        }
        float weight = surface.plane_weight(base, neighbor, P, Ng, pixel_size) * value.y;
        /* Preserve contact hardening and separate depth layers within the same receiver. */
        weight *= exp2(-4.0f * abs(value.x - radius) / max(radius, 1e-16f));
        weighted_radius += value.x * weight;
        weight_sum += weight;
      }
    }
    radius = weighted_radius / weight_sum;
  }
  float2 noise_pixel = float2(pixel & int2(UTIL_TEX_SIZE - 1));
  float angle = 6.28318530718f * utility.fetch(noise_pixel, UTIL_BLUE_NOISE_LAYER).r;
  float4 mask = shadow_node_pcss_core(srd,
                                      light,
                                      is_sun_light(light.type),
                                      P,
                                      Ng,
                                      N,
                                      0.0f,
                                      0.0f,
                                      surface.cache_quality,
                                     data.node_cache_classify,
                                      surface.id(base),
                                      radius,
                                      false,
                                      float2(cos(angle), sin(angle)),
                                     2);
  /* Unknown caster probability remains recoverable as 1 - sum(mask.rgb). */
  mask.w = min(radius_px, 1024.0f);
  imageStore(data.node_raw_img, output_pixel, mask);
}

struct Filter {
  [[sampler(12)]] sampler2D node_raw_tx;
  [[image(0, write, SFLOAT_16_16_16_16)]] image2DArray node_cache_img;
  [[shared]] float4 masks[144];
  [[shared]] float3 positions[144];
  [[shared]] float3 normals[144];
  [[shared]] uint ids[144];
};

[[compute, local_size(8, 8)]]
void filter_main([[resource_table]] Surface &surface,
                 [[resource_table]] Filter &data,
                 [[resource_table]] const Uniform &uni,
                 [[global_invocation_id]] const uint3 global_id,
                 [[local_invocation_index]] const uint local_index,
                 [[work_group_id]] const uint3 group_id)
{
  if (local_index == 0u) {
    surface.selected_light_index = surface.select_light_index();
  }
  barrier();
  if (surface.selected_light_index < 0) {
    if (all(equal(global_id.xy, uint2(0)))) {
      imageStore(data.node_cache_img,
                 int3(surface.extent().x, 0, surface.cache_layer),
                 float4(0.0f, 0.0f, -1.0f, float(surface.cache_quality)));
    }
    return;
  }
  int2 origin = int2(group_id.xy) * 8 - 2;
  for (int i = int(local_index); i < 144; i += 64) {
    int2 pixel = clamp(origin + int2(i % 12, i / 12), int2(-2), surface.extent() + 1);
    int2 base = clamp(pixel, int2(0), surface.extent() - 1);
    data.masks[i] = texelFetch(data.node_raw_tx, pixel + 2, 0);
    data.positions[i] = surface.extended_position(pixel);
    data.normals[i] = surface.normal(base);
    data.ids[i] = surface.id(base);
  }
  barrier();
  int2 pixel = int2(global_id.xy);
  if (any(greaterThanEqual(pixel, surface.extent()))) {
    return;
  }
  if (all(equal(pixel, int2(0)))) {
    /* One extra column holds the view-local light-index mapping without another binding.
     * Byte components remain exact in half-float storage even for large light arrays. */
    int index = surface.selected_light_index;
    imageStore(data.node_cache_img,
               int3(surface.extent().x, 0, surface.cache_layer),
               float4(float(index & 255),
                      float(index >> 8),
                      index >= 0 ? 1.0f : -1.0f,
                      float(surface.cache_quality)));
  }
  int2 local_pixel = pixel - origin;
  int center = local_pixel.y * 12 + local_pixel.x;
  float4 value = data.masks[center];
  if (value.x < 0.0f || value.w < 1.0f) {
    imageStore(data.node_cache_img, int3(pixel, surface.cache_layer), value);
    return;
  }
  [[resource_table]] const draw::View &views = surface.views;
  const ViewMatrices view = views.get(0);
  float3 P = data.positions[center], N = data.normals[center];
  float3 Ng = surface.geometric_normal(pixel, P, N);
  float pixel_size = shadow_screen_pixel_size(
      uni.uniform_buf.shadow.film_pixel_radius, view.is_perspective(), view.z_distance(P));
  float3 total = float3(0.0f);
  float weight_sum = 0.0f;
  float sigma = min(1.25f, max(0.5f, value.w * 0.125f));
  for (int y = -2; y <= 2; y++) {
    for (int x = -2; x <= 2; x++) {
      int index = center + y * 12 + x;
      float4 neighbor = data.masks[index];
      if (neighbor.x < 0.0f || data.ids[index] != data.ids[center]) {
        continue;
      }
      float weight = exp2(-float(x * x + y * y) / (2.0f * sigma * sigma));
      weight *= pow(saturate(dot(N, data.normals[index])), 32.0f);
      weight *= exp2(-abs(dot(Ng, data.positions[index] - P)) / max(pixel_size * 0.5f, 1e-16f));
      /* Do not spread a broad penumbra over a contact edge or a different blocker layer. */
      weight *= exp2(-4.0f * abs(neighbor.w - value.w) / max(value.w, 1.0f));
      total += neighbor.xyz * weight;
      weight_sum += weight;
    }
  }
  value.xyz = total / max(weight_sum, 1e-8f);
  imageStore(data.node_cache_img, int3(pixel, surface.cache_layer), value);
}

PipelineCompute estimate(estimate_main);
PipelineCompute resolve(resolve_main, Resolve{.node_cache_classify = false});
PipelineCompute resolve_classified(resolve_main, Resolve{.node_cache_classify = true});
PipelineCompute reconstruct(filter_main);

}  // namespace eevee::shadow::node_cache
