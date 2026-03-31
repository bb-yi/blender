/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compute light objects lighting contribution using captured Gbuffer data.
 */

#include "infos/eevee_deferred_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_deferred_capture_eval)

#include "draw_view_lib.glsl"
#include "eevee_closure_lib.glsl"
#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_light_eval_lib.glsl"
#include "eevee_lightprobe_volume_eval_lib.glsl"
#include "gpu_shader_shared_exponent_lib.glsl"

void write_radiance_direct(uchar layer_index, int2 texel, float3 radiance)
{
  uint data = rgb9e5_encode(radiance);
  if (layer_index == 0u) {
    imageStore(direct_radiance_1_img, texel, uint4(data));
  }
  else if (layer_index == 1u) {
    imageStore(direct_radiance_2_img, texel, uint4(data));
  }
  else if (layer_index == 2u) {
    imageStore(direct_radiance_3_img, texel, uint4(data));
  }
}

void write_radiance_indirect(uchar layer_index, int2 texel, float3 radiance)
{
  if (layer_index == 0u) {
    imageStore(indirect_radiance_1_img, texel, float4(radiance, 1.0f));
  }
  else if (layer_index == 1u) {
    imageStore(indirect_radiance_2_img, texel, float4(radiance, 1.0f));
  }
  else if (layer_index == 2u) {
    imageStore(indirect_radiance_3_img, texel, float4(radiance, 1.0f));
  }
}

