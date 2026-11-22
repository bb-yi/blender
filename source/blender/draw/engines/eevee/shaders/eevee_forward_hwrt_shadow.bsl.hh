/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_light_lib.bsl.hh"
#include "gpu_shader_math_vector_safe.bsl.hh"

namespace eevee {

struct ForwardShadowCaster {
  uint4 identity;
  float4 uv_x;
  float4 uv_y;
};

struct ForwardShadowProxy {
  float4 reflection_color_roughness;
  float4 transmission_color_roughness;
  float4 ior_closure_type;
  float4 packed_thickness;
};

float3 forward_shadow_origin(float3 position, float3 normal)
{
  const float position_scale = max(max(abs(position.x), abs(position.y)), abs(position.z));
  const float bias = max(position_scale * 1.25e-6f, 1.0e-7f);
  return position + normal * bias;
}

#if defined(MAT_FORWARD_HWRT_SHADOW_REUSE) && defined(GPU_FRAGMENT_SHADER)
const uint forward_shadow_cache_size = 8u;
uint forward_shadow_cache_valid;
int forward_shadow_cache_layer[8];
float3 forward_shadow_cache_rgb[8];

void forward_shadow_cache_store(int layer, float3 visibility)
{
  const uint slot = uint(layer) % forward_shadow_cache_size;
  forward_shadow_cache_layer[slot] = layer;
  forward_shadow_cache_rgb[slot] = visibility;
  forward_shadow_cache_valid |= 1u << slot;
}

float3 forward_shadow_cache_lookup(int layer)
{
  const uint slot = uint(layer) % forward_shadow_cache_size;
  if ((forward_shadow_cache_valid & (1u << slot)) != 0u &&
      forward_shadow_cache_layer[slot] == layer)
  {
    return forward_shadow_cache_rgb[slot];
  }
  return float3(-1.0f);
}
#endif

struct HardwareTriangleScene {
  [[acceleration_structure(0)]] const accelerationStructure hardware_scene_as;
  [[storage(20, read)]] const ForwardShadowCaster (&hardware_scene_instances)[];
  [[storage(21, read)]] const float2 (&hardware_scene_uvs)[];
  [[storage(22, read)]] const uint (&hardware_scene_alpha)[];

  float alpha_texel(uint offset, int2 size, int2 pixel, int extension_mode) const
  {
    if (extension_mode == 2 &&
        (any(lessThan(pixel, int2(0))) || any(greaterThanEqual(pixel, size))))
    {
      return 0.0f;
    }
    if (extension_mode == 1 || extension_mode == 2) {
      pixel = clamp(pixel, int2(0), size - 1);
    }
    else {
      const int2 period = size * (extension_mode == 3 ? 2 : 1);
      pixel = (pixel % period + period) % period;
      if (extension_mode == 3) {
        pixel = min(pixel, period - 1 - pixel);
      }
    }
    const uint index = uint(pixel.y * size.x + pixel.x);
    return unpackUnorm2x16(this->hardware_scene_alpha[offset + 2u + index / 2u])[index & 1u];
  }

  float opacity(ForwardShadowCaster caster, uint primitive, float2 barycentric) const
  {
    const uint corner = caster.identity.z + primitive * 3u;
    const float2 uv = this->hardware_scene_uvs[corner] * (1.0f - barycentric.x - barycentric.y) +
                      this->hardware_scene_uvs[corner + 1u] * barycentric.x +
                      this->hardware_scene_uvs[corner + 2u] * barycentric.y;
    const float2 mapped = float2(dot(caster.uv_x.xyz, float3(uv, 1.0f)),
                                  dot(caster.uv_y.xyz, float3(uv, 1.0f)));
    const uint offset = caster.identity.w;
    const int2 size = int2(this->hardware_scene_alpha[offset],
                           this->hardware_scene_alpha[offset + 1u]);
    const int extension_mode = int(caster.uv_x.w);
    const float2 texel = mapped * float2(size);
    if ((uint(caster.uv_y.w) & 1u) != 0u) {
      return this->alpha_texel(offset, size, int2(floor(texel)), extension_mode);
    }
    const int2 base = int2(floor(texel - 0.5f));
    const float2 weight = fract(texel - 0.5f);
    const float a = this->alpha_texel(offset, size, base, extension_mode);
    const float b = this->alpha_texel(offset, size, base + int2(1, 0), extension_mode);
    const float c = this->alpha_texel(offset, size, base + int2(0, 1), extension_mode);
    const float d = this->alpha_texel(offset, size, base + int2(1, 1), extension_mode);
    return mix(mix(a, b, weight.x), mix(c, d, weight.x), weight.y);
  }
};

struct ForwardHardwareShadow {
  [[resource_table]] HardwareTriangleScene scene;
  [[storage(23, read)]] const ForwardShadowProxy (&forward_shadow_proxies)[];
  /* Ready, texture Alpha, transmission strength, color intensity. */
  [[push_constant]] float4 forward_shadow_options;
  [[push_constant]] int forward_shadow_samples;

