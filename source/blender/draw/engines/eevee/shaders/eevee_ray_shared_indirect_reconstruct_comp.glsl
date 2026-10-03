/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Reconstruct the single OIDN-denoised ray-grid indirect signal to full resolution.
 */

#include "infos/eevee_ray_shared_indirect_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_ray_shared_indirect_reconstruct)

#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_reverse_z_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_uniform.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"

#define raytrace_resolution_scale raytrace_buf.trace_pixel_scale

int2 representative_fullres_texel(int2 sample_texel)
{
  return sample_texel * raytrace_resolution_scale + raytrace_buf.trace_pixel_offset;
}

float3 closure_albedo_for_composite(ClosureUndetermined closure, gbuffer::Header header, int2 texel)
{
  if (closure.type == CLOSURE_NONE_ID ||
      raytrace_buf.skip_hardware_phase(
          closure.type == CLOSURE_BSDF_DIFFUSE_ID || closure.type == CLOSURE_BSSRDF_BURLEY_ID,
          closure.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID,
          closure.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID)) {
    return float3(0.0f);
  }

  float3 closure_color = max(closure.color, float3(0.0f));
  if ((closure.type == CLOSURE_BSDF_TRANSLUCENT_ID ||
       closure.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) &&
      (gbuffer::read_thickness(header, texel).value() != 0.0f))
  {
    closure_color *= closure_color;
  }
  return closure_color;
}

float3 fullres_composite_albedo(int2 texel)
{
  gbuffer::Header header = gbuffer::read_header(texel);
  float3 albedo = float3(0.0f);
  for (int i = 0; i < active_closure_count; i++) {
    albedo += closure_albedo_for_composite(gbuffer::read_bin(texel, i), header, texel);
  }
  return albedo;
}

bool closure_is_specular_material(ClosureUndetermined closure)
{
  return closure.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID ||
         closure.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID;
}

bool gbuffer_has_specular_material(gbuffer::Header header, int2 texel)
{
  for (int i = 0; i < active_closure_count; i++) {
    if (closure_is_specular_material(gbuffer::read_bin(texel, i))) {
      return true;
    }
  }
  return false;
}

bool albedo_chroma_compatible(float3 a, float3 b)
{
  float a_max = reduce_max(a);
  float b_max = reduce_max(b);
  if (a_max <= 1.0e-5f || b_max <= 1.0e-5f) {
    return false;
  }

  float3 a_chroma = a / a_max;
  float3 b_chroma = b / b_max;
  return reduce_max(abs(a_chroma - b_chroma)) < 0.65f;
}

bool representative_material_compatible(int2 sample_texel,
                                        bool center_has_specular,
                                        float3 center_albedo)
{
  if (!center_has_specular) {
    return true;
  }

  int2 sample_fullres = representative_fullres_texel(sample_texel);
  if (any(lessThan(sample_fullres, int2(0))) ||
      any(greaterThanEqual(sample_fullres, textureSize(gbuf_header_tx, 0).xy)))
  {
    return false;
  }

  gbuffer::Header sample_header = gbuffer::read_header(sample_fullres);
  if (!gbuffer_has_specular_material(sample_header, sample_fullres)) {
    return false;
  }

  return albedo_chroma_compatible(center_albedo, fullres_composite_albedo(sample_fullres));
}

bool representative_texel_valid(int2 sample_texel, float center_depth, float3 center_N)
{
  int2 sample_fullres = representative_fullres_texel(sample_texel);
  if (any(lessThan(sample_fullres, int2(0))) ||
      any(greaterThanEqual(sample_fullres, textureSize(gbuf_header_tx, 0).xy)))
  {
    return false;
  }

  float sample_depth = reverse_z::read(texelFetch(depth_tx, sample_fullres, 0).r);
  if (sample_depth <= 0.0f || sample_depth >= 1.0f ||
      abs(sample_depth - center_depth) > 2.0e-3f)
  {
    return false;
  }

  gbuffer::Layers sample_gbuf = gbuffer::read_layers(sample_fullres);
  return dot(sample_gbuf.surface_N(), center_N) > 0.5f;
}

