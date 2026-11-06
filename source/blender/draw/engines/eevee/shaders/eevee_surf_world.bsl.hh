/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Background used to shade the world.
 *
 * Outputs shading parameter per pixel using a set of randomized BSDFs.
 */
#pragma once

#include "eevee_attributes_world_lib.bsl.hh" /* IWYU pragma: export */
#include "eevee_colorspace_lib.bsl.hh"
#include "eevee_lightprobe.bsl.hh"
#include "eevee_nodetree_frag_lib.bsl.hh"
#include "eevee_pipeline.bsl.hh"
#include "eevee_surf_common.bsl.hh"

float4 closure_to_rgba_world(KernelGlobals &kg, ShadingData &sd, Closure /*cl*/)
{
  float3 transmittance = sd.transmittance;
  closure_weights_reset(kg, sd, 0.0f);
  return float4(0.0f, 0.0f, 0.0f, saturate(1.0f - average(transmittance)));
}

#ifdef NPR_SHADER
#  define TEX_HANDLE_NULL 0u
#  define TEX_HANDLE_WORLD_COMBINED_COLOR 1u
#  define TEX_HANDLE_WORLD_DIFFUSE_COLOR 2u
#  define TEX_HANDLE_WORLD_DIFFUSE_DIRECT 3u
#  define TEX_HANDLE_WORLD_DIFFUSE_INDIRECT 4u
#  define TEX_HANDLE_WORLD_SPECULAR_COLOR 5u
#  define TEX_HANDLE_WORLD_SPECULAR_DIRECT 6u
#  define TEX_HANDLE_WORLD_SPECULAR_INDIRECT 7u
#  define TEX_HANDLE_WORLD_POSITION 8u
#  define TEX_HANDLE_WORLD_NORMAL 9u

float4 g_world_combined_color;
float g_world_background_blur;
int4 g_world_coord_packed;
#endif

namespace eevee {

struct SurfWorld {
  [[push_constant]] float world_opacity_fade;
  [[push_constant]] float world_background_blur;
  [[push_constant]] int4 world_coord_packed;
};

struct SurfWorldFragOut {
  [[frag_color(0)]] float4 background;
};

float4 surf_world_background_color(ShadingData &sd, const float world_background_blur,
                                   const int4 world_coord_packed,
                                   const LightprobeRenderData &lightprobes)
{
  sd.holdout = saturate(sd.holdout);

  float4 background;
  background.rgb = colorspace::safe_color(sd.emission) * (1.0f - sd.holdout);
  background.a = saturate(average(sd.transmittance)) * sd.holdout;

  if (sd.ray_type == RAY_TYPE_CAMERA) {
    background.rgb = colorspace::brightness_clamp_max(background.rgb, 65504.0f);
  }

  if (sd.ray_type == RAY_TYPE_CAMERA && world_background_blur != 0.0f) {
    [[resource_table]] const LightprobeVolumeRenderData &lp_volumes = lightprobes.volumes;
    [[resource_table]] const LightprobeSphereRenderData &lp_spheres = lightprobes.spheres;

    float base_lod = lightprobe::sphere::roughness_to_lod(world_background_blur);
    float lod = max(1.0f, base_lod);
    float mix_factor = min(1.0f, base_lod);
    SphereProbeUvArea world_atlas_coord = reinterpret_as_atlas_coord(world_coord_packed);
    float4 probe_color = lp_spheres.sample_probe(-sd.N, lod, world_atlas_coord);
    background.rgb = mix(background.rgb, probe_color.rgb, mix_factor);

    SphericalHarmonicL1<float4> volume_irradiance = lp_volumes.world();
    float3 radiance_sh = volume_irradiance.evaluate_lambert(-sd.N).rgb;
    float radiance_mix_factor = lightprobe::sphere::roughness_to_mix_fac(world_background_blur);
    background.rgb = mix(background.rgb, radiance_sh, radiance_mix_factor);
  }

  return background;
}

[[fragment]] [[early_fragment_tests]]
void surf_world([[resource_table]] KernelGlobals &kg,
                [[resource_table]] PipelineConstants &pipe,
                [[resource_table]] SurfWorld &srt,
                [[resource_table]] const eevee::LightprobeRenderData &lightprobes,
                [[resource_table]] RenderPassOutput &render_passes,
                [[resource_table]] const Uniform &uni,
                [[resource_table]] const draw::View &views,
                [[frag_coord]] const float4 frag_co,
                [[in]] const VertOutCommon &interp,
                [[out]] SurfWorldFragOut &frag_out,
                [[front_facing]] const bool front_face)
{
  const ViewMatrices view = views.get(0);

  ShadingData sd = init_globals(pipe, uni, interp, view, front_face, frag_co);

  /* View position is passed to keep accuracy. */
  sd.N = view.normal_view_to_world(view.view_incident_vector(interp.P));
  sd.Ni = sd.N;
  sd.Ng = sd.N;
  sd.P = -sd.N;
  sd.world_view_position = interp.P;
  attrib_load(WorldPoint{sd.P});

  nodetree_surface(kg, sd, 0.0f);

  frag_out.background = surf_world_background_color(
      sd, srt.world_background_blur, srt.world_coord_packed, lightprobes);

#ifdef NPR_SHADER
  g_world_combined_color = frag_out.background;
  g_world_background_blur = srt.world_background_blur;
  g_world_coord_packed = srt.world_coord_packed;
  frag_out.background = nodetree_npr(kg, sd);
#endif

  /* Output environment pass. */
  float4 environment = frag_out.background;
  environment.a = 1.0f - environment.a;
  environment.rgb *= environment.a;
  render_passes.store_color(
      int2(frag_co.xy), uni.uniform_buf.render_pass.environment_id, environment);

  frag_out.background = mix(
      float4(0.0f, 0.0f, 0.0f, 1.0f), frag_out.background, srt.world_opacity_fade);
}
}  // namespace eevee