  /* RGB is visibility. Alpha is the occlusion attributed to the receiver's own instance. */
  float4 visibility_ray(float3 origin,
                        float3 L,
                        float tmax,
                        uint2 membership,
                        uint receiver_id) const
  {
    const bool linked = any(notEqual(membership, uint2(0xFFFFFFFFu)));
    const bool textured = this->forward_shadow_options.y > 0.0f;
    const float transparency = this->forward_shadow_options.z;
    const uint flags = linked ? gpu_RayFlagsNoOpaque :
                       textured ? gpu_RayFlagsNone : gpu_RayFlagsOpaque;
    float tmin = 0.0f;
    float3 visibility = float3(1.0f);
    float self_shadow = 0.0f;
    const float3 luma_weights = float3(0.2126f, 0.7152f, 0.0722f);
    bool transmitted = false;
    for (int layer = 0; layer < 8 && tmin < tmax; layer++) {
      rayQuery query;
      rayQueryInitialize(query, this->scene.hardware_scene_as, flags, 0x01u, origin, tmin, L, tmax);
      int remaining = 4096;
      while (remaining-- > 0 && rayQueryProceed(query)) {
        const uint candidate = rayQueryGetIntersectionInstanceCustomIndex(query, false);
        const ForwardShadowCaster caster = this->scene.hardware_scene_instances[candidate];
        const uint blocker_set = caster.identity.y;
        if (linked && (blocker_set >= 64u ||
                       (membership[blocker_set >> 5u] & (1u << (blocker_set & 31u))) == 0u))
        {
          continue;
        }
        if (caster.identity.z != 0u && caster.uv_y.w < 2.0f &&
            this->scene.opacity(caster,
                          rayQueryGetIntersectionPrimitiveIndex(query, false),
                          rayQueryGetIntersectionBarycentrics(query, false)) <= 1.0e-5f)
        {
          continue;
        }
        rayQueryConfirmIntersection(query);
      }
      if (rayQueryGetIntersectionType(query, true) != gpu_RayQueryCommittedIntersectionTriangle) {
        break;
      }
      const uint hit = rayQueryGetIntersectionInstanceCustomIndex(query, true);
      const float distance = rayQueryGetIntersectionT(query, true);
      tmin = uintBitsToFloat(max(floatBitsToUint(abs(distance)) + 1u, 0x00800000u));
      const ForwardShadowCaster caster = this->scene.hardware_scene_instances[hit];
      const ForwardShadowProxy proxy = this->forward_shadow_proxies[hit];
      const uint proxy_flags = uint(proxy.ior_closure_type.w);
      if ((proxy_flags & (1u << 7u)) != 0u) {
        continue;
      }
      float3 T = float3(0.0f);
      const bool refracts = uint(proxy.ior_closure_type.z) == 12u;
      if (caster.identity.z != 0u && caster.uv_y.w < 2.0f) {
        const float a = this->scene.opacity(caster,
                                       rayQueryGetIntersectionPrimitiveIndex(query, true),
                                       rayQueryGetIntersectionBarycentrics(query, true));
        T = float3(1.0f - a);
        if (refracts) {
          T += a * transparency * saturate(proxy.transmission_color_roughness.rgb);
        }
      }
      else if (transparency > 0.0f) {
        if (refracts) {
          T = saturate(proxy.transmission_color_roughness.rgb);
        }
        else if ((proxy_flags & (1u << 2u)) != 0u) {
          T = float3(saturate(1.0f - proxy.packed_thickness.y));
        }
        if (!transmitted) {
          T *= transparency;
          transmitted = true;
        }
      }
      const float3 next_visibility = visibility * T;
      if (receiver_id != 0xFFFFFFFFu && caster.identity.x == receiver_id) {
        self_shadow += dot(visibility - next_visibility, luma_weights);
      }
      visibility = next_visibility;
      if (max(visibility.r, max(visibility.g, visibility.b)) < 1.0e-3f) {
        break;
      }
    }
    const float luma = dot(visibility, luma_weights);
    return float4(mix(float3(luma), visibility, this->forward_shadow_options.w), self_shadow);
  }

