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
#define TEX_HANDLE_WORLD_POSITION 2u
#define TEX_HANDLE_WORLD_NORMAL 3u

float4 g_world_combined_color;

#ifdef NPR_SHADER
float3 world_direction_from_offset(float2 offset, bool texel_offset)
{
  float2 screen_uv = drw_point_view_to_screen(interp.P).xy;
  if (texel_offset) {
    screen_uv += offset / float2(uniform_buf.film.render_extent);
  }
  else {
    float3 offset_view_position = interp.P + float3(offset, 0.0f);
    screen_uv = drw_point_view_to_screen(offset_view_position).xy;
  }

  screen_uv = clamp(screen_uv, float2(0.0f), float2(1.0f));
  float3 view_direction = drw_view_incident_vector(drw_point_screen_to_view(float3(screen_uv, 1.0f)));
  return drw_normal_view_to_world(view_direction);
}

float4 world_combined_color_from_direction(float3 direction)
{
  GlobalData current_data = g_data;
  float3 current_emission = g_emission;
  float3 current_transmittance = g_transmittance;
  float current_holdout = g_holdout;
  float3 current_volume_scattering = g_volume_scattering;
  float current_volume_anisotropy = g_volume_anisotropy;
  float3 current_volume_absorption = g_volume_absorption;
  ClosureUndetermined current_closure_bin0 = g_closure_bins[0];
  float current_closure_rand0 = g_closure_rand[0];
#if CLOSURE_BIN_COUNT > 1
  ClosureUndetermined current_closure_bin1 = g_closure_bins[1];
  float current_closure_rand1 = g_closure_rand[1];
#endif
#if CLOSURE_BIN_COUNT > 2
  ClosureUndetermined current_closure_bin2 = g_closure_bins[2];
  float current_closure_rand2 = g_closure_rand[2];
#endif
  bool current_closure_reflection_bin = g_closure_reflection_bin;

  g_data.N = direction;
  g_data.Ng = direction;
  g_data.P = -direction;
  attrib_load(WorldPoint{0});

  nodetree_surface(0.0f);

  float holdout = saturate(g_holdout);
  float4 color;
  color.rgb = colorspace_safe_color(g_emission) * (1.0f - holdout);
  color.a = saturate(average(g_transmittance)) * holdout;

  if (g_data.ray_type == RAY_TYPE_CAMERA && world_background_blur != 0.0f) {
    float base_lod = sphere_probe_roughness_to_lod(world_background_blur);
    float lod = max(1.0f, base_lod);
    float mix_factor = min(1.0f, base_lod);
    SphereProbeUvArea world_atlas_coord = reinterpret_as_atlas_coord(world_coord_packed);
    float4 probe_color = lightprobe_spheres_sample(-g_data.N, lod, world_atlas_coord);
    color.rgb = mix(color.rgb, probe_color.rgb, mix_factor);

    SphericalHarmonicL1 volume_irradiance = lightprobe_volume_sample(
        g_data.P, float3(0.0f), g_data.Ng);
    float3 radiance_sh = spherical_harmonics_evaluate_lambert(-g_data.N, volume_irradiance);
    float radiance_mix_factor = sphere_probe_roughness_to_mix_fac(world_background_blur);
    color.rgb = mix(color.rgb, radiance_sh, radiance_mix_factor);
  }

  g_data = current_data;
  g_emission = current_emission;
  g_transmittance = current_transmittance;
  g_holdout = current_holdout;
  g_volume_scattering = current_volume_scattering;
  g_volume_anisotropy = current_volume_anisotropy;
  g_volume_absorption = current_volume_absorption;
  g_closure_bins[0] = current_closure_bin0;
  g_closure_rand[0] = current_closure_rand0;
#if CLOSURE_BIN_COUNT > 1
  g_closure_bins[1] = current_closure_bin1;
  g_closure_rand[1] = current_closure_rand1;
#endif
#if CLOSURE_BIN_COUNT > 2
  g_closure_bins[2] = current_closure_bin2;
  g_closure_rand[2] = current_closure_rand2;
#endif
  g_closure_reflection_bin = current_closure_reflection_bin;

  return color;
}

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
  position = TextureHandle(TEX_HANDLE_WORLD_POSITION, 0);
  normal = TextureHandle(TEX_HANDLE_WORLD_NORMAL, 0);
}

float4 TextureHandle_eval(TextureHandle tex, float2 offset, bool texel_offset)
{
  if (tex.type == TEX_HANDLE_NULL) {
    return float4(0.0f);
  }

  if (all(equal(offset, float2(0.0f)))) {
    switch (tex.type) {
      case TEX_HANDLE_WORLD_COMBINED_COLOR:
        return g_world_combined_color;
      case TEX_HANDLE_WORLD_POSITION:
        return float4(g_data.P, 0.0f);
      case TEX_HANDLE_WORLD_NORMAL:
        return float4(g_data.N, 0.0f);
      default:
        break;
    }
  }

  switch (tex.type) {
    case TEX_HANDLE_WORLD_COMBINED_COLOR: {
      float3 direction = world_direction_from_offset(offset, texel_offset);
      return world_combined_color_from_direction(direction);
    }
    case TEX_HANDLE_WORLD_NORMAL: {
      float3 direction = world_direction_from_offset(offset, texel_offset);
      return float4(direction, 0.0f);
    }
    case TEX_HANDLE_WORLD_POSITION: {
      float3 direction = world_direction_from_offset(offset, texel_offset);
      return float4(-direction, 0.0f);
    }
    default:
      break;
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
