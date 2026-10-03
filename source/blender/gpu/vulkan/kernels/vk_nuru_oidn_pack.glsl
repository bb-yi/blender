/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Nuru: pack HDR radiance (and optional albedo/normal aux) into tightly packed float3 buffers
 * for OpenImageDenoise consumption. Vulkan port of the Metal `eevee_oidn_pack` kernel. */

#version 460
#extension GL_EXT_scalar_block_layout : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(scalar, binding = 0) uniform NuruUniforms {
  uint extent_x;
  uint extent_y;
  uint use_albedo;
  uint use_normal;
} uniforms;

layout(scalar, binding = 2) writeonly buffer B2 { float color_buf[]; };
layout(scalar, binding = 3) writeonly buffer B3 { float albedo_buf[]; };
layout(scalar, binding = 4) writeonly buffer B4 { float normal_buf[]; };

layout(binding = 16) uniform sampler2D input_radiance_tx;
layout(binding = 17) uniform sampler2D albedo_tx;
layout(binding = 18) uniform sampler2D normal_tx;

/* OIDN inputs must be finite and non-negative: a single NaN/Inf texel poisons the whole CNN
 * receptive field around it and shows up as a large blotchy "mush" patch in the filtered
 * radiance. The Metal `eevee_oidn_pack` kernel clamps with `max(value, 0)` (which also kills
 * NaNs in MSL); GLSL `max()` is undefined for NaN, so guard explicitly and cap at half-float
 * max which is far above any meaningful HDR radiance for denoising. */
float oidn_safe_value(float v)
{
  if (isnan(v) || isinf(v)) {
    return 0.0;
  }
  return clamp(v, 0.0, 65504.0);
}

void main()
{
  uvec2 texel = gl_GlobalInvocationID.xy;
  if (texel.x >= uniforms.extent_x || texel.y >= uniforms.extent_y) {
    return;
  }
  uint pixel_index = (texel.y * uniforms.extent_x + texel.x) * 3u;

  vec4 radiance = texelFetch(input_radiance_tx, ivec2(texel), 0);
  color_buf[pixel_index + 0u] = oidn_safe_value(radiance.x);
  color_buf[pixel_index + 1u] = oidn_safe_value(radiance.y);
  color_buf[pixel_index + 2u] = oidn_safe_value(radiance.z);

  if (uniforms.use_albedo != 0u) {
    vec4 albedo = texelFetch(albedo_tx, ivec2(texel), 0);
    albedo_buf[pixel_index + 0u] = oidn_safe_value(albedo.x);
    albedo_buf[pixel_index + 1u] = oidn_safe_value(albedo.y);
    albedo_buf[pixel_index + 2u] = oidn_safe_value(albedo.z);
  }
  if (uniforms.use_normal != 0u) {
    /* Match the Metal kernel: normalized guide normal, deterministic fallback otherwise. */
    vec3 normal = texelFetch(normal_tx, ivec2(texel), 0).xyz;
    const bool normal_finite = !(any(isnan(normal)) || any(isinf(normal)));
    if (normal_finite && dot(normal, normal) > 1.0e-10) {
      normal = normalize(normal);
    }
    else {
      normal = vec3(0.0, 0.0, 1.0);
    }
    normal_buf[pixel_index + 0u] = normal.x;
    normal_buf[pixel_index + 1u] = normal.y;
    normal_buf[pixel_index + 2u] = normal.z;
  }
}
