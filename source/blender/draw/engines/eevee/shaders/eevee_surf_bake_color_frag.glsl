/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Eevee UV-space color baking.
 *
 * This shader evaluates the normal Eevee GPUMaterial graph for a mesh surface and writes the
 * local material color into a bake target. It deliberately avoids screen-space inputs and final
 * view post-processing; those are rejected by the bake callback before this shader is used.
 */

#include "infos/eevee_surf_bake_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_surf_bake_color)

#include "eevee_forward_lib.bsl.hh"
#include "eevee_nodetree_lib.bsl.hh"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"

Closure nodetree_surface(KernelGlobals kg, ShadingData &sd, float closure_rand);
float nodetree_thickness(KernelGlobals kg, ShadingData &sd);
float4 bake_lighting_eval(KernelGlobals kg, ShadingData &sd);

float bake_closure_rand_get(KernelGlobals kg, ShadingData &sd)
{
  float noise = texelFetch(utility_tx, int3(int2(sd.frag_co.xy) % UTIL_TEX_SIZE,
                                         UTIL_BLUE_NOISE_LAYER), 0).r;
  return fract(noise + eevee_sampling_rng_1D_get(SAMPLING_CLOSURE));
}

void bake_closure_tree_reset(KernelGlobals kg, ShadingData &sd, float closure_rand)
{
  closure_weights_reset(kg, sd, closure_rand);
  sd.bake_principled_depth = 0;
}

float4 bake_closure_to_rgba(KernelGlobals kg, ShadingData &sd)
{
  float4 shaded = bake_lighting_eval(kg, sd);
  bake_closure_tree_reset(kg, sd, bake_closure_rand_get(kg, sd));
  return shaded;
}

float4 closure_to_rgba(KernelGlobals kg, ShadingData &sd, Closure /*cl_unused*/)
{
  return bake_closure_to_rgba(kg, sd);
}

#ifdef NPR_SHADER

#  define TEX_HANDLE_NULL 0u
#  define TEX_HANDLE_COMBINED_COLOR 10u
#  define TEX_HANDLE_DIFFUSE_COLOR 11u
#  define TEX_HANDLE_DIFFUSE_DIRECT 12u
#  define TEX_HANDLE_DIFFUSE_INDIRECT 13u
#  define TEX_HANDLE_SPECULAR_COLOR 14u
#  define TEX_HANDLE_SPECULAR_DIRECT 15u
#  define TEX_HANDLE_SPECULAR_INDIRECT 16u
#  define TEX_HANDLE_POSITION 17u
#  define TEX_HANDLE_NORMAL 18u

void npr_input_impl(out TextureHandle combined_color,
                    out TextureHandle diffuse_color,
                    out TextureHandle diffuse_direct,
                    out TextureHandle diffuse_indirect,
                    out TextureHandle specular_color,
                    out TextureHandle specular_direct,
                    out TextureHandle specular_indirect,
                    out TextureHandle position,
                    out TextureHandle normal)
{
  combined_color = TextureHandle(TEX_HANDLE_COMBINED_COLOR, 0);
  diffuse_color = TextureHandle(TEX_HANDLE_DIFFUSE_COLOR, 0);
  diffuse_direct = TextureHandle(TEX_HANDLE_DIFFUSE_DIRECT, 0);
  diffuse_indirect = TextureHandle(TEX_HANDLE_DIFFUSE_INDIRECT, 0);
  specular_color = TextureHandle(TEX_HANDLE_SPECULAR_COLOR, 0);
  specular_direct = TextureHandle(TEX_HANDLE_SPECULAR_DIRECT, 0);
  specular_indirect = TextureHandle(TEX_HANDLE_SPECULAR_INDIRECT, 0);
  position = TextureHandle(TEX_HANDLE_POSITION, 0);
  normal = TextureHandle(TEX_HANDLE_NORMAL, 0);
}

void npr_refraction_impl(out TextureHandle combined_color, out TextureHandle position)
{
  combined_color = TEXTURE_HANDLE_DEFAULT;
  position = TEXTURE_HANDLE_DEFAULT;
}

