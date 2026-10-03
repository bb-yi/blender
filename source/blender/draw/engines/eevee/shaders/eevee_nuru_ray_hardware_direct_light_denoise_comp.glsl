/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Resolve the sparse many-light direct accumulator per pixel inside the queued tile.
 *
 * The tile pass still chooses a bounded stochastic light sample per culling tile, but the final
 * lighting resolve now reads per-pixel HWRT visibility instead of splatting one sample texel's
 * radiance across the entire tile. This preserves the many-light budget while removing the
 * obvious tile-sized shadow blocks.
 */

#include "infos/eevee_nuru_direct_light_infos.hh"

#define SHADOW_DISPATCH_USE_GLOBAL_TEXEL
#define LIGHT_ITER_FORCE_NO_CULLING
#define LIGHT_CLOSURE_EVAL_COUNT 1
#define HWRT_DIRECT_VISIBILITY
COMPUTE_SHADER_CREATE_INFO(eevee_ray_hardware_direct_light_denoise)

#define HWRT_DIRECT_EVALUATION
#include "eevee_nuru_direct_light_lib.glsl"
#include "eevee_colorspace_lib.bsl.hh"
#include "eevee_gbuffer_read_lib.glsl"
/* `shadow_dispatch_texel_fullres` is declared in `eevee_shadow_tracing_lib.glsl` under
 * SHADOW_DISPATCH_USE_GLOBAL_TEXEL (defined by this shader's create info). */

#include "eevee_reverse_z_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

float light_color_importance(LightData light)
{
  return max(dot(abs(light.color), float3(0.2126f, 0.7152f, 0.0722f)), 1.0e-4f);
}

float local_light_importance(uint light_index)
{
  LightData light = light_buf[light_index];
  float importance = light_color_importance(light) *
                     raytrace_buf.hardware_direct_light.local_light_importance_scale;
  if (is_area_light(light.type)) {
    importance *= raytrace_buf.hardware_direct_light.area_light_importance_scale;
  }
  return importance;
}

float sun_light_importance(uint sun_index)
{
  const uint light_index = raytrace_buf.hardware_direct_light.local_lights_len + sun_index;
  LightData light = light_buf[light_index];
  return light_color_importance(light) *
         raytrace_buf.hardware_direct_light.sun_light_importance_scale;
}

float local_total_importance(HardwareDirectLightWorkTile work_tile)
{
  float total_importance = 0.0f;
  for (uint word_index = 0u; word_index < work_tile.candidate_word_count; word_index++) {
    uint word = light_tile_buf[work_tile.candidate_word_offset + word_index];
    int bit_index;
    while ((bit_index = findLSB(word)) != -1) {
      word &= ~(1u << uint(bit_index));
      const uint light_index = word_index * 32u + uint(bit_index);
      if (light_index < raytrace_buf.hardware_direct_light.local_lights_len) {
        total_importance += local_light_importance(light_index);
      }
    }
  }
  return total_importance;
}

float sun_total_importance()
{
  float total_importance = 0.0f;
  for (uint sun_index = 0u; sun_index < raytrace_buf.hardware_direct_light.sun_lights_len;
       sun_index++)
  {
    const uint light_index = raytrace_buf.hardware_direct_light.local_lights_len + sun_index;
    total_importance += light_color_importance(light_buf[light_index]) *
                        raytrace_buf.hardware_direct_light.sun_light_importance_scale;
  }
  return total_importance;
}

bool select_local_light_from_total(HardwareDirectLightWorkTile work_tile,
                                   float total_importance,
                                   float selector,
                                   uint &r_light_index,
                                   float &r_importance)
{
  if (!(total_importance > 0.0f)) {
    r_light_index = 0xFFFFFFFFu;
    r_importance = 0.0f;
    return false;
  }

  const float target = selector * total_importance;
  float accum_importance = 0.0f;
  for (uint word_index = 0u; word_index < work_tile.candidate_word_count; word_index++) {
    uint word = light_tile_buf[work_tile.candidate_word_offset + word_index];
    int bit_index;
    while ((bit_index = findLSB(word)) != -1) {
      word &= ~(1u << uint(bit_index));
      const uint light_index = word_index * 32u + uint(bit_index);
      if (light_index >= raytrace_buf.hardware_direct_light.local_lights_len) {
        continue;
      }
      const float importance = local_light_importance(light_index);
      accum_importance += importance;
      if (accum_importance >= target) {
        r_light_index = light_index;
        r_importance = importance;
        return true;
      }
    }
  }

  r_light_index = 0xFFFFFFFFu;
  r_importance = 0.0f;
  return false;
}

