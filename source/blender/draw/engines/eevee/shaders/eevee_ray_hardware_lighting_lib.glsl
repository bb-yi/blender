/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/* Nuru c603d408e9f3 lighting compatibility with Blender 5.2 resource tables.
 * The transport/composition stays in the pinned shader; only resource and type APIs live here. */
#include "draw_model.bsl.hh"
#include "draw_view.bsl.hh"
#include "eevee_closure.bsl.hh"
#include "eevee_light_lib.bsl.hh"
#include "eevee_lightprobe.bsl.hh"
#include "eevee_shadow_tracing.bsl.hh"
#include "eevee_uniform.bsl.hh"

int2 shadow_dispatch_texel_fullres;
bool shadow_dispatch_use_hardware_rt = false;
bool shadow_dispatch_allow_transmission_hardware_rt = false;
bool shadow_dispatch_force_unshadowed = false;
#define HWRT_SHADOW_VISIBILITY_MAIN_HIT 0
#define HWRT_SHADOW_VISIBILITY_LAYERED_RECEIVER 1
#define HWRT_SHADOW_VISIBILITY_TRANSMISSION_RECEIVER 2
int shadow_dispatch_visibility_source = HWRT_SHADOW_VISIBILITY_MAIN_HIT;

float3 hardware_rt_shadow_visibility_fetch(int2 texel, int layer)
{
  /* Nuru: RGB visibility carries transparent-shadow attenuation (e.g. tint through glass). */
  if (shadow_dispatch_visibility_source == HWRT_SHADOW_VISIBILITY_LAYERED_RECEIVER) {
    return texelFetch(
               hardware_layered_receiver_rt_shadow_visibility_tx, int3(texel, layer), 0)
        .rgb;
  }
  if (shadow_dispatch_visibility_source == HWRT_SHADOW_VISIBILITY_TRANSMISSION_RECEIVER) {
    return texelFetch(
               hardware_transmission_receiver_rt_shadow_visibility_tx, int3(texel, layer), 0)
        .rgb;
  }
  return texelFetch(hardware_rt_shadow_visibility_tx, int3(texel, layer), 0).rgb;
}

#define SHADOW_DISPATCH_HARDWARE_VISIBILITY_FETCH(_texel, _layer) \
  hardware_rt_shadow_visibility_fetch((_texel), (_layer))
#define SHADOW_DISPATCH_ALLOW_TRANSMISSION_HARDWARE_RT \
  shadow_dispatch_allow_transmission_hardware_rt
#define SHADOW_DISPATCH_FORCE_UNSHADOWED shadow_dispatch_force_unshadowed

float3 drw_point_screen_to_world(float3 P)
{
  [[resource_table]] const draw::View &views = resource_table_get(draw::View);
  return views.get(0).point_screen_to_world(P);
}

float3 drw_point_world_to_screen(float3 P)
{
  [[resource_table]] const draw::View &views = resource_table_get(draw::View);
  return views.get(0).point_world_to_screen(P);
}

float4 drw_point_world_to_homogenous(float3 P)
{
  [[resource_table]] const draw::View &views = resource_table_get(draw::View);
  return views.get(0).point_world_to_homogenous(P);
}

float3 drw_world_incident_vector(float3 P)
{
  [[resource_table]] const draw::View &views = resource_table_get(draw::View);
  return views.get(0).world_incident_vector(P);
}

Thickness hardware_thickness(float value)
{
  return Thickness::from(abs(value), value > 0.0f ? ThicknessMode::Sphere : ThicknessMode::Slab);
}

float hardware_thickness_value(Thickness value)
{
  return value.mode() == ThicknessMode::Sphere ? value.value() : -value.value();
}

float hardware_thickness_unpack(float value)
{
  return hardware_thickness_value(gbuffer::thickness_unpack(value));
}

ObjectInfos hardware_object_infos_get(uint index)
{
  [[resource_table]] const draw::Infos &infos = resource_table_get(draw::Infos);
  return infos.get(index);
}

int2 raytrace_texel_to_fullres(int2 texel, int scale, int denominator, int2 offset)
{
  return texel * scale + offset;
}

int2 raytrace_representative_fullres_texel(int2 texel, int scale, int denominator, int2 offset)
{
  return raytrace_texel_to_fullres(texel, scale, denominator, offset);
}

float sampling_rng_1D_get(eSamplingDimension dim)
{
  [[resource_table]] const eevee::Sampling &sampling = resource_table_get(eevee::Sampling);
  return sampling.rng_1D_get(dim);
}

