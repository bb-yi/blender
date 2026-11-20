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
    return texelFetch(node_normal_tx, pixel, 0).a > 0.5f &&
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
            dot(light.sun.direction, world_sun.object_to_world.z_axis()) > 0.9999f)
        {
          continue;
        }
      }
      if (light.tilemap_index == LIGHT_NO_SHADOW ||
          (light.flags & LIGHT_USE_SHADOW_JITTER) == 0 ||
          reduce_max(abs(light.color)) == 0.0f)
      {
        continue;
      }
      if (selected++ == cache_layer) {
        return index;
      }
    }
    return -1;
  }

};

struct Resolve {
  [[compilation_constant]] bool node_cache_classify;
  [[push_constant]] int history_valid;
  [[sampler(12)]] sampler2DArray node_history_tx;
  [[sampler(13)]] sampler2D node_surface_history_tx;
  [[image(0, write, SFLOAT_16_16_16_16)]] image2DArray node_cache_img;
  [[image(1, write, SFLOAT_32_32)]] image2D node_surface_img;
};

[[compute, local_size(8, 8)]] [[texture_atomic]]
void resolve_main([[resource_table]] Surface &surface,
                  [[resource_table]] Resolve &data,
                  [[resource_table]] ShadowRenderData &srd,
                  [[global_invocation_id]] const uint3 global_id,
                  [[local_invocation_index]] const uint local_index)
{
  if (local_index == 0u) {
    surface.selected_light_index = surface.select_light_index();
  }
  barrier();
  int index = surface.selected_light_index;
  int2 pixel = int2(global_id.xy);
  if (all(equal(pixel, int2(0)))) {
    imageStore(data.node_cache_img,
               int3(surface.extent().x, 0, surface.cache_layer),
               float4(float(index & 255),
                      float(index >> 8),
                      index >= 0 ? 1.0f : -1.0f,
                      float(surface.cache_quality)));
  }
  if (any(greaterThanEqual(pixel, surface.extent()))) {
    return;
  }
  float4 value = float4(-1.0f);
  float4 signature = float4(0.0f);
  if (index >= 0 && surface.valid(pixel)) {
    [[resource_table]] const LightRenderData &lrd = surface.lights;
    LightData light = lrd.light_buf[index];
    float3 P = surface.position(pixel), N = surface.normal(pixel);
    [[resource_table]] const draw::View &views = surface.views;
    const ViewMatrices view = views.get(0);
    signature = float4(view.z_distance(P), float(surface.id(pixel)), 0.0f, 0.0f);
    float3 Ng = surface.geometric_normal(pixel, P, N);
    if (length_squared(Ng) > 0.5f) {
      value = shadow_node_raytrace(srd,
                                   light,
                                   is_sun_light(light.type),
                                   float2(pixel) + 0.5f,
                                   P,
                                   Ng,
                                   N,
                                   0.0f,
                                   0.0f,
                                   surface.cache_quality,
                                   data.node_cache_classify,
                                   surface.id(pixel),
                                   -1.0f);
      value.w = 1.0f;
      /* Accumulate visibility BEFORE material nodes so a ColorRamp sees penumbra gray,
       * including when a jittered shadow map supplies a binary visibility per frame.
       * Sampling resets on view/scene changes. Keep the film pixel fixed through its
       * subpixel jitter sequence: repeated nearest reprojection would diffuse edges. */
      [[resource_table]] const Uniform &uni = srd.uniforms;
      if (data.history_valid != 0) {
        int2 old_pixel = pixel;
        float4 old_surface = texelFetch(data.node_surface_history_tx, old_pixel, 0);
        float4 header = texelFetch(
            data.node_history_tx, int3(surface.extent().x, 0, surface.cache_layer), 0);
        int old_light = int(header.x) | (int(header.y) << 8);
        float pixel_size = shadow_screen_pixel_size(
            uni.uniform_buf.shadow.film_pixel_radius, view.is_perspective(), view.z_distance(P));
        if (old_light == index && header.z > 0.0f && uint(old_surface.y) == surface.id(pixel) &&
            abs(old_surface.x - signature.x) < max(pixel_size * 2.0f, 1e-7f))
        {
          float4 history = texelFetch(
              data.node_history_tx, int3(old_pixel, surface.cache_layer), 0);
          if (history.x >= 0.0f && history.w > 0.0f) {
            /* A bounded history avoids overweighting the first film samples when the
             * material result is itself accumulated, and responds to disocclusion. */
            float count = min(history.w, 31.0f);
            value.xyz = (history.xyz * count + value.xyz) / (count + 1.0f);
            value.w = count + 1.0f;
          }
        }
      }
    }
  }
  if (surface.cache_layer == 0) {
    imageStore(data.node_surface_img, pixel, signature);
  }
  imageStore(data.node_cache_img, int3(pixel, surface.cache_layer), value);
}

PipelineCompute resolve(resolve_main,
                        Resolve{.node_cache_classify = false},
                        ShadowRenderConstants{.shadow_random = true,
                                              .shadow_static_noise = false});
PipelineCompute resolve_classified(resolve_main,
                                   Resolve{.node_cache_classify = true},
                                   ShadowRenderConstants{.shadow_random = true,
                                                         .shadow_static_noise = false});

}  // namespace eevee::shadow::node_cache
