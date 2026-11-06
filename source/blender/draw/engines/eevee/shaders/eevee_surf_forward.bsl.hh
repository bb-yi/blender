/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Forward lighting evaluation: Lighting is evaluated during the geometry rasterization.
 *
 * This is used by alpha blended materials and materials using Shader to RGB nodes.
 */
#pragma once

#include "draw_view.bsl.hh"
#include "eevee_forward_lib.bsl.hh"
#include "eevee_nodetree_frag_lib.bsl.hh"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_outline_lib.glsl"
#include "eevee_surf_common.bsl.hh"
#include "eevee_outline_shell_lib.bsl.hh"
#include "eevee_transparency.bsl.hh"
#include "eevee_volume_lib.bsl.hh"

float4 closure_to_rgba_forward(KernelGlobals &kg, ShadingData &sd, Closure /*cl_unused*/)
{
  [[resource_table]] const eevee::PipelineConstants &pipe = kg.pipe;
  if (!pipe.is_occupancy_pipe) [[static_branch]] {
    [[resource_table]] const draw::View &views = kg.view;
    [[resource_table]] const eevee::Sampling &sampling = kg.sampling;
    [[resource_table]] const UtilityTexture &util_tx = kg.util_tx;
    draw::ID id{sd.resource_id_raw};
    const uint resource_id = id.resource_id<1>();

    const float2 frag_co = sd.frag_co.xy;

    float3 radiance, transmittance;
    float3 nodetree_P = sd.P;
    sd.P = sd.forward_lighting_P;
    eevee::forward_lighting_eval(
        kg, sd, views.get(0), resource_id, sd.thickness, frag_co, radiance, transmittance);

    /* Reset for the next closure tree. */
    float noise = util_tx.fetch(frag_co, UTIL_BLUE_NOISE_LAYER).r;
    float closure_rand = fract(noise + sampling.rng_1D_get(SAMPLING_CLOSURE));
    closure_weights_reset(kg, sd, closure_rand);

    if (pipe.use_forward_lighting && pipe.use_transparency) [[static_branch]] {
      [[resource_table]] eevee::LightprobeRenderData &lightprobes = kg.lightprobes;
      [[resource_table]] eevee::LightprobeSphereRenderData &lp_spheres = lightprobes.spheres;
      [[resource_table]] eevee::PreviousLayerHiZ &prev_hiz = kg.previous_layer_hiz;
      [[resource_table]] eevee::PreviousLayerRadiance &prev_radiance = kg.previous_layer_radiance;

      float3 V = -views.get(0).world_incident_vector(sd.P);
      eevee::LightProbeSample samp = lightprobes.load(frag_co, sd.P, sd.N, V);
      float3 radiance_behind = lp_spheres.spherical_sample_normalized_with_parallax(
          samp, sd.P, V, 0.0);

      int2 texel = int2(frag_co);
      if (texelFetchExtend(prev_hiz.hiz_prev_tx, texel, 0).x != 1.0f) {
        radiance_behind = texelFetch(prev_radiance.previous_layer_radiance_tx, texel, 0).xyz;
      }

      radiance += radiance_behind * saturate(transmittance);
    }

    sd.P = nodetree_P;
    return float4(radiance, saturate(1.0f - average(transmittance)));
  }
  else {
    return float4(0);
  }
}