ClosureLight closure_light_new(ClosureUndetermined cl, float3 V, float thickness)
{
  [[resource_table]] const UtilityTexture &util = resource_table_get(UtilityTexture);
  return closure_light_new(util, cl, V, hardware_thickness(thickness));
}

ClosureLight closure_light_new(ClosureUndetermined cl, float3 V)
{
  [[resource_table]] const UtilityTexture &util = resource_table_get(UtilityTexture);
  return closure_light_new(util, cl, V);
}

eevee::LightProbeSample lightprobe_sample(float2 texel, float3 P, float3 Ng, float3 V)
{
  [[resource_table]] const eevee::LightprobeRenderData &probes =
      resource_table_get(eevee::LightprobeRenderData);
  return probes.load(texel, P, Ng, V);
}

bool lightprobe_uses_world(eevee::LightProbeSample samp)
{
  [[resource_table]] const eevee::LightprobeSphereRenderData &spheres =
      resource_table_get(eevee::LightprobeSphereRenderData);
  return spheres.lightprobe_sphere_buf[samp.spherical_id].influence_scale == 0.0f;
}

float3 lightprobe_eval_with_direction(eevee::LightProbeSample samp,
                                     ClosureUndetermined cl,
                                     float3 P,
                                     float3 V,
                                     float thickness,
                                     float3 direction)
{
  [[resource_table]] const eevee::LightprobeSphereRenderData &spheres =
      resource_table_get(eevee::LightprobeSphereRenderData);
  LightProbeRay ray = bxdf_lightprobe_ray(cl, P, V, hardware_thickness(thickness));
  float lod = eevee::lightprobe::sphere::roughness_to_lod(ray.perceptual_roughness);
  float fac = eevee::lightprobe::sphere::roughness_to_mix_fac(ray.perceptual_roughness);
  direction = safe_normalize(direction);
  float3 cube = spheres.spherical_sample_normalized_with_parallax(samp, P, direction, lod);
  return mix(cube, samp.volume_irradiance.evaluate_lambert(direction).rgb, fac);
}

float3 lightprobe_eval(eevee::LightProbeSample samp, ClosureUndetermined cl,
                      float3 P, float3 V, float thickness)
{
  LightProbeRay ray = bxdf_lightprobe_ray(cl, P, V, hardware_thickness(thickness));
  return lightprobe_eval_with_direction(samp, cl, P, V, thickness, ray.dominant_direction);
}

float3 lightprobe_eval_direction(eevee::LightProbeSample samp, float3 P, float3 L, float inv_pdf)
{
  [[resource_table]] const eevee::LightprobeSphereRenderData &spheres =
      resource_table_get(eevee::LightprobeSphereRenderData);
  /* Nuru uses inverse PDF, not the newer perceptual-roughness argument. */
  return spheres.spherical_sample_normalized_with_parallax(
      samp, P, L, saturate((2.0f * M_PI) * inv_pdf) * 2.0f);
}

struct ClosureLightStack {
  ClosureLight cl[1];
};

LightData hardware_light_get(uint index)
{
  /* 5.2's sync buffer is suns-first; Nuru iterates the unculled lights locals-first. */
  uint locals = uint(hardware_local_lights_len);
  uint suns = uint(hardware_lights_len) - locals;
  uint source_index = index < locals ? index + suns : index - locals;
  LightData light = hardware_light_buf[source_index];
  if (light.color.r < 0.0f) {
    /* Match the native culling pass's world-sun expansion without camera-culling local lights. */
    LightData extracted = hardware_sunlight_buf[min(source_index, 1u)];
    float3 direction = extracted.object_to_world.z_axis();
    bool valid = !any(isnan(extracted.color)) && !any(isinf(extracted.color)) &&
                 max(max(abs(extracted.color.x), abs(extracted.color.y)), abs(extracted.color.z)) > 1e-8f &&
                 !any(isnan(direction)) && !any(isinf(direction)) && dot(direction, direction) > 1e-8f;
    if (valid) {
      light.color = extracted.color;
      light.object_to_world = extracted.object_to_world;
      LightSunData sun = light.sun();
      sun.direction = direction;
      light.sun() = sun;
    }
    else {
      light.color = float3(0.0f);
      light.power = float4(0.0f);
      light.tilemap_index = LIGHT_NO_SHADOW;
    }
  }
  return light;
}

bool light_linking_affects_receiver(uint2 membership, uchar receiver)
{
  return bitmask64_test(membership, receiver);
}