#ifdef NPR_SHADER
float3 world_direction_from_offset(KernelGlobals &kg, ShadingData &sd, float2 offset, bool texel_offset)
{
  [[resource_table]] const eevee::Uniform &uni = resource_table_get(eevee::Uniform);
  [[resource_table]] const draw::View &views = resource_table_get(draw::View);
  const ViewMatrices view = views.get(0);

  float2 screen_uv = view.point_view_to_screen(sd.world_view_position).xy;
  if (texel_offset) {
    screen_uv += offset / float2(uni.uniform_buf.film.render_extent);
  }
  else {
    float3 offset_view_position = sd.world_view_position + float3(offset, 0.0f);
    screen_uv = view.point_view_to_screen(offset_view_position).xy;
  }

  screen_uv = clamp(screen_uv, float2(0.0f), float2(1.0f));
  float3 view_direction = view.view_incident_vector(view.point_screen_to_view(float3(
      screen_uv, 1.0f)));
  return view.normal_view_to_world(view_direction);
}

float4 world_combined_color_from_direction(KernelGlobals &kg, ShadingData &sd, float3 direction)
{
  [[resource_table]] const eevee::LightprobeRenderData &lightprobes = resource_table_get(
      eevee::LightprobeRenderData);

  /* A separate shading state keeps the caller's closure reservoirs, diffusion scope,
   * emission and ray state intact while sampling another world direction. */
  ShadingData sample_sd = sd;
  sample_sd.N = direction;
  sample_sd.Ni = direction;
  sample_sd.Ng = direction;
  sample_sd.P = -direction;
  attrib_load(WorldPoint{sample_sd.P});
  nodetree_surface(kg, sample_sd, 0.0f);
  float4 color = eevee::surf_world_background_color(
      sample_sd, g_world_background_blur, g_world_coord_packed, lightprobes);
  attrib_load(WorldPoint{sd.P});

  return color;
}