float bicubic_bspline_weight(float x)
{
  x = abs(x);
  if (x < 1.0f) {
    return (4.0f + x * x * (-6.0f + 3.0f * x)) / 6.0f;
  }
  if (x < 2.0f) {
    float tail = 2.0f - x;
    return (tail * tail * tail) / 6.0f;
  }
  return 0.0f;
}

bool bicubic_shared_indirect_reconstruct(int2 texel_fullres,
                                         float center_depth,
                                         float3 center_N,
                                         float3 center_albedo,
                                         bool center_has_specular,
                                         out float3 radiance,
                                         out float3 albedo)
{
  if (!source_is_oidn ||
      raytrace_resolution_scale <= 1)
  {
    return false;
  }

  float scale = float(max(raytrace_resolution_scale, 1));
  float denominator = float(max(1, 1));
  float2 lowres_coord = (float2(texel_fullres - raytrace_buf.trace_pixel_offset) *
                         denominator) /
                        scale;
  float2 base_coord = floor(lowres_coord);
  float2 interp = lowres_coord - base_coord;
  int2 base_texel = int2(base_coord);
  int2 lowres_size = imageSize(shared_radiance_img).xy;

  float3 radiance_accum = float3(0.0f);
  float3 albedo_accum = float3(0.0f);
  float weight_accum = 0.0f;

  for (int y = -1; y <= 2; y++) {
    float weight_y = bicubic_bspline_weight(float(y) - interp.y);
    for (int x = -1; x <= 2; x++) {
      float weight = bicubic_bspline_weight(float(x) - interp.x) * weight_y;
      if (weight <= 0.0f) {
        continue;
      }

      int2 sample_texel = base_texel + int2(x, y);
      if (any(lessThan(sample_texel, int2(0))) || any(greaterThanEqual(sample_texel, lowres_size))) {
        continue;
      }
      if (!representative_texel_valid(sample_texel, center_depth, center_N)) {
        continue;
      }
      if (!representative_material_compatible(sample_texel, center_has_specular, center_albedo)) {
        continue;
      }

      radiance_accum += imageLoadFast(shared_radiance_img, sample_texel).rgb * weight;
      albedo_accum += imageLoadFast(shared_albedo_img, sample_texel).rgb * weight;
      weight_accum += weight;
    }
  }

  if (weight_accum <= 0.0f) {
    return false;
  }

  float inv_weight = 1.0f / weight_accum;
  radiance = radiance_accum * inv_weight;
  albedo = albedo_accum * inv_weight;
  return true;
}

float3 composite_fullres_albedo(float3 radiance,
                                float3 lowres_albedo,
                                float3 fullres_albedo,
                                bool allow_remodulate)
{
  if (raytrace_resolution_scale <= 1 ||
      !allow_remodulate)
  {
    return radiance;
  }

  if (reduce_max(fullres_albedo) <= 1.0e-5f || reduce_max(lowres_albedo) <= 1.0e-5f) {
    return radiance;
  }

  float3 ratio = fullres_albedo / max(lowres_albedo, float3(0.05f));
  /* Keep albedo remapping from amplifying residual denoising artifacts near dark guide texels. */
  ratio = min(ratio, float3(4.0f));
  return radiance * ratio;
}