void main()
{
  int2 texel = int2(gl_FragCoord.xy);

  float depth = texelFetch(hiz_tx, texel, 0).r;

  const gbuffer::Layers gbuf = gbuffer::read_layers(texel);

  if (gbuf.has_no_closure()) {
    out_radiance = float4(0.0f);
    return;
  }

  const uchar closure_count = gbuf.header.closure_len();
  const float thickness = gbuffer::read_thickness(gbuf.header, texel);
  const uint3 bin_indices = gbuf.header.bin_index_per_layer();

  ClosureUndetermined cl_reflect;
  cl_reflect.type = CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID;
  cl_reflect.color = float3(0.0f);
  cl_reflect.N = float3(0.0f);
  cl_reflect.data = float4(0.0f);
  float reflect_weight = 0.0f;

  ClosureUndetermined cl_refract;
  cl_refract.type = CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID;
  cl_refract.color = float3(0.0f);
  cl_refract.N = float3(0.0f);
  cl_refract.data = float4(0.0f);
  float refract_weight = 0.0f;

  for (uchar i = 0; i < GBUFFER_LAYER_MAX && i < closure_count; i++) {
    ClosureUndetermined cl_layer = gbuf.layer_get(i);
    switch (cl_layer.type) {
      case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID: {
        cl_reflect.color += cl_layer.color;
        float weight = reduce_add(cl_layer.color);
        cl_reflect.N += cl_layer.N * weight;
        cl_reflect.data += cl_layer.data * weight;
        reflect_weight += weight;
        break;
      }
      case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID: {
        cl_refract.color += (thickness != 0.0f) ? square(cl_layer.color) : cl_layer.color;
        float weight = reduce_add(cl_layer.color);
        cl_refract.N += cl_layer.N * weight;
        cl_refract.data += cl_layer.data * weight;
        refract_weight += weight;
        break;
      }
      case CLOSURE_BSSRDF_BURLEY_ID:
      case CLOSURE_BSDF_DIFFUSE_ID:
      case CLOSURE_BSDF_TRANSLUCENT_ID:
      case CLOSURE_NONE_ID:
        break;
    }
  }

  {
    float inv_weight = safe_rcp(reflect_weight);
    cl_reflect.N *= inv_weight;
    cl_reflect.data *= inv_weight;
  }
  {
    float inv_weight = safe_rcp(refract_weight);
    cl_refract.N *= inv_weight;
    cl_refract.data *= inv_weight;
  }

  float3 P = drw_point_screen_to_world(float3(screen_uv, depth));
  float3 Ng = gbuf.header.geometry_normal(gbuf.surface_N());
  float3 V = drw_world_incident_vector(P);
  float vPz = dot(drw_view_forward(), P) - dot(drw_view_forward(), drw_view_position());

  ClosureUndetermined cl;
  cl.N = gbuf.surface_N();
  cl.type = CLOSURE_BSDF_DIFFUSE_ID;

  ClosureUndetermined cl_transmit;
  cl_transmit.N = gbuf.surface_N();
  cl_transmit.type = CLOSURE_BSDF_TRANSLUCENT_ID;

  ClosureUndetermined cl_none;
  cl_none.N = gbuf.surface_N();
  cl_none.type = CLOSURE_NONE_ID;
  cl_none.color = float3(0.0f);
  cl_none.data = float4(0.0f);

  ClosureUndetermined cl_reflect_eval = (reflect_weight > 0.0f) ? cl_reflect : cl_none;
  ClosureUndetermined cl_refract_eval = (refract_weight > 0.0f) ? cl_refract : cl_none;

  uchar receiver_light_set = 0;
  float normal_offset = 0.0f;
  float geometry_offset = 0.0f;
  if (gbuf.header.use_object_id()) {
    uint object_id = gbuffer::read_object_id(texel);
    ObjectInfos object_infos = drw_infos[object_id];
    receiver_light_set = receiver_light_set_get(object_infos);
    normal_offset = object_infos.shadow_terminator_normal_offset;
    geometry_offset = object_infos.shadow_terminator_geometry_offset;
  }

  /* Direct light. */
  ClosureLightStack stack;
  stack.cl[0] = closure_light_new(cl, V);
  stack.cl[1] = closure_light_new(cl_reflect_eval, V);
  stack.cl[2] = closure_light_new(cl_none, V);
  light_eval_reflection(stack, P, Ng, V, vPz, receiver_light_set, normal_offset, geometry_offset);

  float3 radiance_front = stack.cl[0].light_shadowed;
  float3 radiance_reflect = (reflect_weight > 0.0f) ? stack.cl[1].light_shadowed :
                                                     float3(0.0f);

  stack.cl[0] = closure_light_new(cl_transmit, V, thickness);
  stack.cl[1] = closure_light_new(cl_refract_eval, V, thickness);
  stack.cl[2] = closure_light_new(cl_none, V, thickness);
  light_eval_transmission(
      stack, P, Ng, V, vPz, thickness, receiver_light_set, normal_offset, geometry_offset);

  float3 radiance_back = stack.cl[0].light_shadowed;
  float3 radiance_refract = (refract_weight > 0.0f) ? stack.cl[1].light_shadowed :
                                                     float3(0.0f);

  /* Indirect light. */
  /* Can only load irradiance to avoid dependency loop with the reflection probe. */
  SphericalHarmonicL1 sh = lightprobe_volume_sample(P, V, Ng);

  float3 indirect_front = spherical_harmonics_evaluate_lambert(Ng, sh);
  /* TODO(fclem): Correct transmission eval. */
  float3 indirect_back = spherical_harmonics_evaluate_lambert(-Ng, sh);
  float3 indirect_reflect = (reflect_weight > 0.0f) ?
                                spherical_harmonics_evaluate_lambert(cl_reflect.N, sh) :
                                float3(0.0f);
  float3 indirect_refract = (refract_weight > 0.0f) ?
                                spherical_harmonics_evaluate_lambert(-cl_refract.N, sh) :
                                float3(0.0f);

  float3 out_direct = float3(0.0f);
  float3 out_indirect = float3(0.0f);
  for (uchar i = 0; i < GBUFFER_LAYER_MAX && i < closure_count; i++) {
    ClosureUndetermined cl_layer = gbuf.layer_get(i);
    float3 direct_light = float3(0.0f);
    float3 indirect_light = float3(0.0f);
    float3 closure_color = cl_layer.color;

    switch (cl_layer.type) {
      case CLOSURE_BSSRDF_BURLEY_ID:
      case CLOSURE_BSDF_DIFFUSE_ID:
        direct_light = radiance_front;
        indirect_light = indirect_front;
        break;
      case CLOSURE_BSDF_TRANSLUCENT_ID:
        direct_light = radiance_back;
        indirect_light = indirect_back;
        if (thickness != 0.0f) {
          closure_color *= closure_color;
        }
        break;
      case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
        direct_light = radiance_reflect;
        indirect_light = indirect_reflect;
        break;
      case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
        direct_light = radiance_refract;
        indirect_light = indirect_refract;
        if (thickness != 0.0f) {
          closure_color *= closure_color;
        }
        break;
      case CLOSURE_NONE_ID:
        break;
    }

    write_radiance_direct(bin_indices[i], texel, direct_light);
    write_radiance_indirect(bin_indices[i], texel, indirect_light);
    out_direct += direct_light * closure_color;
    out_indirect += indirect_light * closure_color;
  }

  out_radiance = float4(out_direct + out_indirect, 0.0f);
}
