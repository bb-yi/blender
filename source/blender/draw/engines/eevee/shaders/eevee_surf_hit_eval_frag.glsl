/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_surf_hit_eval_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_geom_hit_mesh)
#ifdef MAT_SHADOW_TINT
FRAGMENT_SHADER_CREATE_INFO(eevee_surf_shadow_tint)
#else
FRAGMENT_SHADER_CREATE_INFO(eevee_surf_hit_eval)
#endif

#define HWRT_HIT_IDENTITY_PRINCIPLED_LAYERED_SCENE_FINAL 32u
#define HWRT_HIT_IDENTITY_METALLIC_BSDF_SCENE_FINAL 64u

#include "eevee_gbuffer_types.bsl.hh"
#include "eevee_hit_attr_lib.glsl"
#include "eevee_nodetree_lib.bsl.hh"
#include "gpu_shader_index_load_lib.glsl"
#include "gpu_shader_math_vector_reduce.bsl.hh"
#include "gpu_shader_math_vector_safe.bsl.hh"

void attrib_load(HitMeshVertex domain);
Closure nodetree_surface(KernelGlobals kg, ShadingData &sd, float closure_rand);
float nodetree_thickness(KernelGlobals kg, ShadingData &sd);

float4 closure_to_rgba(KernelGlobals /*kg*/, ShadingData & /*sd*/, Closure /*cl*/)
{
  return float4(0.0f);
}

float3 hit_eval_barycentric_expand(float2 barycentric_coords)
{
  return float3(max(0.0f, 1.0f - barycentric_coords.x - barycentric_coords.y),
                barycentric_coords.x,
                barycentric_coords.y);
}

void hit_eval_init_globals(ShadingData &sd, float3 P, float3 N)
{
  [[resource_table]] const eevee::Uniform &uni = resource_table_get(eevee::Uniform);
  sd.P = P;
  sd.displacement_base = P;
  sd.forward_lighting_P = P;
  sd.Ni = sd.front_facing ? N : -N;
  sd.N = safe_normalize(sd.Ni);
  sd.ray_type = uni.pipeline_buf.ray_type;
  sd.ray_length = distance(P, sd.hit_view_origin);
  sd.thickness = Thickness::zero();
  sd.npr_diffusion_weight = 1.0f;
  /* Hit-eval replays the material on a tiny screen-space proxy triangle around the destination
   * pixel. Deriving the geometric normal from that proxy leaks the raw traced triangle plane back
   * into smooth hits, which is especially visible on reflected / refracted curved surfaces. Keep a
   * smooth-facing geometry proxy here instead of rebuilding `Ng` from the proxy triangle. */
  sd.Ng = uni.pipeline_buf.is_main_view_inverted ? -float3(sd.N) : float3(sd.N);
}