void main()
{
  int2 texel_fullres = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel_fullres, imageSize(out_radiance_img).xy))) {
    return;
  }

  int2 lowres_size = imageSize(shared_radiance_img).xy;
  int2 texel = (texel_fullres - raytrace_buf.trace_pixel_offset) / raytrace_resolution_scale;
  texel = clamp(texel, int2(0), lowres_size - int2(1));

  if (raytrace_resolution_scale <= 1) {
    imageStoreFast(out_radiance_img, texel_fullres, imageLoadFast(shared_radiance_img, texel));
    return;
  }

  float center_depth = reverse_z::read(texelFetch(depth_tx, texel_fullres, 0).r);
  if (center_depth <= 0.0f || center_depth >= 1.0f) {
    imageStoreFast(out_radiance_img, texel_fullres, float4(0.0f));
    return;
  }

  gbuffer::Layers center_gbuf = gbuffer::read_layers(texel_fullres);
  float3 center_N = center_gbuf.surface_N();
  float3 center_albedo = fullres_composite_albedo(texel_fullres);
  bool center_has_specular = gbuffer_has_specular_material(center_gbuf.header, texel_fullres);

  float3 radiance_accum = float3(0.0f);
  float3 albedo_accum = float3(0.0f);
  float weight_accum = 0.0f;
  bool material_compatible = !center_has_specular;

  if (bicubic_shared_indirect_reconstruct(texel_fullres,
                                          center_depth,
                                          center_N,
                                          center_albedo,
                                          center_has_specular,
                                          radiance_accum,
                                          albedo_accum))
  {
    radiance_accum = composite_fullres_albedo(radiance_accum, albedo_accum, center_albedo, true);
    imageStoreFast(out_radiance_img, texel_fullres, float4(radiance_accum, 1.0f));
    return;
  }

  const int filter_radius = 1;
  for (int y = -filter_radius; y <= filter_radius; y++) {
    for (int x = -filter_radius; x <= filter_radius; x++) {
      int2 sample_texel = texel + int2(x, y);
      if (any(lessThan(sample_texel, int2(0))) || any(greaterThanEqual(sample_texel, lowres_size))) {
        continue;
      }
      if (!representative_texel_valid(sample_texel, center_depth, center_N)) {
        continue;
      }
      if (!representative_material_compatible(sample_texel, center_has_specular, center_albedo)) {
        continue;
      }

      float2 sample_delta = float2(sample_texel - texel);
      float weight = 1.0f / (1.0f + dot(sample_delta, sample_delta));
      radiance_accum += imageLoadFast(shared_radiance_img, sample_texel).rgb * weight;
      albedo_accum += imageLoadFast(shared_albedo_img, sample_texel).rgb * weight;
      weight_accum += weight;
      material_compatible = true;
    }
  }

  if (weight_accum > 0.0f) {
    radiance_accum /= weight_accum;
    albedo_accum /= weight_accum;
  }
  else {
    radiance_accum = imageLoadFast(shared_radiance_img, texel).rgb;
    albedo_accum = imageLoadFast(shared_albedo_img, texel).rgb;

    bool found_fallback = false;
    const int fallback_radius = 2;
    for (int y = -fallback_radius; y <= fallback_radius; y++) {
      for (int x = -fallback_radius; x <= fallback_radius; x++) {
        if (abs(x) <= filter_radius && abs(y) <= filter_radius) {
          continue;
        }
        int2 sample_texel = texel + int2(x, y);
        if (any(lessThan(sample_texel, int2(0))) ||
            any(greaterThanEqual(sample_texel, lowres_size)))
        {
          continue;
        }
        if (!representative_texel_valid(sample_texel, center_depth, center_N)) {
          continue;
        }
        if (!representative_material_compatible(sample_texel, center_has_specular, center_albedo)) {
          continue;
        }

        radiance_accum = imageLoadFast(shared_radiance_img, sample_texel).rgb;
        albedo_accum = imageLoadFast(shared_albedo_img, sample_texel).rgb;
        material_compatible = true;
        found_fallback = true;
        break;
      }
      if (found_fallback) {
        break;
      }
    }
  }

  radiance_accum = composite_fullres_albedo(
      radiance_accum, albedo_accum, center_albedo, material_compatible);
  imageStoreFast(out_radiance_img, texel_fullres, float4(radiance_accum, 1.0f));
}
