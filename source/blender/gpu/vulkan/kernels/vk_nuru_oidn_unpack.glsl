/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Nuru: unpack OpenImageDenoise filtered float3 radiance back into the output texture,
 * preserving the alpha channel from the input radiance. Vulkan port of the Metal
 * `eevee_oidn_unpack` kernel. */

#version 460
#extension GL_EXT_scalar_block_layout : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(scalar, binding = 0) uniform NuruUniforms {
  uint extent_x;
  uint extent_y;
  uint use_albedo;
  uint use_normal;
} uniforms;

layout(scalar, binding = 2) readonly buffer B2 { float output_buf[]; };

layout(binding = 16) uniform sampler2D input_radiance_tx;
layout(binding = 41) uniform writeonly image2D output_radiance_img;

void main()
{
  uvec2 texel = gl_GlobalInvocationID.xy;
  if (texel.x >= uniforms.extent_x || texel.y >= uniforms.extent_y) {
    return;
  }
  uint pixel_index = (texel.y * uniforms.extent_x + texel.x) * 3u;

  float alpha = texelFetch(input_radiance_tx, ivec2(texel), 0).w;
  vec3 filtered = vec3(
      output_buf[pixel_index + 0u], output_buf[pixel_index + 1u], output_buf[pixel_index + 2u]);
  imageStore(output_radiance_img, ivec2(texel), vec4(filtered, alpha));
}
