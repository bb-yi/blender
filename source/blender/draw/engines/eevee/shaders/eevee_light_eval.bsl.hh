/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_bxdf_types.bsl.hh"
#include "eevee_light_iter.bsl.hh"
#include "eevee_light_lib.bsl.hh"
#include "eevee_shadow.bsl.hh"
#include "eevee_shadow_tracing.bsl.hh"
#include "eevee_thickness_lib.bsl.hh"
#include "gpu_shader_utildefines_lib.glsl"

#if !defined(SRT_CONSTANT_light_closure_eval_count_reflect)
#  define SRT_CONSTANT_light_closure_eval_count_reflect 0
#endif
#if !defined(SRT_CONSTANT_light_closure_eval_count_transmit)
#  define SRT_CONSTANT_light_closure_eval_count_transmit 0
#endif

#ifdef GLSL_CPP_STUBS
#  define LIGHT_STACK_SIZE_REFLECT 3
#elif SRT_CONSTANT_light_closure_eval_count_reflect == 0
#  define LIGHT_STACK_SIZE_REFLECT 1 /* Avoid compilation error. */
#else
#  define LIGHT_STACK_SIZE_REFLECT SRT_CONSTANT_light_closure_eval_count_reflect
#endif

#ifdef GLSL_CPP_STUBS
#  define LIGHT_STACK_SIZE_TRANSMIT 3
#elif SRT_CONSTANT_light_closure_eval_count_transmit == 0
#  define LIGHT_STACK_SIZE_TRANSMIT 1 /* Avoid compilation error. */
#else
#  define LIGHT_STACK_SIZE_TRANSMIT SRT_CONSTANT_light_closure_eval_count_transmit
#endif

namespace eevee {

struct LightEvalData {
  [[resource_table]] srt_t<ShadowRenderData> shadow_data;
  [[resource_table]] srt_t<UtilityTexture> utility_tx;
  [[resource_table, condition(use_light_shader_texture_eval)]] srt_t<LightShaderEvalData> light_shader_data;

