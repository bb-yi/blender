/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Resource/type adaptation only. Sampling and PDF compensation live in the pinned kernels. */
#include "draw_view.bsl.hh"
#include "draw_model.bsl.hh"
#include "eevee_uniform.bsl.hh"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_light_lib.bsl.hh"

float3 drw_point_screen_to_world(float3 P)
{
  [[resource_table]] const draw::View &views = resource_table_get(draw::View);
  return views.get(0).point_screen_to_world(P);
}
float3 drw_world_incident_vector(float3 P)
{
  [[resource_table]] const draw::View &views = resource_table_get(draw::View);
  return views.get(0).world_incident_vector(P);
}
float sampling_rng_1D_get(eSamplingDimension dim)
{
  [[resource_table]] const eevee::Sampling &sampling = resource_table_get(eevee::Sampling);
  return sampling.rng_1D_get(dim);
}
ObjectInfos hwrt_object_infos_get(uint index)
{
  [[resource_table]] const draw::Infos &infos = resource_table_get(draw::Infos);
  return infos.get(index);
}

#ifdef HWRT_DIRECT_VISIBILITY
float3 hardware_direct_visibility(int2 texel, int layer)
{
  /* Unshadowed and GPU-extracted world lights retain the existing native visibility path. */
  return layer >= 0 ? texelFetch(hardware_rt_shadow_visibility_tx, int3(texel, layer), 0).rgb :
                      float3(1.0f);
}
#endif

#ifdef HWRT_DIRECT_EVALUATION
#include "eevee_closure.bsl.hh"
#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_light_eval.bsl.hh"

int2 shadow_dispatch_texel_fullres;
float3 direct_last_unshadowed;

float3 hardware_direct_evaluate(uint light_index, LightData light, bool directional,
                                ClosureUndetermined cl, float3 P, float3 Ng, float3 V,
                                uchar receiver_light_set)
{
  [[resource_table]] eevee::LightEvalData &srt = resource_table_get(eevee::LightEvalData);
  [[resource_table]] const UtilityTexture &util = resource_table_get(UtilityTexture);
  const int2 texel = shadow_dispatch_texel_fullres;
  const gbuffer::Header header = gbuffer::read_header(texel);
  eevee::light::EvalCtx<false> ctx;
  ctx.stack.cl[0] = closure_light_new(util, cl, V);
  ctx.P = P;
  ctx.Ng = Ng;
  ctx.V = V;
  ctx.texel = float2(texel) + 0.5f;
  ctx.thickness = gbuffer::read_thickness(header, texel);
  ctx.receiver_light_set = receiver_light_set;
  ctx.receiver_id = header.use_object_id() ? gbuffer::read_object_id(texel) : 0u;
  ctx.terminator_normal_offset = 0.0f;
  ctx.terminator_geometry_offset = 0.0f;
  if (header.use_object_id()) {
    ObjectInfos info = hwrt_object_infos_get(ctx.receiver_id);
    ctx.terminator_normal_offset = info.shadow_terminator_normal_offset;
    ctx.terminator_geometry_offset = info.shadow_terminator_geometry_offset;
  }
  /* Uses the same primary HWRT visibility atlas as the Nuru selection record. Reuse current
   * evaluation to retain NPR light policies, anisotropic closures and light-node textures. */
  ctx.light_eval_single(srt, light_index, light, directional);
  direct_last_unshadowed = ctx.stack.cl[0].light_unshadowed;
  return ctx.stack.cl[0].light_shadowed;
}
#endif