bool hit_eval_closure_is_specular_family(ClosureType type)
{
  return (type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) ||
         (type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
}

bool hit_eval_closure_is_base_family(ClosureType type)
{
  return (type == CLOSURE_BSDF_DIFFUSE_ID) || (type == CLOSURE_BSDF_TRANSLUCENT_ID) ||
         (type == CLOSURE_BSSRDF_BURLEY_ID);
}

bool hit_eval_closure_has_energy(ClosureUndetermined cl)
{
  return (cl.type != CLOSURE_NONE_ID) && (dot(cl.color, cl.color) > 1.0e-10f);
}

ClosureUndetermined hit_eval_closure_select_family(const ShadingData &sd,
                                                   bool want_specular)
{
  [[resource_table]] const eevee::PipelineConstants &pipe =
      resource_table_get(eevee::PipelineConstants);
  ClosureUndetermined best = closure_new(CLOSURE_NONE_ID);
  best.N = sd.N;

  float best_score = 0.0f;
  for (int i = 0; i < pipe.closure_bin_count; i++) {
    ClosureUndetermined candidate = sd.closure_get_resolved(pipe, uchar(i), 1.0f);
    if (candidate.type == CLOSURE_NONE_ID || candidate.weight() <= CLOSURE_WEIGHT_CUTOFF) {
      continue;
    }

    bool family_match = want_specular ? hit_eval_closure_is_specular_family(candidate.type) :
                                        hit_eval_closure_is_base_family(candidate.type);
    if (!family_match) {
      continue;
    }

    const float score = average(abs(candidate.color));
    const bool prefer_reflection_tie =
        want_specular && (candidate.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) &&
        (best.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) &&
        (abs(score - best_score) <= 1.0e-4f);
    if (score > best_score || prefer_reflection_tie) {
      best = candidate;
      best_score = score;
    }
  }

  return best;
}

ClosureUndetermined hit_eval_closure_select_type(const ShadingData &sd,
                                                 ClosureType wanted_type)
{
  [[resource_table]] const eevee::PipelineConstants &pipe =
      resource_table_get(eevee::PipelineConstants);
  ClosureUndetermined best = closure_new(CLOSURE_NONE_ID);
  best.N = sd.N;

  float best_score = 0.0f;
  for (int i = 0; i < pipe.closure_bin_count; i++) {
    ClosureUndetermined candidate = sd.closure_get_resolved(pipe, uchar(i), 1.0f);
    if (candidate.type != wanted_type || candidate.weight() <= CLOSURE_WEIGHT_CUTOFF) {
      continue;
    }

    float score = average(abs(candidate.color));
    if (score > best_score) {
      best = candidate;
      best_score = score;
    }
  }

  return best;
}

/* Both transport and shadow replay use the draw buffers and the same generated node graph. */
void hit_eval_surface_at_triangle(KernelGlobals kg,
                                  ShadingData &sd,
                                  uint primitive_id,
                                  float2 barycentric_coords,
                                  float3 object_P,
                                  float3 traced_world_P,
                                  bool interpolate_position)
{
  const float3 barycentric = hit_eval_barycentric_expand(barycentric_coords);
  const uint primitive_index = primitive_id * 3u + uint(max(hit_eval_index_start, 0));
  const uint i0 = gpu_index_load(primitive_index + 0u);
  const uint i1 = gpu_index_load(primitive_index + 1u);
  const uint i2 = gpu_index_load(primitive_index + 2u);

  const float3 pos0 = hit_attr_fetch_float3(pos, gpu_attr_0, gpu_attr_0_meta, i0);
  const float3 pos1 = hit_attr_fetch_float3(pos, gpu_attr_0, gpu_attr_0_meta, i1);
  const float3 pos2 = hit_attr_fetch_float3(pos, gpu_attr_0, gpu_attr_0_meta, i2);
  const float3 nor0 = hit_attr_fetch_float3(nor, gpu_attr_1, gpu_attr_1_meta, i0);
  const float3 nor1 = hit_attr_fetch_float3(nor, gpu_attr_1, gpu_attr_1_meta, i1);
  const float3 nor2 = hit_attr_fetch_float3(nor, gpu_attr_1, gpu_attr_1_meta, i2);

  const ObjectMatrices obj = kg.object_matrices_get(sd);
  const ObjectInfos ob_infos = kg.object_infos_get(sd);
  if (interpolate_position) {
    object_P = pos0 * barycentric.x + pos1 * barycentric.y + pos2 * barycentric.z;
  }
  float3 world_P = obj.point_object_to_world(object_P);
  if (!interpolate_position && dot(traced_world_P, traced_world_P) > 1.0e-10f) {
    world_P = traced_world_P;
    object_P = obj.point_world_to_object(traced_world_P);
  }
  float3 object_N = nor0 * barycentric.x + nor1 * barycentric.y + nor2 * barycentric.z;
  if (!(dot(object_N, object_N) > 1.0e-10f)) {
    object_N = cross(pos1 - pos0, pos2 - pos0);
  }
  hit_eval_init_globals(sd, world_P, safe_normalize(obj.normal_object_to_world(object_N)));
#ifdef MAT_SHADOW_TINT
  sd.ray_type = RAY_TYPE_SHADOW;
#endif
  HitMeshVertex domain = HitMeshVertex{
      MeshVertex{object_P, to_float3x3(obj.model_inverse),
                 object_P * ob_infos.orco_mul + ob_infos.orco_add},
      int3(i0, i1, i2),
      barycentric};
  attrib_load(domain);
  fragment_displacement(kg, sd);
  nodetree_surface(kg, sd, 0.5f);
}

#ifdef MAT_SHADOW_TINT
void hit_eval_shadow_tint(KernelGlobals kg, ShadingData &sd, uint hit_index)
{
  const HardwareShadowTintHit hit = shadow_tint_hits[hit_index];
  /* The direct order includes rays that were fully blocked later in the walk. */
  if (shadow_tint_rays[hit.identity.z >> 2u].range.x == 0u) {
    return;
  }
  sd.resource_id_raw = shadow_tint_entries[hit.identity.x].y;
  sd.front_facing = (hit.identity.z & SHADOW_TINT_HIT_FRONT_FACING) != 0u;
  sd.hit_view_origin = hit.origin.xyz;
  hit_eval_surface_at_triangle(
      kg, sd, hit.identity.y, hit.barycentric.xy, float3(0.0f), float3(0.0f), true);

  ClosureUndetermined transmission = hit_eval_closure_select_type(
      sd, CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
  if (transmission.type == CLOSURE_NONE_ID) {
    transmission = hit_eval_closure_select_type(sd, CLOSURE_BSDF_TRANSLUCENT_ID);
  }
  float3 tint = (transmission.type == CLOSURE_NONE_ID) ?
                    float3(sd.transmittance) : transmission.color;
  if (any(isnan(tint)) || any(isinf(tint))) {
    return;
  }
  shadow_tint_hits[hit_index].transmission = float4(
      hit.barycentric.z + hit.barycentric.w * saturate(tint), 0.0f);
}
#else
ClosureUndetermined hit_eval_closure_select_specular(const ShadingData &sd,
                                                     ClosureType traced_specular_type,
                                                     ClosureUndetermined secondary_cl)
{
  /* Proxy fallback can only carry one dominant closure family, so dielectric Principled hits can
   * still reach replay with a non-specular traced type even though the real material exposes a
   * reflective lobe. Only honor the exact traced type when it is already a specular family;
   * otherwise choose the dominant replayed specular closure directly.
   *
   * Layered Principled surfaces keep a replayed base-family lobe alongside their reflective or
   * transmissive branch. For those hits, the sync-time proxy thresholds (`metallic > 0.5`,
   * `transmission > 0`) are too coarse to use as a hard exact-type anchor, because they snap the
   * reflected result to pure metal or pure glass before the replayed material has a chance to keep
   * the mixed base/specular balance. Preserve exact reflection/refraction anchoring only for
   * pure-specular hits such as Glossy/Glass where no replayed base-family energy exists. */
  if (!hit_eval_closure_has_energy(secondary_cl) &&
      hit_eval_closure_is_specular_family(traced_specular_type))
  {
    ClosureUndetermined exact = hit_eval_closure_select_type(sd, traced_specular_type);
    if (exact.type != CLOSURE_NONE_ID) {
      return exact;
    }
  }
  return hit_eval_closure_select_family(sd, true);
}

ClosureUndetermined hit_eval_clear_refraction_closure(float3 N,
                                                      float monochromatic_transmittance)
{
  ClosureUndetermined cl = closure_new(CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
  cl.color = float3(saturate(monochromatic_transmittance));
  cl.N = N;
  cl.data = float4(0.0f);
  cl.data.x = 0.0f;
  cl.data.y = 1.0f;
  return cl;
}

float hit_eval_reflection_layer_opacity(ClosureUndetermined base_cl, ClosureUndetermined specular_cl)
{
  if (specular_cl.type != CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) {
    return 0.0f;
  }
  /* The resolved colors already include the closure reservoir weights. */
  const float base_weight = average(abs(base_cl.color));
  const float specular_weight = average(abs(specular_cl.color));
  const float total_weight = base_weight + specular_weight;
  return (total_weight > 1.0e-6f) ? saturate(specular_weight / total_weight) :
                                    float(hit_eval_closure_has_energy(specular_cl));
}

float4 hit_eval_base_pack(ClosureUndetermined cl)
{
  return float4(max(cl.color, float3(0.0f)), float(cl.type));
}

float4 hit_eval_specular_state_pack(ClosureUndetermined cl,
                                    float2 packed_direction,
                                    float reflection_layer_opacity)
{
  float4 hit_material = float4(0.0f, 0.0f, float(cl.type), packed_direction.x);

  switch (cl.type) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID: {
      ClosureReflection reflection = to_closure_reflection(cl);
      hit_material.x = reflection.roughness;
      hit_material.y = reflection_layer_opacity;
      break;
    }
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID: {
      ClosureRefraction refraction = to_closure_refraction(cl);
      hit_material.x = refraction.roughness;
      hit_material.y = refraction.ior;
      break;
    }
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
    case CLOSURE_NONE_ID:
      break;
  }

  return hit_material;
}

float4 hit_eval_specular_color_pack(ClosureUndetermined cl, Thickness thickness)
{
  return float4(max(cl.color, float3(0.0f)), gbuffer::thickness_pack(thickness));
}

void hit_eval_material(KernelGlobals kg, ShadingData &sd, int2 texel, ThicknessMode mode)
{
  const float ray_time = texelFetch(ray_time_tx, texel, 0).x;
  if (!(ray_time > 0.0f)) {
    return;
  }

  const float4 carried_throughput = imageLoadFast(hit_throughput_img, texel);
  const bool apply_carried_throughput = carried_throughput.a > 0.5f;
  const float3 path_tint = max(carried_throughput.rgb, float3(0.0f));

  const uint4 hit_identity = uint4(texelFetch(hit_identity_tx, texel, 0));
  const bool preserved_layered_scene_final = (hit_identity.z & 2u) != 0u;
  const bool preserved_principled_layered_scene_final =
      (hit_identity.z & HWRT_HIT_IDENTITY_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u;
  const bool preserved_metallic_bsdf_scene_final =
      (hit_identity.z & HWRT_HIT_IDENTITY_METALLIC_BSDF_SCENE_FINAL) != 0u;
  const bool preserved_transparent_scene_final = (hit_identity.z & 4u) != 0u;
  const bool is_additional_scene_final_receiver = (hit_identity.z & 8u) != 0u;
  const bool preserved_scene_final_composite = preserved_layered_scene_final ||
                                               preserved_transparent_scene_final;
  const bool scene_final_receiver_replay = preserved_scene_final_composite ||
                                           is_additional_scene_final_receiver;
  const uint primitive_id = hit_identity.y;
  const float4 traced_barycentric = texelFetch(hit_barycentric_tx, texel, 0);
  const float2 barycentric_coords = traced_barycentric.xy;
  const float reflection_layer_coverage = saturate(traced_barycentric.w);
  const ClosureType traced_specular_type = ClosureType(
      uint(max(imageLoadFast(hit_material_img, texel).z, 0.0f) + 0.5f));

  float2 packed_direction = float2(imageLoadFast(hit_material_img, texel).w,
                                   imageLoadFast(hit_normal_img, texel).w);

  const ObjectInfos ob_infos = kg.object_infos_get(sd);
  const float3 object_P = imageLoadFast(hit_position_img, texel).xyz;
  const float3 traced_world_P = texelFetch(hit_world_position_tx, texel, 0).xyz;
  hit_eval_surface_at_triangle(
      kg, sd, primitive_id, barycentric_coords, object_P, traced_world_P, false);

  Thickness thickness = Thickness::from(nodetree_thickness(kg, sd), mode);
  eObjectInfoFlag ob_flag = ob_infos.flag;
  if (flag_test(ob_flag, OBJECT_HOLDOUT)) {
    sd.holdout = 1.0f - average(sd.transmittance);
  }
  const float holdout_visibility = 1.0f - saturate(sd.holdout);
  const float transparent_transmittance = saturate(average(sd.transmittance));

  ClosureUndetermined secondary_cl = hit_eval_closure_select_family(sd, false);
  ClosureUndetermined transmission_cl = hit_eval_closure_select_type(
      sd, CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
  ClosureUndetermined specular_cl = hit_eval_closure_select_specular(
      sd, traced_specular_type, secondary_cl);
  if ((traced_specular_type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) &&
      !hit_eval_closure_has_energy(specular_cl) && !hit_eval_closure_has_energy(transmission_cl) &&
      (transparent_transmittance > 1.0e-4f))
  {
    /* Alpha-transparent materials are already captured into the HWRT scene as a clear refraction
     * proxy. Replay the same contract here instead of leaving direct mirror hits with no usable
     * specular/transmission closure, which collapses them to black. */
    ClosureUndetermined clear_refraction = hit_eval_clear_refraction_closure(
        sd.N, transparent_transmittance);
    specular_cl = clear_refraction;
    transmission_cl = clear_refraction;
  }
  if (scene_final_receiver_replay && hit_eval_closure_has_energy(transmission_cl)) {
    /* Direct-view Principled always keeps the reflection lobe and the transmission lobe as
     * separate layers. Apply the same rule to preserved scene-final hits and their additional
     * receiver payloads so replay does not hand the only specular slot to refraction once
     * transmission becomes dominant. */
    ClosureUndetermined reflection_cl = hit_eval_closure_select_type(
        sd, CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID);
    if (reflection_cl.type != CLOSURE_NONE_ID) {
      specular_cl = reflection_cl;
    }
  }
  const bool textured_refractive_receiver_replay =
      is_additional_scene_final_receiver && ((hit_identity.z & 16u) != 0u) &&
      (transmission_cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) &&
      hit_eval_closure_has_energy(transmission_cl);
  const bool textured_reflective_receiver_replay =
      preserved_layered_scene_final && !preserved_principled_layered_scene_final &&
      !preserved_metallic_bsdf_scene_final && hit_eval_closure_has_energy(specular_cl) &&
      (specular_cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID);
  const bool full_reflective_scene_final_replay =
      preserved_layered_scene_final &&
      (reflection_layer_coverage >= 1.0f - 1.0e-3f) &&
      hit_eval_closure_has_energy(specular_cl) &&
      (specular_cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) &&
      (((hit_identity.z & 16u) != 0u) || preserved_metallic_bsdf_scene_final);
  if (textured_refractive_receiver_replay) {
    /* The transmission receiver is the exit/back face seen through glass. Replaying it only as a
     * refraction closure carries tint but not the visible back-face texture. Promote the sampled
     * refractive texture color into the receiver base layer so HWRT lighting resolves the other
     * side of the surface as a textured hit. */
    ClosureUndetermined receiver_texture_cl = closure_new(CLOSURE_BSDF_DIFFUSE_ID);
    receiver_texture_cl.color = max(max(secondary_cl.color, transmission_cl.color), float3(0.0f));
    receiver_texture_cl.N = sd.N;
    receiver_texture_cl.data = float4(0.0f);
    secondary_cl = receiver_texture_cl;
  }
  else if (textured_reflective_receiver_replay && !full_reflective_scene_final_replay) {
    /* A textured specular object seen inside another mirror must contribute its sampled material
     * texture as the reflected receiver. If we keep it as a specular closure, the continuation
     * mirror proxy wins and replaces the texture with flat grey mirror. */
    ClosureUndetermined receiver_texture_cl = closure_new(CLOSURE_BSDF_DIFFUSE_ID);
    receiver_texture_cl.color = max(max(secondary_cl.color, specular_cl.color), float3(0.0f));
    receiver_texture_cl.N = sd.N;
    receiver_texture_cl.data = float4(0.0f);
    secondary_cl = receiver_texture_cl;
    specular_cl = closure_new(CLOSURE_NONE_ID);
    specular_cl.N = sd.N;
  }
  const bool specular_texture_tint_only_replay =
      (preserved_layered_scene_final && !preserved_principled_layered_scene_final &&
       hit_eval_closure_has_energy(specular_cl)) ||
      (preserved_principled_layered_scene_final && full_reflective_scene_final_replay);
  if (specular_texture_tint_only_replay) {
    /* Glossy / Metallic BSDF and full-metal Principled: the image texture must tint the
     * reflective lobe only. Do not also export a separate base-family layer that mirror lighting
     * treats as an extra diffuse pass on top of the metal reflection. */
    if (hit_eval_closure_is_base_family(secondary_cl.type) &&
        hit_eval_closure_has_energy(secondary_cl))
    {
      specular_cl.color = max(specular_cl.color, secondary_cl.color);
    }
    secondary_cl = closure_new(CLOSURE_NONE_ID);
    secondary_cl.N = sd.N;
  }
  const float reflection_layer_opacity = hit_eval_reflection_layer_opacity(secondary_cl, specular_cl);
  /* Keep the base-family closure intact even when glass exposes both reflection and refraction.
   * The later Hardware lighting pass expects `secondary_cl` to stay on the diffuse/transmission
   * family so probe / GI transport is not replaced by a reflected specular lobe. Pure Glass/Glossy
   * hits still honor the exact traced reflection/refraction branch when available, while layered
   * Principled surfaces fall back to the dominant replayed specular lobe instead of inheriting the
   * coarse proxy threshold that picked the traversal family. */
  secondary_cl.color *= holdout_visibility;
  specular_cl.color *= holdout_visibility;
  transmission_cl.color *= holdout_visibility;
  float3 transmission_layer_color = max(transmission_cl.color, float3(0.0f));
  if ((transmission_cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) &&
      (thickness.value() != 0.0f))
  {
    /* Match direct-view Principled transmission compositing: the transmitted receiver is seen
     * through both the front and back transmission events, while earlier path throughput should
     * still only tint the mirrored branch once. */
    transmission_layer_color *= transmission_layer_color;
  }
  if (apply_carried_throughput) {
    /* Metal continuation already accumulated the chromatic throughput of the earlier specular
     * segments. Reapply that tint onto the replayed final receiver so mirrors keep the colored
     * sphere appearance instead of bleaching back to only the last hit material. */
    secondary_cl.color *= path_tint;
    specular_cl.color *= path_tint;
    transmission_cl.color *= path_tint;
    transmission_layer_color *= path_tint;
  }

  float3 shading_normal = sd.N;
  if (!(dot(shading_normal, shading_normal) > 1.0e-10f)) {
    shading_normal = sd.Ni;
  }

  imageStoreFast(hit_albedo_img, texel, hit_eval_base_pack(secondary_cl));
  imageStoreFast(
      hit_material_img, texel, hit_eval_specular_state_pack(specular_cl, packed_direction, reflection_layer_opacity));
  imageStoreFast(hit_normal_img, texel, float4(safe_normalize(shading_normal), packed_direction.y));
  imageStoreFast(hit_position_img, texel, hit_eval_specular_color_pack(specular_cl, thickness));
  if (is_additional_scene_final_receiver) {
    imageStoreFast(hit_throughput_img, texel, carried_throughput);
  }
  else {
    const bool has_transmission_layer = hit_eval_closure_has_energy(transmission_cl);
    imageStoreFast(hit_throughput_img,
                   texel,
                   float4(transmission_layer_color,
                          has_transmission_layer ? 1.0f : 0.0f));
  }

  [[resource_table]] const eevee::Uniform &uni = resource_table_get(eevee::Uniform);
  float3 history_ndc = project_point(uni.raytrace_buf.history_persmat, sd.P);
  float2 history_uv = history_ndc.xy * 0.5f + 0.5f;
  uint packed_uv = 0u;
  /* Raster reuse must fail closed near the screen border. Clamping a reflected hit back into the
   * viewport can turn an off-screen projection into a moving screen-space band inside the mirror. */
  float2 history_margin = uni.raytrace_buf.full_resolution_inv * 2.0f;
  if (all(lessThan(abs(history_ndc.xy), float2(1.0f))) &&
      all(greaterThan(history_uv, history_margin)) &&
      all(lessThan(history_uv, float2(1.0f) - history_margin)))
  {
    packed_uv = (uint(history_uv.x * 65535.0f) & 0xFFFFu) |
                ((uint(history_uv.y * 65535.0f) & 0xFFFFu) << 16u);
  }

  float4 radiance = imageLoadFast(ray_radiance_img, texel);
  /* Replace only this receiver's coarse emission. Earlier path radiance must survive replay,
   * and compilation failures must retain the complete traversal proxy. Keep the incoming
   * throughput separate from the layered BSDF tint, which can belong to a later path segment. */
  const float4 emission_replay = texelFetch(hit_emission_replay_tx, texel, 0);
  radiance.rgb = max(radiance.rgb - emission_replay.rgb * hit_proxy_emission * emission_replay.a,
                    float3(0.0f));
  radiance.rgb += emission_replay.rgb * max(sd.emission, float3(0.0f)) * holdout_visibility;
  radiance.a = uintBitsToFloat(packed_uv);
  imageStoreFast(ray_radiance_img, texel, radiance);
}
#endif
