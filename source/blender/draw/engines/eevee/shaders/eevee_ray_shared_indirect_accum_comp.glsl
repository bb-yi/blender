/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Nuru c603d408e9f3: accumulate colored closure radiance and primary-surface OIDN guides.
 * Coordinate conversion follows the current ray generator's integer sampling grid. */
#include "infos/eevee_ray_shared_indirect_infos.hh"
COMPUTE_SHADER_CREATE_INFO(eevee_ray_shared_indirect_accum)

#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_uniform.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"

void main()
{
  uint2 tile_coord = unpackUvec2x16(tiles_coord_buf[gl_WorkGroupID.x]);
  int2 texel = int2(gl_LocalInvocationID.xy + tile_coord * RAYTRACE_GROUP_SIZE);
  if (any(greaterThanEqual(texel, imageSize(shared_radiance_img).xy))) {
    return;
  }
  int2 texel_fullres = texel * raytrace_buf.trace_pixel_scale + raytrace_buf.trace_pixel_offset;
  if (any(lessThan(texel_fullres, int2(0))) ||
      any(greaterThanEqual(texel_fullres, textureSize(gbuf_header_tx, 0).xy)))
  {
    return;
  }
  ClosureUndetermined closure = gbuffer::read_bin(texel_fullres, closure_index);
  if (closure.type == CLOSURE_NONE_ID ||
      raytrace_buf.skip_hardware_phase(
          closure.type == CLOSURE_BSDF_DIFFUSE_ID || closure.type == CLOSURE_BSSRDF_BURLEY_ID,
          closure.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID,
          closure.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID)) {
    return;
  }
  float3 color = max(closure.color, float3(0.0f));
  if ((closure.type == CLOSURE_BSDF_TRANSLUCENT_ID ||
       closure.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) &&
      gbuffer::read_thickness(gbuffer::read_header(texel_fullres), texel_fullres).value() != 0.0f)
  {
    color *= color;
  }
  float3 radiance = max(imageLoadFast(ray_radiance_img, texel).rgb, float3(0.0f));
  float3 accum = imageLoadFast(shared_radiance_img, texel).rgb + radiance * color;
  imageStoreFast(shared_radiance_img, texel, float4(accum, 1.0f));
  float4 albedo = imageLoadFast(shared_albedo_img, texel) + float4(color, 1.0f);
  imageStoreFast(shared_albedo_img, texel, albedo);
  if (imageLoadFast(shared_albedo_img, texel).a <= 1.0f) {
    imageStoreFast(shared_normal_img, texel, float4(closure.N, 1.0f));
  }
}