namespace eevee {

struct SurfaceForwardFragOut {
  /* Splitting RGB components into different target to overcome the lack of dual source blending
   * with multiple render targets. */
  [[frag_color(0)]] float4 combined_r;
  [[frag_color(1)]] float4 combined_g;
  [[frag_color(2)]] float4 combined_b;
  [[frag_color(3)]] float4 combined_a;
};

void surf_forward_impl([[resource_table]] KernelGlobals &kg,
                  [[resource_table]] PipelineConstants &pipe,
                  [[resource_table]] LightEvalIterator & lights,
                  [[resource_table]] LightprobeRenderData & lightprobes,
                  [[resource_table]] LightprobePlaneRenderData & lightprobe_planes,
                  [[resource_table]] const draw::View &views,
                  [[resource_table]] const draw::Model & models,
                  [[resource_table]] const draw::Infos & infos,
                  [[resource_table]] const UnifiedVolumeData &volumes,
                  [[resource_table]] const Uniform &uni,
                  [[resource_table]] const Sampling &sampling,
                  [[resource_table]] const UtilityTexture &util_tx,
                  const VertOutCommon &interp,
                  const float4 frag_co,
                   SurfaceForwardFragOut &frag_out, float &out_depth, ShadingData &sd)
{
#ifdef MAT_OUTLINE_SHELL_SELF_OCCLUSION
  if (outline_shell_self_occluded()) {
    gpu_discard_fragment();
    return;
  }
#endif
  draw::ID id{interp.resource_id_raw};
  const uint resource_id = id.resource_id<1>();
  const ViewMatrices view = views.get(0);



  float noise = util_tx.fetch(frag_co.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling.rng_1D_get(SAMPLING_CLOSURE));

  float volume_depth = reverse_z::read(frag_co.z);
#ifdef MAT_DEPTH_OFFSET
  float depth_offset = nodetree_depth_offset(kg, sd);
  if (!material_depth_offset_is_zero(kg, sd, depth_offset)) {
    volume_depth = material_depth_offset_screen_depth(kg, sd, depth_offset);
    float3 depth_offset_P = material_depth_offset_apply_nodetree_position(kg, sd, depth_offset);
#  ifndef MAT_DEPTH_OFFSET_NO_LIGHTING
    sd.forward_lighting_P = depth_offset_P;
#  endif
  }
  material_depth_offset_write(kg, sd, depth_offset, out_depth);
#endif

  fragment_displacement(kg, sd);

  sd.thickness = Thickness::from(nodetree_thickness(kg, sd),
                                 ThicknessMode(kg.nt.node_tree.thickness_mode));

  outline_output_reset();
  nodetree_surface(kg, sd, closure_rand);

#ifdef MAT_OUTLINE_SHELL
  const float shell_transparency = average(sd.transmittance);
#  ifdef MAT_OUTLINE_SHELL_BLENDED
  if (shell_transparency >= 1.0f) {
    gpu_discard_fragment();
    return;
  }
#  else
  if (hashed_transparency::should_discard(shell_transparency,
                                         uni.pipeline_buf.alpha_hash_scale,
                                         sampling.rng_1D_get(SAMPLING_TRANSPARENCY),
                                         sd.P))
  {
    gpu_discard_fragment();
    return;
  }
  /* Coverage already applied Alpha. A surviving sample is opaque, including volume resolve. */
  sd.emission *= safe_rcp(1.0f - shell_transparency);
  sd.transmittance = float3(0.0f);
#  endif
#endif

  /* Forward surfaces do not pass through the GBuffer combine, but still write the Normal pass. */
  [[resource_table]] RenderPassOutput &render_passes = resource_table_get(RenderPassOutput);
  render_passes.store_color(int2(frag_co.xy), uni.uniform_buf.render_pass.normal_id,
                           float4(sd.N, 1.0f));

  float3 radiance, transmittance;
  float3 nodetree_P = sd.P;
  sd.P = sd.forward_lighting_P;
  eevee::forward_lighting_eval(
      kg, sd, view, resource_id, sd.thickness, frag_co.xy, radiance, transmittance);

  if (pipe.use_lighting_nodes) [[static_branch]] {
    radiance += sd.light_accum.diffuse_light;
    radiance += sd.light_accum.glossy_light;
    radiance += sd.light_accum.transmission_light;
  }
  sd.P = nodetree_P;
  attenuate_outline(saturate(average(transmittance)));
  outline_output_flush(1.0f - saturate(average(transmittance)), resource_id);

  /* Volumetric resolve and compositing. */
  float2 uvs = frag_co.xy * uni.uniform_buf.volumes.main_view_extent_inv;
  VolumeResolveSample vol = volumes.resolve(float3(uvs, volume_depth));
  /* Removes the part of the volume scattering that has
   * already been added to the destination pixels by the opaque resolve.
   * Since we do that using the blending pipeline we need to account for material transmittance. */
  vol.scattering -= vol.scattering * sd.transmittance;
  radiance = radiance * vol.transmittance + vol.scattering;

  eObjectInfoFlag ob_flag = kg.object_infos_get(sd).flag;
  if (flag_test(ob_flag, OBJECT_HOLDOUT)) {
    sd.holdout = 1.0f - average(sd.transmittance);
    radiance *= 0.0f;
  }

  sd.holdout = saturate(sd.holdout);

  radiance *= 1.0f - sd.holdout;

  /* There can be 2 frame-buffer layout for forward transparency:
   * - Combined RGB radiance with Monochromatic transmittance.
   * - Channel split RGB radiance & RGB transmittance + Dedicated average alpha with holdout. */
  if (uni.pipeline_buf.use_monochromatic_transmittance) {
    frag_out.combined_r = float4(radiance.rgb, transmittance.r);
  }
  else {
    frag_out.combined_r = float4(radiance.r, 0.0f, 0.0f, transmittance.r);
    frag_out.combined_g = float4(radiance.g, 0.0f, 0.0f, transmittance.g);
    frag_out.combined_b = float4(radiance.b, 0.0f, 0.0f, transmittance.b);
    frag_out.combined_a = float4(sd.holdout, 0.0f, 0.0f, average(transmittance));
  }
  return;
}

/* Early fragment test is needed for render pass support for forward surfaces.
 * NOTE: This removes the possibility of using gl_FragDepth. Depth Offset materials use the
 * sibling entry-point below so they can keep late depth tests and write gl_FragDepth. */
[[fragment]] [[early_fragment_tests]]
void surf_forward([[resource_table]] KernelGlobals &kg,
                  [[resource_table]] PipelineConstants &pipe,
                  [[resource_table]] LightEvalIterator & lights,
                  [[resource_table]] LightprobeRenderData & lightprobes,
                  [[resource_table]] LightprobePlaneRenderData & lightprobe_planes,
                  [[resource_table]] const draw::View &views,
                  [[resource_table]] const draw::Model & models,
                  [[resource_table]] const draw::Infos & infos,
                  [[resource_table]] const UnifiedVolumeData &volumes,
                  [[resource_table]] const Uniform &uni,
                  [[resource_table]] const Sampling &sampling,
                  [[resource_table]] const UtilityTexture &util_tx,
                  [[in]] const VertOutCommon &interp,
                  [[in]] [[condition(is_curves)]] const VertOutCurves &curves_interp,
                  [[in]] [[condition(is_pointcloud)]] const VertOutPointcloud &ptcloud_interp,
                  [[frag_coord]] const float4 frag_co,
                  [[out]] SurfaceForwardFragOut &frag_out,
                  [[bary_coord]] [[condition(use_barycentric)]] const float3 bary_co,
                  [[front_facing]] const bool front_face)
{
  float out_depth = frag_co.z;
  material_surface_cull_discard(front_face);
  const ViewMatrices view = views.get(0);
  float3 barycentric_co = float3(0.0f);
  if (pipe.use_barycentric) [[static_branch]] {
    barycentric_co = bary_co;
  }

  ShadingData sd = init_globals(pipe, uni, interp, view, front_face, frag_co);
  sd.forward_lighting_P = sd.P;
  if (pipe.is_mesh) [[static_branch]] {
    init_globals_mesh(interp, sd, barycentric_co);
  }
  else if (pipe.is_curves) [[static_branch]] {
    init_globals_curves(interp, curves_interp, sd, view);
  }
  else if (pipe.is_pointcloud) [[static_branch]] {
    init_globals_pointcloud(ptcloud_interp, sd);
  }

  surf_forward_impl(kg, pipe, lights, lightprobes, lightprobe_planes, views, models, infos, volumes, uni, sampling, util_tx, interp, frag_co, frag_out, out_depth, sd);
}

[[fragment]]
void surf_forward_depth_offset([[resource_table]] KernelGlobals &kg,
                  [[resource_table]] PipelineConstants &pipe,
                  [[resource_table]] LightEvalIterator & lights,
                  [[resource_table]] LightprobeRenderData & lightprobes,
                  [[resource_table]] LightprobePlaneRenderData & lightprobe_planes,
                  [[resource_table]] const draw::View &views,
                  [[resource_table]] const draw::Model & models,
                  [[resource_table]] const draw::Infos & infos,
                  [[resource_table]] const UnifiedVolumeData &volumes,
                  [[resource_table]] const Uniform &uni,
                  [[resource_table]] const Sampling &sampling,
                  [[resource_table]] const UtilityTexture &util_tx,
                  [[in]] const VertOutCommon &interp,
                  [[in]] [[condition(is_curves)]] const VertOutCurves &curves_interp,
                  [[in]] [[condition(is_pointcloud)]] const VertOutPointcloud &ptcloud_interp,
                  [[frag_coord]] const float4 frag_co,
                  [[out]] SurfaceForwardFragOut &frag_out,
                  [[bary_coord]] [[condition(use_barycentric)]] const float3 bary_co,
                  [[front_facing]] const bool front_face,
                   [[frag_depth(any)]] float &out_depth)
{
  out_depth = frag_co.z;
  material_surface_cull_discard(front_face);
  const ViewMatrices view = views.get(0);
  float3 barycentric_co = float3(0.0f);
  if (pipe.use_barycentric) [[static_branch]] {
    barycentric_co = bary_co;
  }

  ShadingData sd = init_globals(pipe, uni, interp, view, front_face, frag_co);
  sd.forward_lighting_P = sd.P;
  if (pipe.is_mesh) [[static_branch]] {
    init_globals_mesh(interp, sd, barycentric_co);
  }
  else if (pipe.is_curves) [[static_branch]] {
    init_globals_curves(interp, curves_interp, sd, view);
  }
  else if (pipe.is_pointcloud) [[static_branch]] {
    init_globals_pointcloud(ptcloud_interp, sd);
  }

  surf_forward_impl(kg, pipe, lights, lightprobes, lightprobe_planes, views, models, infos, volumes, uni, sampling, util_tx, interp, frag_co, frag_out, out_depth, sd);
}

}  // namespace eevee
