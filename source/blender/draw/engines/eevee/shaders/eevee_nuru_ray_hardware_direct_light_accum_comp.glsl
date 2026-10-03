/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Convert sampled many-light RT visibility records into a sparse combined direct-light buffer.
 *
 * This first slice intentionally targets opaque/specular direct lighting only. Transmission-style
 * closures remain on the existing deferred path until the material-policy todos land.
 */

#include "infos/eevee_nuru_direct_light_infos.hh"

#define SHADOW_DISPATCH_USE_GLOBAL_TEXEL
#define LIGHT_ITER_FORCE_NO_CULLING
#define LIGHT_CLOSURE_EVAL_COUNT 1
COMPUTE_SHADER_CREATE_INFO(eevee_ray_hardware_direct_light_accum)

#define HWRT_DIRECT_EVALUATION
#include "eevee_nuru_direct_light_lib.glsl"
#include "eevee_colorspace_lib.bsl.hh"
#include "eevee_gbuffer_read_lib.glsl"
/* `shadow_dispatch_texel_fullres` is declared in `eevee_shadow_tracing_lib.glsl` under
 * SHADOW_DISPATCH_USE_GLOBAL_TEXEL (defined by this shader's create info). */

#include "eevee_reverse_z_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

#include "eevee_nuru_direct_light_importance_lib.glsl"


float sun_total_importance()
{
  float total_importance = 0.0f;
  for (uint sun_index = 0u; sun_index < raytrace_buf.hardware_direct_light.sun_lights_len;
       sun_index++)
  {
    const uint light_index = raytrace_buf.hardware_direct_light.local_lights_len + sun_index;
    total_importance += hardware_direct_light_color_importance(light_buf[light_index]) *
                        raytrace_buf.hardware_direct_light.sun_light_importance_scale;
  }
  return total_importance;
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
  return result * cl.color;
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
  const HardwareDirectLightVisibilitySample visibility_record =
      hardware_direct_light_visibility_samples_buf[queue_index];
  const int2 texel = int2(unpackUvec2x16(visibility_record.packed_sample_texel));

  if (any(lessThan(texel, int2(0))) || any(greaterThanEqual(texel, textureSize(depth_tx, 0)))) {
    return;
  }

  float depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
  if (!(depth > 0.0f && depth < 1.0f)) {
    return;
  }

  float2 uv = (float2(texel) + 0.5f) * raytrace_buf.full_resolution_inv;
  const gbuffer::Layers gbuf = gbuffer::read_layers(texel);
  const float3 P = drw_point_screen_to_world(float3(uv, depth));
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
  float train_luma = 0.0f;

  if (visibility_record.local_light_index != 0xFFFFFFFFu &&
      visibility_record.local_importance > 0.0f)
  {
    /* Nuru N0/N1: P is reconstructed from the same texel/depth as in the selection
     * kernel, so the per-cluster sums match the selection PDF exactly. Two-stage
     * compensation: sum_c(s_c * m_c) / (m_picked * imp_picked). */
    float cluster_sums[HWRT_LIGHT_CLUSTER_COUNT];
    hardware_light_cluster_sums(work_tile, P, true, cluster_sums);
    float cluster_multipliers[HWRT_LIGHT_CLUSTER_COUNT];
    hardware_light_cluster_multipliers(P, true, cluster_multipliers);
    const float weighted_total = hardware_light_cluster_weighted_total(cluster_sums,
                                                                       cluster_multipliers);
    const uint picked_cluster =
        uint(light_buf[visibility_record.local_light_index].hwrt_cluster_id) %
        uint(HWRT_LIGHT_CLUSTER_COUNT);
    const float sample_weight =
        weighted_total / max(cluster_multipliers[picked_cluster] *
                                 visibility_record.local_importance,
                             1.0e-6f);
    float3 local_radiance = float3(0.0f);
    for (uchar closure_index = 0u; closure_index < gbuf.header.closure_len(); closure_index++) {
      local_radiance += hardware_direct_light_evaluate_single(visibility_record.local_light_index,
                                                              false,
                                                              visibility_record.local_visibility,
                                                              gbuf.layer[closure_index],
                                                              P,
                                                              Ng,
                                                              V,
                                                              receiver_light_set) *
                        sample_weight;
    }
    radiance += local_radiance;
    /* Nuru NIS: realized L/p estimate for the online KL training pass. */
    train_luma = dot(max(local_radiance, float3(0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
  }

  if (visibility_record.sun_light_index != 0xFFFFFFFFu &&
      visibility_record.sun_importance > 0.0f)
  {
    const uint sun_light_index = raytrace_buf.hardware_direct_light.local_lights_len +
                                 visibility_record.sun_light_index;
    const float sample_weight = sun_total_importance() /
                                max(visibility_record.sun_importance, 1.0e-6f);
    for (uchar closure_index = 0u; closure_index < gbuf.header.closure_len(); closure_index++) {
      radiance += hardware_direct_light_evaluate_single(sun_light_index,
                                                        true,
                                                        visibility_record.sun_visibility,
                                                        gbuf.layer[closure_index],
                                                        P,
                                                        Ng,
                                                        V,
                                                        receiver_light_set) *
                  sample_weight;
    }
  }

  radiance = colorspace::brightness_clamp_max(radiance, uniform_buf.clamp.surface_direct);
  imageStore(out_direct_light_accum_img, texel, float4(radiance, 1.0f));
  hardware_direct_light_visibility_samples_buf[queue_index].train_luma = train_luma;
}