  float4 evaluate_classified(LightData light,
                             LightVector lv,
                             bool is_directional,
                             float3 P,
                             float3 Ng,
                             uint receiver_id) const
  {
    const float3 origin = forward_shadow_origin(P, Ng);
    const float radius = max(light.hwrt_shadow_size, 0.0f);
    const int samples = radius > 1.0e-6f ? this->forward_shadow_samples : 1;
    float4 visibility = float4(0.0f);
    for (int i = 0; i < samples; i++) {
      const float angle = float(i) * 2.39996323f;
      const float r = sqrt((float(i) + 0.5f) / float(samples)) * (samples > 1 ? radius : 0.0f);
      const float3 offset = r * (cos(angle) * light.x_axis() + sin(angle) * light.y_axis());
      if (is_directional) {
        visibility += this->visibility_ray(origin, normalize(lv.L + offset), 1.0e5f,
                                            light.shadow_set_membership, receiver_id);
      }
      else {
        const float3 delta = light.position() + offset - origin;
        const float distance = length(delta);
        visibility += this->visibility_ray(origin, delta / max(distance, 1.0e-6f), distance,
                                            light.shadow_set_membership, receiver_id);
      }
    }
    return visibility / float(samples);
  }

  float3 evaluate(LightData light, LightVector lv, bool is_directional, float3 P, float3 Ng) const
  {
    return this->evaluate_classified(light, lv, is_directional, P, Ng, 0xFFFFFFFFu).rgb;
  }
};

struct HardwareRaycast {
  [[resource_table]] HardwareTriangleScene scene;
  [[storage(24, read)]] const uint2 (&hardware_raycast_normal_ranges)[];
  [[storage(25, read)]] const float4 (&hardware_raycast_smooth_normals)[];

  void trace(float3 origin,
             float3 direction,
             float max_distance,
             uint source_id,
             bool only_local,
             bool &is_hit,
             bool &self_hit,
             float &hit_distance,
             float3 &hit_position,
             float3 &hit_normal) const
  {
    const float direction_scale = reduce_max(abs(direction));
    if (!(direction_scale > 0.0f) || any(isnan(direction)) || any(isinf(direction)) ||
        any(isnan(origin)) || any(isinf(origin)) || isinf(max_distance))
    {
      return;
    }
    direction = normalize(direction / direction_scale);
    const float tmin = max(reduce_max(abs(origin)) * 4.7683716e-7f, 1.0e-7f);
    if (!(max_distance > tmin)) {
      return;
    }

    const uint flags = only_local ? gpu_RayFlagsNoOpaque : gpu_RayFlagsNone;
    rayQuery query;
    rayQueryInitialize(query, this->scene.hardware_scene_as, flags, 0x02u,
                       origin, tmin, direction, max_distance);
    while (rayQueryProceed(query)) {
      const uint candidate_id = rayQueryGetIntersectionInstanceCustomIndex(query, false);
      const ForwardShadowCaster candidate = this->scene.hardware_scene_instances[candidate_id];
      if (only_local && candidate.identity.x != source_id) {
        continue;
      }
      if (candidate.identity.z != 0u &&
          this->scene.opacity(candidate,
                              rayQueryGetIntersectionPrimitiveIndex(query, false),
                              rayQueryGetIntersectionBarycentrics(query, false)) <= 1.0e-5f)
      {
        continue;
      }
      rayQueryConfirmIntersection(query);
    }
    if (rayQueryGetIntersectionType(query, true) != gpu_RayQueryCommittedIntersectionTriangle) {
      return;
    }

    const uint hit_id = rayQueryGetIntersectionInstanceCustomIndex(query, true);
    const uint primitive = rayQueryGetIntersectionPrimitiveIndex(query, true);
    is_hit = true;
    self_hit = this->scene.hardware_scene_instances[hit_id].identity.x == source_id;
    hit_distance = rayQueryGetIntersectionT(query, true);
    hit_position = origin + direction * hit_distance;

    const uint2 range = this->hardware_raycast_normal_ranges[hit_id];
    if (primitive >= range.y) {
      return;
    }
    const uint base = (range.x + primitive) * 3u;
    const float2 barycentric = rayQueryGetIntersectionBarycentrics(query, true);
    const float4x3 world_to_object = rayQueryGetIntersectionWorldToObject(query, true);
    const float3x3 inverse_basis = float3x3(
        world_to_object[0], world_to_object[1], world_to_object[2]);
    const float3 local_first = safe_normalize(
        inverse_basis * this->hardware_raycast_smooth_normals[base].xyz);
    const float3 local_second = safe_normalize(
        inverse_basis * this->hardware_raycast_smooth_normals[base + 1u].xyz);
    const float3 local_third = safe_normalize(
        inverse_basis * this->hardware_raycast_smooth_normals[base + 2u].xyz);
    const float3 local_normal = local_first * (1.0f - barycentric.x - barycentric.y) +
                                local_second * barycentric.x + local_third * barycentric.y;
    hit_normal = safe_normalize(transpose(inverse_basis) * local_normal);
  }
};

}  // namespace eevee