void light_eval_single_closure(LightData light, LightVector lv, ClosureLight &cl,
                               float3 V, float attenuation, float3 shadow)
{
  [[resource_table]] const UtilityTexture &util = resource_table_get(UtilityTexture);
  attenuation *= light.power[cl.type & 3u];
  if (attenuation < 1e-30f) {
    return;
  }
  float ltc = light_ltc(util.utility_tx, light, cl.N, V, lv, cl.ltc_mat,
                       light_shape_corners(light, lv));
  float3 radiance = light.color * ltc;
  cl.light_shadowed += shadow * attenuation * radiance;
  cl.light_unshadowed += attenuation * radiance;
}

float3 shadow_eval_dispatch(uint light_index, LightData light,
                            bool directional, bool transmission, bool translucent,
                            float thickness, float3 P, float3 Ng, float3 N,
                            float normal_offset, float geometry_offset,
                            int ray_count, int step_count)
{
  if (shadow_dispatch_force_unshadowed) {
    return float3(1.0f);
  }
  if (shadow_dispatch_use_hardware_rt && light.hwrt_shadow_layer >= 0 &&
      (!transmission || shadow_dispatch_allow_transmission_hardware_rt))
  {
    return hardware_rt_shadow_visibility_fetch(shadow_dispatch_texel_fullres,
                                               light.hwrt_shadow_layer);
  }
  [[resource_table]] eevee::ShadowRenderData &shadows = resource_table_get(eevee::ShadowRenderData);
  return float3(eevee::shadow_eval(shadows, light, directional, transmission, translucent,
                           float2(shadow_dispatch_texel_fullres), hardware_thickness(thickness),
                           P, Ng, N, normal_offset, geometry_offset, ray_count, step_count));
}

void light_eval_single(uint light_index, bool directional, bool transmission,
                       ClosureLightStack &stack, float3 P, float3 Ng, float3 V,
                       float thickness, uchar receiver, float normal_offset, float geometry_offset)
{
  LightData light = hardware_light_get(light_index);
  if (!light_linking_affects_receiver(light.light_set_membership, receiver)) {
    return;
  }
  LightVector lv = light_vector_get(light, directional, P);
  bool translucent = transmission && stack.cl[0].type == LIGHT_TRANSLUCENT_WITH_THICKNESS;
  float attenuation = light_attenuation_surface(light, directional, lv);
  if (!translucent) {
    attenuation *= light_attenuation_facing(light, lv.L, lv.dist, stack.cl[0].N, transmission);
  }
  if (attenuation < LIGHT_ATTENUATION_THRESHOLD) {
    return;
  }
  float3 shadow = float3(1.0f);
  if (light.tilemap_index != LIGHT_NO_SHADOW || light.hwrt_shadow_layer >= 0) {
    shadow = shadow_eval_dispatch(light_index, light, directional, transmission, translucent,
                                  thickness, P, Ng, stack.cl[0].N, normal_offset, geometry_offset,
                                  uniform_buf.shadow.ray_count, uniform_buf.shadow.step_count);
  }
  if (translucent) {
    stack.cl[0].N = lv.L;
    attenuation *= M_1_PI;
  }
  light_eval_single_closure(light, lv, stack.cl[0], V, attenuation, shadow);
}

#define LIGHT_FOREACH_BEGIN_DIRECTIONAL(_unused, index) \
  for (uint index = uint(hardware_local_lights_len); index < uint(hardware_lights_len); index++)
#define LIGHT_FOREACH_END

/* Nuru's neutral N0 distribution. Learned NIS cluster weights are an optional optimization,
 * not a different light estimator; neutral multipliers preserve the PMF and its compensation. */
void hardware_light_cluster_multipliers(float3 P, bool valid, out float weights[HWRT_LIGHT_CLUSTER_COUNT])
{
  for (int i = 0; i < HWRT_LIGHT_CLUSTER_COUNT; i++) {
    weights[i] = 1.0f;
  }
}

float hardware_direct_light_local_importance(uint light_index, float3 P, bool valid)
{
  LightData light = hardware_light_get(light_index);
  float importance = max(dot(abs(light.color), float3(0.2126f, 0.7152f, 0.0722f)), 1.0e-4f);
  if (valid) {
    float3 to_light = light.position() - P;
    float radius = max(light.local().local.shape_radius, 1.0e-2f);
    float power = max(abs(light.power[LIGHT_DIFFUSE]), 1.0e-6f);
    importance *= power / max(dot(to_light, to_light) + radius * radius, 1.0e-4f);
  }
  return max(importance, 1.0e-8f);
}