void input_aov_impl(uint hash, out TextureHandle color, out TextureHandle value)
{
  color = TEXTURE_HANDLE_DEFAULT;
  value = TEXTURE_HANDLE_DEFAULT;
}

float4 npr_bake_swap_alpha(float4 v)
{
  v.a = 1.0f - saturate(v.a);
  return v;
}

#  define TEXTURE_HANDLE_EVAL_DEFINED

float4 TextureHandle_eval(KernelGlobals kg, ShadingData &sd, TextureHandle tex, float2 offset, bool texel_offset)
{
  if (!all(equal(offset, float2(0.0f)))) {
    return float4(0.0f);
  }

  switch (tex.type) {
    case TEX_HANDLE_COMBINED_COLOR:
      return npr_bake_swap_alpha(g_combined_color);
    case TEX_HANDLE_DIFFUSE_COLOR:
      return npr_bake_swap_alpha(g_diffuse_color);
    case TEX_HANDLE_DIFFUSE_DIRECT:
      return npr_bake_swap_alpha(g_diffuse_direct);
    case TEX_HANDLE_DIFFUSE_INDIRECT:
      return npr_bake_swap_alpha(g_diffuse_indirect);
    case TEX_HANDLE_SPECULAR_COLOR:
      return npr_bake_swap_alpha(g_specular_color);
    case TEX_HANDLE_SPECULAR_DIRECT:
      return npr_bake_swap_alpha(g_specular_direct);
    case TEX_HANDLE_SPECULAR_INDIRECT:
      return npr_bake_swap_alpha(g_specular_indirect);
    case TEX_HANDLE_POSITION:
      return float4(sd.P, 0.0f);
    case TEX_HANDLE_NORMAL:
      return float4(sd.N, 0.0f);
    default:
      return float4(0.0f);
  }
}

float4 TextureHandle_eval(KernelGlobals kg, ShadingData &sd, TextureHandle tex)
{
  return TextureHandle_eval(kg, sd, tex, float2(0.0f), true);
}
#endif

void bake_evaluate(KernelGlobals kg, ShadingData &sd, float3 geom_normal, int primitive_id,
                   int thickness_mode)
{
  int2 texel = int2(sd.frag_co.xy);
  if (texelFetch(bake_primitive_tx, texel, 0).r != primitive_id) {
    gpu_discard_fragment();
    return;
  }
  /* UV-space front-facing is defined by UV triangle winding, not by the source mesh surface.
   * Keep local material lighting helpers on the evaluated mesh normal. */
  sd.N = safe_normalize(sd.Ni);
  sd.Ng = safe_normalize(geom_normal);
  fragment_displacement(kg, sd);

  float closure_rand = bake_closure_rand_get(kg, sd);

  sd.thickness = Thickness::from(nodetree_thickness(kg, sd), ThicknessMode(thickness_mode));

  sd.bake_color = float3(0.0f);
  sd.bake_color_weight = 0.0f;
  sd.bake_principled_depth = 0;
  bake_closure_tree_reset(kg, sd, closure_rand);
  nodetree_surface(kg, sd, closure_rand);

  float3 albedo = bake_color_resolve(sd);
  float4 shaded = bake_closure_to_rgba(kg, sd);

#ifdef NPR_SHADER
  g_combined_color = float4(shaded.rgb, 1.0f);
  g_diffuse_color = float4(albedo, 1.0f);
  g_diffuse_direct = float4(shaded.rgb, 1.0f);
  g_diffuse_indirect = float4(0.0f, 0.0f, 0.0f, 1.0f);
  g_specular_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
  g_specular_direct = float4(0.0f, 0.0f, 0.0f, 1.0f);
  g_specular_indirect = float4(0.0f, 0.0f, 0.0f, 1.0f);
  g_average_normal = sd.N;

  out_color = npr_bake_swap_alpha(nodetree_npr(kg, sd));
#else
  out_color = shaded;
#endif
}