  [[compilation_constant]] bool use_light_shader_texture_eval;
  [[compilation_constant]] bool use_light_shader_surfel_eval;
  [[resource_table, condition(use_light_shader_surfel_eval)]] srt_t<LightShaderSurfelEvalData>
      light_shader_surfel_data;
  [[compilation_constant]] int light_closure_eval_count_reflect;
  [[compilation_constant]] int light_closure_eval_count_transmit;
};

namespace light {

template<bool is_transmission> struct ClosureStack {};

template<> struct ClosureStack<false> {
  ClosureLight cl[LIGHT_STACK_SIZE_REFLECT];
};

template<> struct ClosureStack<true> {
  ClosureLight cl[LIGHT_STACK_SIZE_TRANSMIT];
};

float power_get(LightData light, LightingType type)
{
  /* Mask anything above 3. See LIGHT_TRANSLUCENT_WITH_THICKNESS. */
  return light.power[type & 3u];
}

float light_shader_shape_radiance_get(LightData light)
{
  if (light.type == LIGHT_RECT || light.type == LIGHT_ELLIPSE) {
    float area = light.area().size.x * light.area().size.y * 4.0f;
    if (light.type == LIGHT_ELLIPSE) {
      area *= M_PI * 0.25f;
    }
    return M_1_PI / area;
  }

  if (is_sphere_light(light.type)) {
    float area = 4.0f * M_PI * square(light.local().local.shape_radius);
    return 1.0f / (area * M_PI);
  }

  if (is_sun_light(light.type)) {
    float inv_sin_sq = 1.0f + 1.0f / square(light.sun().shape_radius);
    return M_1_PI * inv_sin_sq;
  }

  return 1.0f;
}

float light_shader_point_radiance_get(LightData light)
{
  if (light.type == LIGHT_RECT || light.type == LIGHT_ELLIPSE) {
    float area = light.area().size.x * light.area().size.y * 4.0f;
    float tmp = M_PI_2 / (M_PI_2 + sqrt(area));
    float mrp_scaling = tmp + (1.0f - tmp) * M_1_PI;
    return M_1_PI * mrp_scaling;
  }

  if (is_sphere_light(light.type)) {
    return 1.0f / (4.0f * M_PI);
  }

  if (is_sun_light(light.type)) {
    return 1.0f;
  }

  return 1.0f;
}

float light_shader_no_distance_power_get(LightData light, LightingType type)
{
  if (is_sun_light(light.type)) {
    return power_get(light, type);
  }

  float shape_power = light_shader_shape_radiance_get(light);
  if (shape_power <= 1e-16f) {
    return 0.0f;
  }
  return power_get(light, type) * (light_shader_point_radiance_get(light) / shape_power);
}

float light_shader_distance_falloff_get(LightData light, LightVector lv, const bool is_directional)
{
  if (is_directional) {
    return 1.0f;
  }

  float shape_power = light_shader_shape_radiance_get(light);
  if (shape_power <= 1e-16f) {
    return 0.0f;
  }
  return light_point_light(light, is_directional, lv) *
         (light_shader_point_radiance_get(light) / shape_power);
}

void light_shader_eval_apply(LightData &light,
                             LightVector lv,
                             const bool is_directional,
                             float4 light_shader,
                             float &attenuation,
                             bool &light_shader_no_distance_falloff)
{
  light.color = light_shader.rgb;
  attenuation = light_attenuation_common(light, is_directional, lv.L) * light_shader.a;
  light_shader_no_distance_falloff = true;
  if (!is_directional) {
    attenuation *= light_influence_cutoff(lv.dist,
                                          light.local().local.influence_radius_invsqr_surface);
  }
}

float light_shader_no_distance_ltc(sampler2DArray util_tx,
                                   LightData light,
                                   LightVector lv,
                                   LightVertices vertices,
                                   ClosureLight cl,
                                   float3 V,
                                   const bool is_directional)
{
  float ltc_result = light_ltc(util_tx, light, cl.N, V, lv, cl.ltc_mat, vertices);
  return ltc_result / max(light_shader_distance_falloff_get(light, lv, is_directional), 1e-8f);
}

bool light_linking_affects_receiver(uint2 light_set_membership, uchar receiver_light_set)
{
  return bitmask64_test(light_set_membership, receiver_light_set);
}

void eval_single_closure(sampler2DArray util_tx,
                         LightData light,
                         LightVector lv,
                         LightVertices vertices,
                         ClosureLight &cl,
                         float3 V,
                         float attenuation,
                         float shadow,
                         const bool light_shader_no_distance_falloff,
                         const bool is_directional)
{
  attenuation *= light_shader_no_distance_falloff ?
                     light_shader_no_distance_power_get(light, cl.type) :
                     power_get(light, cl.type);
  if (attenuation < 1e-30f) {
    return;
  }
  float ltc_result = light_shader_no_distance_falloff ?
                         light_shader_no_distance_ltc(
                             util_tx, light, lv, vertices, cl, V, is_directional) :
                         light_ltc(util_tx, light, cl.N, V, lv, cl.ltc_mat, vertices);
  float3 out_radiance = light.color * ltc_result;
  float visibility = shadow * attenuation;
  cl.light_shadowed += visibility * out_radiance;
  cl.light_unshadowed += attenuation * out_radiance;
}

template<bool is_transmission> struct EvalCtx {
  ClosureStack<is_transmission> stack;

  float3 P;
  float3 Ng;
  float3 V;
  float2 texel;
  Thickness thickness;
  uchar receiver_light_set;
  float terminator_normal_offset;
  float terminator_geometry_offset;
  int light_shader_surfel_index;
  int light_shader_surfel_len;

  void light_eval_single([[resource_table]] LightEvalData &srt,
                         uint l_idx,
                         LightData light,
                         const bool is_directional)
  {
    [[resource_table]] ShadowRenderData &srd = srt.shadow_data;
    [[resource_table]] Uniform &uni = srd.uniforms;

    if (!light_linking_affects_receiver(light.light_set_membership, receiver_light_set)) {
      return;
    }

#if defined(SPECIALIZED_SHADOW_PARAMS) || defined(SRT_CONSTANT_shadow_ray_count)
    int ray_count = shadow_ray_count;
    int ray_step_count = shadow_ray_step_count;
#else
    int ray_count = uni.uniform_buf.shadow.ray_count;
    int ray_step_count = uni.uniform_buf.shadow.step_count;
#endif

    LightVector lv = light_vector_get(light, is_directional, P);

    /* TODO(fclem): Get rid of this special case. */
    bool is_translucent_with_thickness = is_transmission &&
                                         (stack.cl[0].type == LIGHT_TRANSLUCENT_WITH_THICKNESS);

    float attenuation = light_attenuation_surface(light, is_directional, lv);
    float facing = light_attenuation_facing(light, lv.L, lv.dist, stack.cl[0].N, is_transmission);
    bool light_shader_no_distance_falloff = false;

    if (!is_transmission) [[static_branch]] {
      if (srt.use_light_shader_texture_eval) [[static_branch]] {
        [[resource_table]] const LightShaderEvalData &light_shader_data = srt.light_shader_data;
        int light_shader_index = light_shader_data.light_shader_index_buf[l_idx];
        int light_shader_uniform_index = (light_shader_index < -1) ? -light_shader_index - 2 : -1;
        if (light_shader_uniform_index >= 0) {
          float4 light_shader =
              light_shader_data.light_shader_uniform_buf[light_shader_uniform_index];
          light_shader_eval_apply(light,
                                  lv,
                                  is_directional,
                                  light_shader,
                                  attenuation,
                                  light_shader_no_distance_falloff);
        }
        else if (light_shader_index >= 0) {
          float4 light_shader = texelFetch(light_shader_data.light_shader_tx,
                                           int3(int2(texel), light_shader_index),
                                           0);
          light_shader_eval_apply(light,
                                  lv,
                                  is_directional,
                                  light_shader,
                                  attenuation,
                                  light_shader_no_distance_falloff);
        }
      }
      if (srt.use_light_shader_surfel_eval) [[static_branch]] {
        [[resource_table]] const LightShaderSurfelEvalData &light_shader_data =
            srt.light_shader_surfel_data;
        int light_shader_index = light_shader_data.surfel_light_shader_index_buf[l_idx];
        int light_shader_uniform_index = (light_shader_index < -1) ? -light_shader_index - 2 : -1;
        if (light_shader_uniform_index >= 0) {
          float4 light_shader =
              light_shader_data.surfel_light_shader_uniform_buf[light_shader_uniform_index];
          light_shader_eval_apply(light,
                                  lv,
                                  is_directional,
                                  light_shader,
                                  attenuation,
                                  light_shader_no_distance_falloff);
        }
        else if (light_shader_index >= 0) {
          int surfel_offset = light_shader_index * light_shader_surfel_len +
                              light_shader_surfel_index;
          float4 light_shader = light_shader_data.surfel_light_shader_buf[surfel_offset];
          light_shader_eval_apply(light,
                                  lv,
                                  is_directional,
                                  light_shader,
                                  attenuation,
                                  light_shader_no_distance_falloff);
        }
      }
    }

    if (!is_translucent_with_thickness) {
      /* Only do attenuation for this case, since we integrate the whole sphere for translucency.
       * Moreover, stack.cl[0].N is overwritten for is_translucent_with_thickness. */
      attenuation *= facing;
    }

    if (attenuation < LIGHT_ATTENUATION_THRESHOLD) {
      return;
    }

    float shadow = 1.0f;
    if (light.tilemap_index != LIGHT_NO_SHADOW) {
      shadow = shadow_eval(srd,
                           light,
                           is_directional,
                           is_transmission,
                           is_translucent_with_thickness,
                           texel,
                           thickness,
                           P,
                           Ng,
                           stack.cl[0].N,
                           terminator_normal_offset,
                           terminator_geometry_offset,
                           ray_count,
                           ray_step_count);
    }

    if (is_translucent_with_thickness) {
      /* This makes the LTC compute the solid angle of the light (still with the cosine term
       * applied but that still works great enough in practice). */
      stack.cl[0].N = lv.L;
      /* Adjust power because of the second lambertian distribution. */
      attenuation *= M_1_PI;
    }

    LightVertices light_shape_vertices = light_shape_corners(light, lv);

    [[resource_table]] const UtilityTexture &util = srt.utility_tx;
    const auto &util_tx = util.utility_tx;

    for (uint i = 0u; i < 3; i++) [[unroll]] {
      if (is_transmission) [[static_branch]] {
        if (srt.light_closure_eval_count_transmit > i) [[static_branch]] {
          eval_single_closure(util_tx,
                              light,
                              lv,
                              light_shape_vertices,
                              stack.cl[i],
                              V,
                              attenuation,
                              shadow,
                              light_shader_no_distance_falloff,
                              is_directional);
        }
      }
      else {
        if (srt.light_closure_eval_count_reflect > i) [[static_branch]] {
          eval_single_closure(util_tx,
                              light,
                              lv,
                              light_shape_vertices,
                              stack.cl[i],
                              V,
                              attenuation,
                              shadow,
                              light_shader_no_distance_falloff,
                              is_directional);
        }
      }
    }
  }

  void eval_directional([[resource_table]] LightEvalData &srt, uint l_idx, LightData light)
  {
    light_eval_single(srt, l_idx, light, true);
  }

  void eval_local([[resource_table]] LightEvalData &srt, uint l_idx, LightData light)
  {
    light_eval_single(srt, l_idx, light, false);
  }
};

template struct EvalCtx<true>;
template struct EvalCtx<false>;

template void foreach_visible<EvalCtx<true>, LightEvalData>(
    const LightRenderData &, float2, float, EvalCtx<true> &, LightEvalData &);
template void foreach_visible<EvalCtx<false>, LightEvalData>(
    const LightRenderData &, float2, float, EvalCtx<false> &, LightEvalData &);

/* NOTE: Doesn't init the closure stack. */
EvalCtx<true> init_from_reflect_ctx(EvalCtx<false> ctx)
{
  EvalCtx<true> ctx_tr;
  ctx_tr.P = ctx.P;
  ctx_tr.Ng = ctx.Ng;
  ctx_tr.V = ctx.V;
  ctx_tr.texel = ctx.texel;
  ctx_tr.thickness = ctx.thickness;
  ctx_tr.receiver_light_set = ctx.receiver_light_set;
  ctx_tr.terminator_normal_offset = ctx.terminator_normal_offset;
  ctx_tr.terminator_geometry_offset = ctx.terminator_geometry_offset;
  ctx_tr.light_shader_surfel_index = ctx.light_shader_surfel_index;
  ctx_tr.light_shader_surfel_len = ctx.light_shader_surfel_len;
  return ctx_tr;
}

}  // namespace light

struct LightEvalIterator {
  [[resource_table]] srt_t<LightEvalData> inner;
  [[resource_table]] srt_t<LightRenderData> light_data;

  void eval_reflection(light::EvalCtx<false> &ctx, float vPz)
  {
    [[resource_table]] LightEvalData &srt = inner;
    if (srt.light_closure_eval_count_reflect > 0) [[static_branch]] {
      light::foreach_visible(light_data, ctx.texel, vPz, ctx, srt);
    }
  }

  void eval_transmission(light::EvalCtx<true> &ctx, float vPz)
  {
    [[resource_table]] LightEvalData &srt = inner;
    if (srt.light_closure_eval_count_transmit > 0) [[static_branch]] {
      light::foreach_visible(light_data, ctx.texel, vPz, ctx, srt);
    }
  }
};

}  // namespace eevee