bool select_sun_light_from_total(float total_importance,
                                 float selector,
                                 uint &r_sun_index,
                                 float &r_importance)
{
  const uint sun_lights_len = raytrace_buf.hardware_direct_light.sun_lights_len;
  if (sun_lights_len == 0u ||
      !raytrace_buf.hardware_direct_light.trace_sun_lights_separately ||
      !(total_importance > 0.0f))
  {
    r_sun_index = 0xFFFFFFFFu;
    r_importance = 0.0f;
    return false;
  }

  const float target = selector * total_importance;
  float accum_importance = 0.0f;
  for (uint sun_index = 0u; sun_index < sun_lights_len; sun_index++) {
    const float importance = sun_light_importance(sun_index);
    accum_importance += importance;
    if (accum_importance >= target) {
      r_sun_index = sun_index;
      r_importance = importance;
      return true;
    }
  }

  r_sun_index = 0xFFFFFFFFu;
  r_importance = 0.0f;
  return false;
}

bool hardware_direct_light_supported_closure(ClosureUndetermined cl)
{
  return (cl.type != CLOSURE_NONE_ID) && (cl.type != CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID) && (cl.type != CLOSURE_BSDF_TRANSLUCENT_ID) &&
         (cl.type != CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
}

LightData hardware_direct_light_resolved(uint light_index)
{
  LightData light = light_buf[light_index];
  light.color = abs(light.color);
  const uint local_lights_len = raytrace_buf.hardware_direct_light.local_lights_len;
  const uint sun_lights_len = raytrace_buf.hardware_direct_light.sun_lights_len;
  if (sun_lights_len < 2u || light_index < local_lights_len || light_index >= local_lights_len + 2u) {
    return light;
  }

  const uint first_index = local_lights_len;
  const uint second_index = local_lights_len + 1u;
  LightData first = light_buf[first_index];
  LightData second = light_buf[second_index];
  first.color = abs(first.color);
  second.color = abs(second.color);

  const bool split_pair = dot(first.color - second.color, first.color - second.color) <= 1.0e-6f &&
                          dot(first.sun().direction, second.sun().direction) >= 0.9999f &&
                          ((first.power[LIGHT_DIFFUSE] == 0.0f && second.power[LIGHT_SPECULAR] == 0.0f) ||
                           (first.power[LIGHT_SPECULAR] == 0.0f && second.power[LIGHT_DIFFUSE] == 0.0f));
  if (!split_pair) {
    return light;
  }

  const uint peer_index = (light_index == first_index) ? second_index : first_index;
  const LightData peer = light_buf[peer_index];
  light.power[LIGHT_DIFFUSE] = max(light.power[LIGHT_DIFFUSE], peer.power[LIGHT_DIFFUSE]);
  light.power[LIGHT_SPECULAR] = max(light.power[LIGHT_SPECULAR], peer.power[LIGHT_SPECULAR]);
  light.power[LIGHT_TRANSMISSION] = max(light.power[LIGHT_TRANSMISSION], peer.power[LIGHT_TRANSMISSION]);
  light.power[LIGHT_VOLUME] = max(light.power[LIGHT_VOLUME], peer.power[LIGHT_VOLUME]);
  return light;
}

LightData hardware_direct_light_exact_local(uint local_light_index)
{
  LightData light = light_buf[local_light_index];
  light.color = abs(light.color);
  return light;
}

float3 hardware_direct_light_evaluate_single(uint light_index,
                                             bool is_directional,
                                             float3 shadow,
                                             ClosureUndetermined cl,
                                             float3 P,
                                             float3 Ng,
                                             float3 V,
                                             uchar receiver_light_set)
{
  direct_last_unshadowed = float3(0.0f);
  if (!hardware_direct_light_supported_closure(cl)) {
    return float3(0.0f);
  }
  const uint index = light_index;
  LightData light = hardware_direct_light_resolved(index);
  float3 result = hardware_direct_evaluate(index, light, is_directional, cl, P, Ng, V, receiver_light_set);
  return result;
}

float3 hardware_direct_light_evaluate_exact_local(uint local_light_index,
                                                  float3 shadow,
                                                  ClosureUndetermined cl,
                                                  float3 P,
                                                  float3 Ng,
                                                  float3 V,
                                                  uchar receiver_light_set)
{
  direct_last_unshadowed = float3(0.0f);
  if (!hardware_direct_light_supported_closure(cl)) {
    return float3(0.0f);
  }
  const uint index = local_light_index;
  LightData light = hardware_direct_light_exact_local(index);
  float3 result = hardware_direct_evaluate(index, light, false, cl, P, Ng, V, receiver_light_set);
  return result;
}

void main()
{
  const uint queue_index = gl_GlobalInvocationID.x;
  /* See `eevee_nuru_ray_hardware_direct_light_visibility_comp.glsl`: never index past the host
   * allocation captured at sync. */
  if (queue_index >= uint(hardware_direct_light_tile_capacity)) {
    return;
  }
  const HardwareDirectLightWorkTile work_tile = hardware_direct_light_work_tiles_buf[queue_index];
  const uint2 tile_coord = unpackUvec2x16(work_tile.packed_tile_coord);
  const uint tile_size_px = max(raytrace_buf.hardware_direct_light.tile_size_px, 1u);
  const int2 tile_origin = int2(tile_coord * tile_size_px);
  const int2 image_extent = imageSize(out_direct_light_denoised_img);
  const uint samples_per_point = max(
      raytrace_buf.hardware_direct_light.light_samples_per_shading_point, 1u);
  const float total_local_importance = local_total_importance(work_tile);
  const bool use_exact_local_lights =
      (raytrace_buf.hardware_direct_light.local_lights_len > 0u) &&
      (raytrace_buf.hardware_direct_light.local_lights_len <= 8u);
  const float total_sun_importance = sun_total_importance();

  imageStore(direct_light_tilemask_img, int2(tile_coord), uint4(1u));

  for (uint y = 0u; y < tile_size_px; y++) {
    for (uint x = 0u; x < tile_size_px; x++) {
      const int2 texel = tile_origin + int2(x, y);
      if (any(greaterThanEqual(texel, image_extent))) {
        continue;
      }

      const float scene_depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
      if (!(scene_depth > 0.0f && scene_depth < 1.0f)) {
        imageStore(out_direct_light_denoised_img, texel, float4(0.0f));
        imageStore(out_direct_light_depth_img, texel, float4(0.0f));
        continue;
      }

      const float2 texel_f = float2(texel) + 0.5f;
      float2 uv = texel_f * raytrace_buf.full_resolution_inv;
      const gbuffer::Layers gbuf = gbuffer::read_layers(texel);
      const uchar closure_count = gbuf.header.closure_len();
      if (closure_count == 0u) {
        imageStore(out_direct_light_denoised_img, texel, float4(0.0f));
        imageStore(out_direct_light_depth_img, texel, float4(scene_depth));
        continue;
      }
      const float3 P = drw_point_screen_to_world(float3(uv, scene_depth));
      const float3 Ng = gbuf.header.geometry_normal(gbuf.surface_N());
      const float3 V = drw_world_incident_vector(P);
      shadow_dispatch_texel_fullres = texel;

      uchar receiver_light_set = 0u;
      if (gbuf.header.use_object_id()) {
        const uint object_id = gbuffer::read_object_id(texel);
        ObjectInfos object_infos = hwrt_object_infos_get(object_id);
        receiver_light_set = receiver_light_set_get(object_infos);
      }

      float3 radiance = float3(0.0f);
      float3 closure_radiance[3] = float3[3](float3(0.0f), float3(0.0f), float3(0.0f));
      float3 closure_unshadowed[3] = float3[3](float3(0.0f), float3(0.0f), float3(0.0f));

      if (use_exact_local_lights) {
        for (uint local_light_index = 0u;
             local_light_index < min(raytrace_buf.hardware_direct_light.local_lights_len, light_cull_buf.visible_count);
             local_light_index++)
        {
          const LightData light = light_buf[local_light_index];
          const float3 local_visibility = hardware_direct_visibility(texel, light.hwrt_shadow_layer);
          for (uchar closure_index = 0u; closure_index < closure_count; closure_index++) {
            closure_radiance[closure_index] += hardware_direct_light_evaluate_exact_local(local_light_index,
                                                                   local_visibility,
                                                                   gbuf.layer[closure_index],
                                                                   P,
                                                                   Ng,
                                                                   V,
                                                                   receiver_light_set);
            closure_unshadowed[closure_index] += direct_last_unshadowed;
          }
        }
      }
      else if (total_local_importance > 0.0f) {
        for (uint sample_index = 0u; sample_index < samples_per_point; sample_index++) {
          uint local_light_index;
          float local_importance;
          const float local_selector = interleaved_gradient_noise(
              float2(texel) + 0.5f,
              2.0f + float(sample_index),
              sampling_rng_1D_get(SAMPLING_RAYTRACE_U) + float(sample_index) * 0.6180339f);
          if (!select_local_light_from_total(
                  work_tile, total_local_importance, local_selector, local_light_index, local_importance))
          {
            continue;
          }
          /* Nuru: `local_light_index` is into the culled+sorted `light_buf`. Translate to the
           * stable visibility layer through `light.hwrt_shadow_layer`. RGB carries the transparent
           * shadow attenuation. */
          const LightData picked_light = light_buf[local_light_index];
          const float3 local_visibility = hardware_direct_visibility(texel, picked_light.hwrt_shadow_layer);
          const float local_sample_weight =
              (total_local_importance / max(local_importance, 1.0e-6f)) / float(samples_per_point);
          for (uchar closure_index = 0u; closure_index < closure_count; closure_index++) {
            closure_radiance[closure_index] += hardware_direct_light_evaluate_single(local_light_index,
                                                              false,
                                                              local_visibility,
                                                              gbuf.layer[closure_index],
                                                              P,
                                                              Ng,
                                                              V,
                                                              receiver_light_set) *
                        local_sample_weight;
            closure_unshadowed[closure_index] += direct_last_unshadowed * local_sample_weight;
          }
        }
      }

      if (total_sun_importance > 0.0f) {
        for (uint sample_index = 0u; sample_index < samples_per_point; sample_index++) {
          uint sun_index;
          float sun_importance;
          const float sun_selector = interleaved_gradient_noise(
              float2(texel) + 0.5f,
              17.0f + float(sample_index),
              sampling_rng_1D_get(SAMPLING_RAYTRACE_X) + float(sample_index) * 0.41421356f);
          if (!select_sun_light_from_total(total_sun_importance, sun_selector, sun_index, sun_importance)) {
            continue;
          }
          const uint sun_light_index = raytrace_buf.hardware_direct_light.local_lights_len +
                                       sun_index;
          const LightData sun_light = light_buf[sun_light_index];
          const float3 sun_visibility = hardware_direct_visibility(texel, sun_light.hwrt_shadow_layer);
          const float sun_sample_weight =
              (total_sun_importance / max(sun_importance, 1.0e-6f)) / float(samples_per_point);
          for (uchar closure_index = 0u; closure_index < closure_count; closure_index++) {
            closure_radiance[closure_index] += hardware_direct_light_evaluate_single(
                            sun_light_index,
                            true,
                            sun_visibility,
                            gbuf.layer[closure_index],
                            P,
                            Ng,
                            V,
                            receiver_light_set) *
                        sun_sample_weight;
            closure_unshadowed[closure_index] += direct_last_unshadowed * sun_sample_weight;
          }
        }
      }

      for (uchar layer = 0u; layer < closure_count; layer++) {
        const ClosureUndetermined cl = gbuf.layer[layer];
        const bool supported = hardware_direct_light_supported_closure(cl);
        imageStore(out_direct_light_closures_img, int3(texel, int(layer)),
                   float4(closure_radiance[layer], float(supported)));
        imageStore(out_direct_light_closures_img, int3(texel, int(layer) + 3),
                   float4(closure_unshadowed[layer], float(supported)));
        radiance += closure_radiance[layer] * cl.color;
      }
      radiance = colorspace::brightness_clamp_max(radiance, uniform_buf.clamp.surface_direct);
      imageStore(out_direct_light_denoised_img, texel, float4(radiance, 1.0f));
      imageStore(out_direct_light_depth_img, texel, float4(scene_depth));
    }
  }
}
