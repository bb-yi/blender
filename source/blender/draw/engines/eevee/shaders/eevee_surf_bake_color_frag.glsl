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

#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_nodetree_infos.hh"
#include "infos/eevee_surf_bake_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)
FRAGMENT_SHADER_CREATE_INFO(eevee_geom_bake_mesh)
FRAGMENT_SHADER_CREATE_INFO(eevee_surf_bake_color)

#include "draw_view_lib.glsl"
#include "eevee_forward_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

/* Global lighting state used by Shader to RGB and by the final bake output. */
float g_thickness;
float3 g_forward_lighting_P;

float4 bake_closure_to_rgba()
{
  float3 radiance, transmittance;
  forward_lighting_eval(g_forward_lighting_P, g_thickness, radiance, transmittance);

  /* Reset for the next closure tree, matching the regular forward path. */
  float noise = utility_tx_fetch(utility_tx, gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling_rng_1D_get(SAMPLING_CLOSURE));
  closure_weights_reset(closure_rand);

  return float4(radiance, saturate(1.0f - average(transmittance)));
}

float4 closure_to_rgba(Closure cl_unused)
{
  UNUSED_VARS(cl_unused);
  return bake_closure_to_rgba();
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

float4 TextureHandle_eval(TextureHandle tex, float2 offset, bool texel_offset)
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
      return float4(g_data.P, 0.0f);
    case TEX_HANDLE_NORMAL:
      return float4(g_data.N, 0.0f);
    default:
      return float4(0.0f);
  }
}

float4 TextureHandle_eval(TextureHandle tex)
{
  return TextureHandle_eval(tex, float2(0.0f), true);
}
#endif

void main()
{
  material_surface_cull_discard();
  init_globals();
  /* UV-space front-facing is defined by UV triangle winding, not by the source mesh surface.
   * Keep local material lighting helpers on the evaluated mesh normal. */
  g_data.Ni = interp.N;
  g_data.N = safe_normalize(interp.N);
  g_data.Ng = g_data.N;
  fragment_displacement();

  float noise = utility_tx_fetch(utility_tx, gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling_rng_1D_get(SAMPLING_CLOSURE));

  g_forward_lighting_P = g_data.P;
  g_thickness = nodetree_thickness() * thickness_mode;

  nodetree_surface(closure_rand);

  float3 albedo = bake_color_resolve();
  float4 shaded = bake_closure_to_rgba();

#ifdef NPR_SHADER
  g_combined_color = float4(shaded.rgb, 1.0f);
  g_diffuse_color = float4(albedo, 1.0f);
  g_diffuse_direct = float4(shaded.rgb, 1.0f);
  g_diffuse_indirect = float4(0.0f, 0.0f, 0.0f, 1.0f);
  g_specular_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
  g_specular_direct = float4(0.0f, 0.0f, 0.0f, 1.0f);
  g_specular_indirect = float4(0.0f, 0.0f, 0.0f, 1.0f);
  g_average_normal = g_data.N;

  out_color = npr_bake_swap_alpha(nodetree_npr());
#else
  out_color = shaded;
#endif
}
