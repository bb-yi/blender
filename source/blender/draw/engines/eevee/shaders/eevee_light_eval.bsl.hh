/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_bxdf_types.bsl.hh"
#include "eevee_light_iter.bsl.hh"
#include "eevee_light_lib.bsl.hh"
#include "eevee_hiz.bsl.hh"
#include "infos/eevee_common_infos.hh"
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

struct NPRShadowSurfaceData {
  [[legacy_info]] ShaderCreateInfo eevee_npr_shadow_surface_data;
  [[resource_table]] srt_t<HiZ> hiz;
};

struct LightEvalData {
  [[compilation_constant]] bool use_light_shader_texture_eval;
  [[compilation_constant]] bool use_light_shader_surfel_eval;
  [[compilation_constant]] bool use_npr_light_policy;

  [[resource_table]] srt_t<ShadowRenderData> shadow_data;
  [[resource_table]] srt_t<UtilityTexture> utility_tx;
  [[resource_table, condition(use_npr_light_policy)]] srt_t<NPRShadowSurfaceData> npr_surface;
  [[resource_table, condition(use_light_shader_texture_eval)]] srt_t<LightShaderEvalData>
      light_shader_data;
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

struct NPRShadowContext {
  float3 P;
  float3 Ng;
  float3 N;
  float2 texel;
  float2 terminator_offset;
  Thickness thickness;
  uint receiver_id;
  bool transmission_path;
  bool is_translucent_with_thickness;
};

float npr_shadow_seeded([[resource_table]] ShadowRenderData &srd,
                        LightData light,
                        bool is_directional,
                        NPRShadowContext ctx,
                        uint type,
                        int ray_count,
                        int step_count,
                        float3 random_shadow,
                        float2 random_pcf)
{
  if (type == 1u || type == 3u) {
    float3 classified = shadow_caster_classification_seeded_ex(
        srd, light, is_directional, ctx.receiver_id, ctx.P, ctx.Ng, ctx.N,
        ctx.terminator_offset.x, ctx.terminator_offset.y, ray_count, step_count,
        random_shadow, random_pcf, ctx.transmission_path, ctx.thickness);
    return 1.0f - (type == 1u ? classified.z : classified.y);
  }
  return shadow_eval_seeded(srd, light, is_directional, ctx.transmission_path,
                            ctx.is_translucent_with_thickness, ctx.thickness, ctx.P, ctx.Ng, ctx.N,
                            ctx.terminator_offset.x, ctx.terminator_offset.y, ray_count, step_count,
                            random_shadow, random_pcf);
}

float npr_shadow_soft_spatial([[resource_table]] LightEvalData &srt,
                              LightData light,
                              bool is_directional,
                              NPRShadowContext ctx,
                              int samples,
                              float center_visibility,
                              float3 frame_3d,
                              float2 frame_2d)
{
  [[resource_table]] ShadowRenderData &srd = srt.shadow_data;
  [[resource_table]] const Uniform &uni = srd.uniforms;
  [[resource_table]] const draw::View &views = srd.views;
  [[resource_table]] const NPRShadowSurfaceData &surface = srt.npr_surface;
  [[resource_table]] const HiZ &hiz = surface.hiz;
  const auto &object_id_tx = sampler_get(eevee_npr_shadow_surface_data, npr_shadow_object_id_tx);
  const auto &normal_tx = sampler_get(eevee_npr_shadow_surface_data, npr_shadow_normal_tx);
  float penumbra = saturate(1.0f - abs(center_visibility * 2.0f - 1.0f));
  int2 size = textureSize(object_id_tx, 0);
  if (penumbra <= 1e-3f || any(lessThanEqual(size, int2(1)))) {
    return center_visibility;
  }
  int2 center_texel = clamp(int2(ctx.texel), int2(0), size - 1);
  uint center_id = texelFetch(object_id_tx, center_texel, 0).r;
  /* Blended surfaces need not exist in the prepass. Never filter their shadows using the
   * opaque surface behind them; the actual multi-ray soft-shadow core is still evaluated. */
  if (center_id == 0u || (center_id & 0xFFFFu) != (ctx.receiver_id & 0xFFFFu)) {
    return center_visibility;
  }
  float2 center_uv = (float2(center_texel) + 0.5f) / float2(size);
  float center_depth = textureLod(hiz.hiz_tx, center_uv, 0.0f).r;
  if (center_depth >= 1.0f) {
    return center_visibility;
  }
  const ViewMatrices view = views.get(0);
  float3 center_P = view.point_screen_to_world(float3(center_uv, center_depth));
  float3 center_N = texelFetch(normal_tx, center_texel, 0).xyz * 2.0f - 1.0f;
  center_N = length_squared(center_N) > 1e-16f ? normalize(center_N) : ctx.Ng;
  float radius_factor = saturate((float(samples) - 4.0f) / 28.0f);
  float radius = mix(1.5f, 3.0f, radius_factor) * mix(0.75f, 1.0f, penumbra);
  float gaussian = 1.442695041f * 1.5f / square(max(radius, 1.0f));
  int tap_count = clamp(4 + int(radius_factor * 4.0f + 0.5f), 4, 8);
  int ray_count = samples >= 24 ? 2 : 1;
  int step_count = max(uni.uniform_buf.shadow.step_count, 6);
  float visibility_sum = center_visibility;
  float weight_sum = 1.0f;
  for (int tap = 0; tap < 8; tap++) {
    if (tap >= tap_count) {
      break;
    }
    float angle = 6.28318530718f * (float(tap) / float(tap_count) + frame_2d.x);
    float tap_radius = radius * mix(0.65f, 1.0f, fract(frame_2d.y + float(tap) * 0.381966f));
    float2 offset_px = float2(cos(angle), sin(angle)) * tap_radius;
    int2 texel = clamp(int2((center_uv + offset_px / float2(size)) * float2(size)),
                        int2(0), size - 1);
    if (texelFetch(object_id_tx, texel, 0).r != center_id) {
      continue;
    }
    float2 uv = (float2(texel) + 0.5f) / float2(size);
    float depth = textureLod(hiz.hiz_tx, uv, 0.0f).r;
    if (depth >= 1.0f) {
      continue;
    }
    float3 sample_P = view.point_screen_to_world(float3(uv, depth));
    float3 sample_N = texelFetch(normal_tx, texel, 0).xyz * 2.0f - 1.0f;
    sample_N = length_squared(sample_N) > 1e-16f ? normalize(sample_N) : center_N;
    float plane_distance = dot(center_N, sample_P - center_P);
    float weight = exp2(-gaussian * dot(offset_px, offset_px)) *
                   exp2(-1200.0f * square(plane_distance)) *
                   square(square(saturate(dot(center_N, sample_N))));
    if (weight <= 1e-5f) {
      continue;
    }
    NPRShadowContext sample_ctx = ctx;
    sample_ctx.P = sample_P;
    sample_ctx.Ng = sample_N;
    sample_ctx.N = sample_N;
    float2 ray_tap = fract(hammersley_2d(tap, tap_count) + frame_3d.xy + uv);
    float2 pcf_tap = fract(hammersley_2d(tap, tap_count) + frame_2d + uv.yx +
                            float2(0.19f, 0.61f));
    float z = fract(van_der_corput_radical_inverse(uint(tap) * 747796405u + 2891336453u) +
                    frame_3d.z + dot(uv, float2(0.25f, 0.5f)));
    visibility_sum += npr_shadow_seeded(srd, light, is_directional, sample_ctx, 2u,
                                        ray_count, step_count, float3(ray_tap, z), pcf_tap) * weight;
    weight_sum += weight;
  }
  float filtered = saturate(visibility_sum / max(weight_sum, 1e-6f));
  return mix(center_visibility, filtered, saturate(penumbra * 1.75f));
}

float npr_shadow_visibility([[resource_table]] LightEvalData &srt,
                            LightData light,
                            bool is_directional,
                            float3 light_vector,
                            NPRShadowContext ctx,
                            ClosureNPRLightPolicy policy)
{
  uint type = closure_npr_policy_shadow_type(policy);
  if (type == 0u || policy.strength <= 0.0f || light.tilemap_index == LIGHT_NO_SHADOW) {
    return 1.0f;
  }
  [[resource_table]] ShadowRenderData &srd = srt.shadow_data;
  [[resource_table]] const Uniform &uni = srd.uniforms;
  float3 frame_3d = float3(0.0f);
  float2 frame_2d = float2(0.0f);
  float3 random_shadow = float3(0.5f);
  float2 random_pcf = float2(0.0f);
  if (srd.shadow_random) [[static_branch]] {
    [[resource_table]] const Sampling sampling = srd.sampling;
    [[resource_table]] const UtilityTexture util = srd.util_tx;
    float3 blue_noise = util.fetch(ctx.texel, UTIL_BLUE_NOISE_LAYER).rgb;
    frame_3d = sampling.rng_3D_get(SAMPLING_SHADOW_U);
    frame_2d = sampling.rng_2D_get(SAMPLING_SHADOW_X);
    random_shadow = fract(blue_noise + frame_3d);
    random_pcf = fract(blue_noise.xy + frame_2d);
  }
  if (closure_npr_policy_quality(policy) == 1u) {
    float visibility = npr_shadow_seeded(srd, light, is_directional, ctx, type,
                                         uni.uniform_buf.shadow.ray_count,
                                         uni.uniform_buf.shadow.step_count,
                                         random_shadow, random_pcf);
    return mix(1.0f, visibility, policy.strength);
  }
  /* Same budget and sequence as Shader Info Soft Filtered. Classification components are
   * averaged per ray, never reconstructed from a combined screen-space shadow value. */
  int samples = clamp(int(closure_npr_policy_samples(policy)), 1, 32);
  int eval_count = clamp(int(ceil(sqrt(float(samples)) * 1.5f)), 2, 8);
  int ray_count = clamp(int(ceil(float(samples) / float(eval_count))), 1, 4);
  int step_count = max(uni.uniform_buf.shadow.step_count, 6);
  float visibility = 0.0f;
  for (int tap = 0; tap < 8; tap++) {
    if (tap >= eval_count) {
      break;
    }
    float2 ray_tap = fract(hammersley_2d(tap, eval_count) + frame_3d.xy);
    float2 pcf_tap = fract(hammersley_2d(tap, eval_count) + frame_2d + float2(0.37f, 0.13f));
    float z = fract(van_der_corput_radical_inverse(uint(tap) * 1103515245u + 12345u) + frame_3d.z);
    visibility += npr_shadow_seeded(srd, light, is_directional, ctx, type,
                                    ray_count, step_count, float3(ray_tap, z), pcf_tap);
  }
  visibility = saturate(visibility / float(eval_count));
  if (type == 2u) {
    visibility = npr_shadow_soft_spatial(
        srt, light, is_directional, ctx, samples, visibility, frame_3d, frame_2d);
    float seed = dot(ctx.P, float3(0.06711056f, 0.00583715f, 0.049981f)) +
                 dot(light_vector, float3(0.03125f, 0.125f, 0.2109375f));
    float noise = interleaved_gradient_noise(ctx.texel, seed, 0.0f) - 0.5f;
    visibility = saturate(visibility + noise * saturate(visibility * (1.0f - visibility) * 4.0f) /
                                         float(max(eval_count * ray_count * 2, 1)));
  }
  return mix(1.0f, visibility, policy.strength);
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
  uint receiver_id;
  float terminator_normal_offset;
  float terminator_geometry_offset;
  int light_shader_surfel_index;
  int light_shader_surfel_len;

  void eval_closure([[resource_table]] LightEvalData &srt,
                    LightData light,
                    LightVector lv,
                    LightVertices vertices,
                    ClosureLight &cl,
                    float attenuation,
                    float native_shadow,
                    bool light_shader_no_distance_falloff,
                    bool is_directional)
  {
    float visibility = native_shadow;
    if (srt.use_npr_light_policy) [[static_branch]] {
      if (closure_npr_policy_enabled(cl.npr_policy)) {
        /* This is native direct illumination, including the SSS back-light term. Custom NPR
         * M/A is already scaled in the node and bypasses this native result at resolve time. */
        attenuation *= cl.npr_policy.direct_gain;
        if (attenuation <= 0.0f) {
          return;
        }
        uint group = closure_npr_policy_group(cl.npr_policy);
        if (group != 16383u && int(group) != light.lightgroup_id) {
          return;
        }
        NPRShadowContext shadow_ctx;
        shadow_ctx.P = P;
        shadow_ctx.Ng = Ng;
        shadow_ctx.N = cl.shadow_N;
        shadow_ctx.texel = texel;
        shadow_ctx.terminator_offset = float2(terminator_normal_offset, terminator_geometry_offset);
        shadow_ctx.thickness = thickness;
        shadow_ctx.receiver_id = receiver_id;
        shadow_ctx.transmission_path = is_transmission;
        shadow_ctx.is_translucent_with_thickness =
            is_transmission && cl.type == LIGHT_TRANSLUCENT_WITH_THICKNESS;
        visibility = npr_shadow_visibility(srt, light, is_directional, lv.L,
                                            shadow_ctx, cl.npr_policy);
      }
    }
    [[resource_table]] const UtilityTexture &util = srt.utility_tx;
    eval_single_closure(util.utility_tx, light, lv, vertices, cl, V, attenuation, visibility,
                         light_shader_no_distance_falloff, is_directional);
  }

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
    bool needs_native_shadow = true;
    if (srt.use_npr_light_policy) [[static_branch]] {
      needs_native_shadow = false;
      for (uint i = 0u; i < 3u; i++) [[unroll]] {
        if (is_transmission) [[static_branch]] {
          if (srt.light_closure_eval_count_transmit > i) [[static_branch]] {
            needs_native_shadow = needs_native_shadow ||
                                    !closure_npr_policy_enabled(stack.cl[i].npr_policy);
          }
        }
        else {
          if (srt.light_closure_eval_count_reflect > i) [[static_branch]] {
            needs_native_shadow = needs_native_shadow ||
                                    !closure_npr_policy_enabled(stack.cl[i].npr_policy);
          }
        }
      }
    }
    if (light.tilemap_index != LIGHT_NO_SHADOW && needs_native_shadow) {
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

    for (uint i = 0u; i < 3; i++) [[unroll]] {
      if (is_transmission) [[static_branch]] {
        if (srt.light_closure_eval_count_transmit > i) [[static_branch]] {
          this->eval_closure(srt,
                              light,
                              lv,
                              light_shape_vertices,
                              stack.cl[i],
                              attenuation,
                              shadow,
                              light_shader_no_distance_falloff,
                              is_directional);
        }
      }
      else {
        if (srt.light_closure_eval_count_reflect > i) [[static_branch]] {
          this->eval_closure(srt,
                              light,
                              lv,
                              light_shape_vertices,
                              stack.cl[i],
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
  ctx_tr.receiver_id = ctx.receiver_id;
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