void npr_input_impl(TextureHandle &combined_color,
                    TextureHandle &diffuse_color,
                    TextureHandle &diffuse_direct,
                    TextureHandle &diffuse_indirect,
                    TextureHandle &specular_color,
                    TextureHandle &specular_direct,
                    TextureHandle &specular_indirect,
                    TextureHandle &position,
                    TextureHandle &normal)
{
  combined_color = TextureHandle{TEX_HANDLE_WORLD_COMBINED_COLOR, 0};
  diffuse_color = TextureHandle{TEX_HANDLE_WORLD_DIFFUSE_COLOR, 0};
  diffuse_direct = TextureHandle{TEX_HANDLE_WORLD_DIFFUSE_DIRECT, 0};
  diffuse_indirect = TextureHandle{TEX_HANDLE_WORLD_DIFFUSE_INDIRECT, 0};
  specular_color = TextureHandle{TEX_HANDLE_WORLD_SPECULAR_COLOR, 0};
  specular_direct = TextureHandle{TEX_HANDLE_WORLD_SPECULAR_DIRECT, 0};
  specular_indirect = TextureHandle{TEX_HANDLE_WORLD_SPECULAR_INDIRECT, 0};
  position = TextureHandle{TEX_HANDLE_WORLD_POSITION, 0};
  normal = TextureHandle{TEX_HANDLE_WORLD_NORMAL, 0};
}

void npr_refraction_impl(TextureHandle &combined_color, TextureHandle &position)
{
  combined_color = TEXTURE_HANDLE_DEFAULT;
  position = TEXTURE_HANDLE_DEFAULT;
}

void input_aov_impl(uint /*hash*/, TextureHandle &color, TextureHandle &value)
{
  color = TEXTURE_HANDLE_DEFAULT;
  value = TEXTURE_HANDLE_DEFAULT;
}

bool TextureHandle_is_scene_depth(TextureHandle /*tex*/)
{
  return false;
}

float4 TextureHandle_eval(KernelGlobals kg, ShadingData &sd, TextureHandle tex, float2 offset, bool texel_offset)
{
  if (tex.type == TEX_HANDLE_NULL) {
    return float4(0.0f);
  }

  if (all(equal(offset, float2(0.0f)))) {
    switch (tex.type) {
      case TEX_HANDLE_WORLD_COMBINED_COLOR:
      case TEX_HANDLE_WORLD_DIFFUSE_COLOR:
      case TEX_HANDLE_WORLD_DIFFUSE_DIRECT:
      case TEX_HANDLE_WORLD_DIFFUSE_INDIRECT:
      case TEX_HANDLE_WORLD_SPECULAR_COLOR:
      case TEX_HANDLE_WORLD_SPECULAR_DIRECT:
      case TEX_HANDLE_WORLD_SPECULAR_INDIRECT:
        return g_world_combined_color;
      case TEX_HANDLE_WORLD_POSITION:
        return float4(sd.P, 0.0f);
      case TEX_HANDLE_WORLD_NORMAL:
        return float4(sd.N, 0.0f);
      default:
        break;
    }
  }

  switch (tex.type) {
    case TEX_HANDLE_WORLD_COMBINED_COLOR:
    case TEX_HANDLE_WORLD_DIFFUSE_COLOR:
    case TEX_HANDLE_WORLD_DIFFUSE_DIRECT:
    case TEX_HANDLE_WORLD_DIFFUSE_INDIRECT:
    case TEX_HANDLE_WORLD_SPECULAR_COLOR:
    case TEX_HANDLE_WORLD_SPECULAR_DIRECT:
    case TEX_HANDLE_WORLD_SPECULAR_INDIRECT: {
      float3 direction = world_direction_from_offset(kg, sd, offset, texel_offset);
      return world_combined_color_from_direction(kg, sd, direction);
    }
    case TEX_HANDLE_WORLD_NORMAL: {
      float3 direction = world_direction_from_offset(kg, sd, offset, texel_offset);
      return float4(direction, 0.0f);
    }
    case TEX_HANDLE_WORLD_POSITION: {
      float3 direction = world_direction_from_offset(kg, sd, offset, texel_offset);
      return float4(-direction, 0.0f);
    }
    default:
      break;
  }
  return float4(0.0f);
}

float4 TextureHandle_eval(KernelGlobals kg, ShadingData &sd, TextureHandle tex)
{
  return TextureHandle_eval(kg, sd, tex, float2(0.0f), false);
}

float4 TextureHandle_eval_uv(KernelGlobals kg, ShadingData &sd, TextureHandle tex, float2 uv)
{
  return TextureHandle_eval(kg, sd, tex, uv - float2(0.5f), false);
}
#endif
