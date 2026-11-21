/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

struct ShadowCasterRecord {
  /* Draw instance ID, shadow blocker set, packed UV offset, packed Alpha image offset. */
  uvec4 identity;
  vec4 uv_x;
  vec4 uv_y;
};

/* Indexed by instanceCustomIndex. The Draw instance ID is shared across material batches. */
layout(scalar, binding = 10) readonly buffer ShadowCasters {
  ShadowCasterRecord shadow_casters[];
};
layout(scalar, binding = 11) readonly buffer ShadowUVs {
  vec2 shadow_uvs[];
};
layout(scalar, binding = 12) readonly buffer ShadowAlphaImages {
  uint shadow_alpha_images[];
};

const uint SHADOW_UNKNOWN_RECEIVER_ID = 0xFFFFFFFFu;
const vec3 SHADOW_LUMA = vec3(0.2126f, 0.7152f, 0.0722f);

bool shadow_link_is_restricted()
{
  return any(notEqual(uniforms.shadow_options.xy, uvec2(0xFFFFFFFFu)));
}

bool shadow_needs_hit_details()
{
  return uniforms.shadow_options.z != 0u || uniforms.shadow_options.w != 0u ||
         shadow_link_is_restricted();
}

bool shadow_link_includes(uint user_id)
{
  uint set_index = shadow_casters[user_id].identity.y;
  return set_index < 64u &&
         (uniforms.shadow_options[set_index >> 5u] & (1u << (set_index & 31u))) != 0u;
}

int shadow_alpha_address(int pixel, int size, int extension_mode)
{
  if (extension_mode == 1 || extension_mode == 2) {
    return clamp(pixel, 0, size - 1);
  }
  const bool mirror = extension_mode == 3;
  const int period = mirror ? size * 2 : size;
  const int wrapped = (pixel % period + period) % period;
  return mirror && wrapped >= size ? period - 1 - wrapped : wrapped;
}

float shadow_alpha_texel(uint offset, ivec2 size, ivec2 pixel, int extension_mode)
{
  if (extension_mode == 2 &&
      (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, size))))
  {
    return 0.0f;
  }
  pixel = ivec2(shadow_alpha_address(pixel.x, size.x, extension_mode),
                shadow_alpha_address(pixel.y, size.y, extension_mode));
  const uint index = uint(pixel.y * size.x + pixel.x);
  return unpackUnorm2x16(shadow_alpha_images[offset + 2u + index / 2u])[index & 1u];
}

float shadow_alpha_sample(ShadowCasterRecord caster, uint primitive, vec2 barycentric)
{
  const uint corner = caster.identity.z + primitive * 3u;
  const vec2 uv = shadow_uvs[corner] * (1.0f - barycentric.x - barycentric.y) +
                  shadow_uvs[corner + 1u] * barycentric.x +
                  shadow_uvs[corner + 2u] * barycentric.y;
  const vec2 mapped = vec2(dot(caster.uv_x.xyz, vec3(uv, 1.0f)),
                           dot(caster.uv_y.xyz, vec3(uv, 1.0f)));
  const uint offset = caster.identity.w;
  const ivec2 size = ivec2(shadow_alpha_images[offset], shadow_alpha_images[offset + 1u]);
  const int extension_mode = int(caster.uv_x.w);
  const vec2 texel = mapped * vec2(size);
  if (caster.uv_y.w != 0.0f) {
    return shadow_alpha_texel(offset, size, ivec2(floor(texel)), extension_mode);
  }
  const ivec2 base = ivec2(floor(texel - 0.5f));
  const vec2 weight = fract(texel - 0.5f);
  const float a = shadow_alpha_texel(offset, size, base, extension_mode);
  const float b = shadow_alpha_texel(offset, size, base + ivec2(1, 0), extension_mode);
  const float c = shadow_alpha_texel(offset, size, base + ivec2(0, 1), extension_mode);
  const float d = shadow_alpha_texel(offset, size, base + ivec2(1, 1), extension_mode);
  return mix(mix(a, b, weight.x), mix(c, d, weight.x), weight.y);
}

/* RGB is visibility. A is the share of occlusion caused by the receiver's own instance. */
vec4 hardware_shadow_detailed_visibility(accelerationStructureEXT scene_as,
                                         vec3 origin,
                                         vec3 direction,
                                         float tmin,
                                         float tmax,
                                         float transparency,
                                         float color_intensity,
                                         uint receiver_id)
{
  const bool restricted = shadow_link_is_restricted();
  const bool textured = uniforms.shadow_options.w != 0u;
  const uint flags = restricted ? gl_RayFlagsNoOpaqueEXT :
                     textured ? gl_RayFlagsNoneEXT : gl_RayFlagsOpaqueEXT;
  vec3 visibility = vec3(1.0f);
  float self_shadow = 0.0f;
  bool transmitted = false;

  for (int layer = 0; layer < 8 && tmin < tmax; layer++) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene_as, flags, HWRT_SHADOW_CASTER_MASK, origin, tmin, direction, tmax);
    int remaining = 4096;
    while (remaining-- > 0 && rayQueryProceedEXT(query)) {
      const uint candidate_id = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false));
      if (restricted && !shadow_link_includes(candidate_id)) {
        continue;
      }
      const ShadowCasterRecord caster = shadow_casters[candidate_id];
      if (caster.identity.z != 0u &&
          shadow_alpha_sample(caster,
                              uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, false)),
                              rayQueryGetIntersectionBarycentricsEXT(query, false)) <= 1.0e-5f)
      {
        continue;
      }
      rayQueryConfirmIntersectionEXT(query);
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
      break;
    }

    const uint user_id = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, true));
    tmin = shadow_ray_after_hit(rayQueryGetIntersectionTEXT(query, true));
    const HardwareMaterialProxy proxy = material_proxies[user_id];
    const uint proxy_flags = uint(proxy.ior_closure_type.w);
    if ((proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) != 0u) {
      continue;
    }

    const ShadowCasterRecord caster = shadow_casters[user_id];
    vec3 transmission = vec3(0.0f);
    if (caster.identity.z != 0u) {
      const float opacity = shadow_alpha_sample(
          caster,
          uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, true)),
          rayQueryGetIntersectionBarycentricsEXT(query, true));
      transmission = vec3(1.0f - opacity);
      if (transparency > 0.0f && uint(proxy.ior_closure_type.z) == HWRT_CLOSURE_REFRACTION) {
        transmission += opacity * clamp(proxy.transmission_color_roughness.rgb, 0.0f, 1.0f) *
                        transparency;
      }
    }
    else if (transparency > 0.0f) {
      if (uint(proxy.ior_closure_type.z) == HWRT_CLOSURE_REFRACTION) {
        transmission = clamp(proxy.transmission_color_roughness.rgb, 0.0f, 1.0f);
      }
      else if ((proxy_flags & HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT) != 0u) {
        transmission = vec3(clamp(1.0f - proxy.packed_thickness.y, 0.0f, 1.0f));
      }
      if (!transmitted) {
        transmission *= transparency;
        transmitted = true;
      }
    }
    const vec3 next_visibility = visibility * transmission;
    if (receiver_id != SHADOW_UNKNOWN_RECEIVER_ID && caster.identity.x == receiver_id) {
      self_shadow += dot(visibility - next_visibility, SHADOW_LUMA);
    }
    visibility = next_visibility;
    if (max(visibility.r, max(visibility.g, visibility.b)) < 1.0e-3f) {
      break;
    }
  }
  return vec4(hardware_shadow_apply_color_transmission(visibility, color_intensity), self_shadow);
}
