/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Background used to shade the world.
 *
 * Outputs shading parameter per pixel using a set of randomized BSDFs.
 */

#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_nodetree_infos.hh"
#include "infos/eevee_surf_world_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)
FRAGMENT_SHADER_CREATE_INFO(eevee_geom_world)
FRAGMENT_SHADER_CREATE_INFO(eevee_surf_world)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.glsl"
#include "eevee_colorspace_lib.glsl"
#include "eevee_lightprobe_sphere_lib.glsl"
#include "eevee_lightprobe_volume_eval_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

#define TEX_HANDLE_NULL 0u
#define TEX_HANDLE_WORLD_COMBINED_COLOR 1u

float4 g_world_combined_color;

#ifdef NPR_SHADER
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
  combined_color = TextureHandle(TEX_HANDLE_WORLD_COMBINED_COLOR, 0);
  diffuse_color = TEXTURE_HANDLE_DEFAULT;
  diffuse_direct = TEXTURE_HANDLE_DEFAULT;
  diffuse_indirect = TEXTURE_HANDLE_DEFAULT;
  specular_color = TEXTURE_HANDLE_DEFAULT;
  specular_direct = TEXTURE_HANDLE_DEFAULT;
  specular_indirect = TEXTURE_HANDLE_DEFAULT;
  position = TEXTURE_HANDLE_DEFAULT;
  normal = TEXTURE_HANDLE_DEFAULT;
}

float4 TextureHandle_eval(TextureHandle tex, float2 offset, bool texel_offset)
{
  UNUSED_VARS(offset);
  UNUSED_VARS(texel_offset);
  if (tex.type == TEX_HANDLE_WORLD_COMBINED_COLOR) {
    return g_world_combined_color;
  }
  return float4(0.0f);
}

float4 TextureHandle_eval(TextureHandle tex)
{
  return TextureHandle_eval(tex, float2(0.0f), false);
}
#endif

float4 closure_to_rgba(Closure cl)
{
  return float4(0.0f);
}

void main()
{
  init_globals();
  /* View position is passed to keep accuracy. */
  g_data.N = drw_normal_view_to_world(drw_view_incident_vector(interp.P));
  g_data.Ng = g_data.N;
  g_data.P = -g_data.N;
  attrib_load(WorldPoint{0});

  nodetree_surface(0.0f);

  g_holdout = saturate(g_holdout);

  out_background.rgb = colorspace_safe_color(g_emission) * (1.0f - g_holdout);
  out_background.a = saturate(average(g_transmittance)) * g_holdout;

  if (g_data.ray_type == RAY_TYPE_CAMERA && world_background_blur != 0.0f) {
    float base_lod = sphere_probe_roughness_to_lod(world_background_blur);
    float lod = max(1.0f, base_lod);
    float mix_factor = min(1.0f, base_lod);
    SphereProbeUvArea world_atlas_coord = reinterpret_as_atlas_coord(world_coord_packed);
    float4 probe_color = lightprobe_spheres_sample(-g_data.N, lod, world_atlas_coord);
    out_background.rgb = mix(out_background.rgb, probe_color.rgb, mix_factor);

    SphericalHarmonicL1 volume_irradiance = lightprobe_volume_sample(
        g_data.P, float3(0.0f), g_data.Ng);
    float3 radiance_sh = spherical_harmonics_evaluate_lambert(-g_data.N, volume_irradiance);
    float radiance_mix_factor = sphere_probe_roughness_to_mix_fac(world_background_blur);
    out_background.rgb = mix(out_background.rgb, radiance_sh, radiance_mix_factor);
  }

#ifdef NPR_SHADER
  g_world_combined_color = out_background;
  out_background = nodetree_npr();
#endif

  /* Output environment pass. */
#ifdef MAT_RENDER_PASS_SUPPORT
  float4 environment = out_background;
  environment.a = 1.0f - environment.a;
  environment.rgb *= environment.a;
  output_renderpass_color(uniform_buf.render_pass.environment_id, environment);
#endif

  out_background = mix(float4(0.0f, 0.0f, 0.0f, 1.0f), out_background, world_opacity_fade);
}
