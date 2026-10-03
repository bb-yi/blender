/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Evaluate secondary-hit lighting for the experimental Hardware GI path.
 *
 * The Metal trace currently provides hit distance plus coarse material proxies. This pass rebuilds
 * the hit point in Eevee space and reuses the existing light/shadow evaluation code so sun, point,
 * and spot lights can contribute indirect bounce without re-implementing Eevee lighting in Metal.
 */

#include "infos/eevee_ray_hardware_lighting_infos.hh"

#define SHADOW_DISPATCH_USE_GLOBAL_TEXEL
#define SHADOW_DISPATCH_USE_GLOBAL_HARDWARE_RT
#define LIGHT_ITER_FORCE_NO_CULLING
#define LIGHT_CLOSURE_EVAL_COUNT 1
COMPUTE_SHADER_CREATE_INFO(eevee_ray_trace_hardware_lighting)

#include "eevee_closure.bsl.hh"
#include "eevee_colorspace_lib.bsl.hh"
#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_ray_hardware_environment_lib.glsl"
/* `shadow_dispatch_texel_fullres` is declared in `eevee_shadow_tracing_lib.glsl` under
 * SHADOW_DISPATCH_USE_GLOBAL_TEXEL (defined by this shader's create info). */
#include "eevee_ray_hardware_lighting_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_ray_trace_screen_lib.bsl.hh"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_spherical_harmonics.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"

#define RAYTRACE_SPECULAR_MODE_OFF 0
#define RAYTRACE_SPECULAR_MODE_AUTO 3
#define RAYTRACE_SPECULAR_MODE_HYBRID 1
#define RAYTRACE_SPECULAR_MODE_FULL_RT 2
#define AUTO_FULL_RT_REFLECTION_MAX_ROUGHNESS 0.999f
#define AUTO_FULL_RT_REFRACTION_MAX_ROUGHNESS 0.10f
#define HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR 2
#define HWRT_HIT_IDENTITY_PRINCIPLED_LAYERED_SCENE_FINAL 32u
#define HWRT_HIT_IDENTITY_METALLIC_BSDF_SCENE_FINAL 64u
#define PRINCIPLED_DIFFUSE_REFLECTION_FADE_START 0.5f
#define PRINCIPLED_DIFFUSE_REFLECTION_FADE_END 1.0f

#ifndef GPU_METAL
/* Nuru: plain GLSL compiles this file top to bottom, so helpers defined further down need
 * prototypes before their first use. Metal must not see these prototypes: the MSL generator
 * already forward-declares every function as a shader-class member, so an explicit prototype
 * becomes a duplicate member declaration and fails compilation. */
bool hardware_hit_closure_has_energy(ClosureUndetermined cl);
bool hardware_hit_closure_is_specular_family(ClosureType type);
bool hardware_closure_has_transmission(ClosureType type);
bool layered_receiver_hit_exists(int2 texel);
bool layered_receiver_hit_uses_proxy_payload(int2 texel);
bool transmission_receiver_hit_uses_proxy_payload(int2 texel);
#endif

int hardware_hit_specular_mode(ClosureUndetermined cl)
{
  int mode = RAYTRACE_SPECULAR_MODE_OFF;
  switch (cl.type) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      mode = hardware_reflection_mode;
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      mode = hardware_refraction_mode;
      break;
    default:
      return RAYTRACE_SPECULAR_MODE_OFF;
  }

  if (mode != RAYTRACE_SPECULAR_MODE_AUTO) {
    return mode;
  }

  const float roughness = closure_apparent_roughness_get(cl);
  if (cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) {
    return (roughness <= AUTO_FULL_RT_REFLECTION_MAX_ROUGHNESS) ?
               RAYTRACE_SPECULAR_MODE_FULL_RT :
               RAYTRACE_SPECULAR_MODE_HYBRID;
  }

  return (roughness <= AUTO_FULL_RT_REFRACTION_MAX_ROUGHNESS) ?
             RAYTRACE_SPECULAR_MODE_FULL_RT :
             RAYTRACE_SPECULAR_MODE_HYBRID;
}

float hardware_principled_diffuse_reflection_fade(ClosureUndetermined base_cl,
                                                  ClosureUndetermined specular_cl)
{
  const float base_strength = average(abs(base_cl.color));
  const float specular_strength = average(abs(specular_cl.color));
  const float total_strength = base_strength + specular_strength;
  return (total_strength > 1.0e-6f) ? saturate(base_strength / total_strength) : 1.0f;
}

float hardware_hit_reflection_layer_opacity(ClosureUndetermined specular_cl)
{
  return (specular_cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) ?
             saturate(specular_cl.data.y) :
             0.0f;
}

bool hardware_scene_final_is_principled_layered(uint identity_flags)
{
  return (identity_flags & HWRT_HIT_IDENTITY_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u;
}

bool hardware_hit_is_full_reflective_mirror_proxy(uint identity_flags,
                                                  float reflection_layer_coverage,
                                                  ClosureType specular_type)
{
  if (hardware_trace_phase != HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR) {
    return false;
  }
  /* Transmission/layered receiver payloads must keep their own direct-light resolve. */
  if ((identity_flags & 8u) != 0u) {
    return false;
  }
  if ((identity_flags & 2u) == 0u) {
    return false;
  }
  if (((identity_flags & 16u) == 0u) &&
      ((identity_flags & HWRT_HIT_IDENTITY_METALLIC_BSDF_SCENE_FINAL) == 0u))
  {
    return false;
  }
  if (specular_type != CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) {
    return false;
  }
  return reflection_layer_coverage >= 1.0f - 1.0e-3f;
}

bool hardware_hit_uses_specular_texture_tint_only(uint identity_flags,
                                                  float metallic_coverage,
                                                  ClosureUndetermined base_cl,
                                                  ClosureUndetermined specular_cl)
{
  if (!hardware_hit_closure_has_energy(specular_cl) ||
      !hardware_hit_closure_is_specular_family(specular_cl.type))
  {
    return false;
  }
  if ((identity_flags & 2u) == 0u) {
    return false;
  }
  if ((identity_flags & HWRT_HIT_IDENTITY_METALLIC_BSDF_SCENE_FINAL) != 0u) {
    return true;
  }
  if (metallic_coverage >= 1.0f - 1.0e-3f) {
    if ((identity_flags & 16u) != 0u) {
      return true;
    }
    if (hardware_scene_final_is_principled_layered(identity_flags)) {
      return true;
    }
  }
  return false;
}

bool hardware_scene_final_preserves_principled_specular_direct(uint identity_flags,
                                                               ClosureType cl_type,
                                                               float metallic_coverage)
{
  return hardware_scene_final_is_principled_layered(identity_flags) &&
         (cl_type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) &&
         (metallic_coverage > 1.0e-3f);
}

bool hardware_scene_final_suppress_direct_hit_light(bool primary_is_diffuse_gi,
                                                    bool proxy_payload,
                                                    uint identity_flags,
                                                    ClosureType cl_type,
                                                    float metallic_coverage)
{
  const bool scene_final_specular_phase =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
  if (!scene_final_specular_phase || primary_is_diffuse_gi) {
    return false;
  }
  /* Transmission/layered receiver payloads (flag 8u) resolve what is seen through glass. They must
   * keep analytic direct light and replayed textures on interior/back faces. */
  if ((identity_flags & 8u) != 0u) {
    return false;
  }
  if ((cl_type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) &&
      (metallic_coverage >= 1.0f - 1.0e-3f))
  {
    return true;
  }
  if ((identity_flags & 16u) != 0u) {
    return false;
  }
  if (hardware_closure_has_transmission(cl_type)) {
    return true;
  }
  /* Principled mirror replay keeps diffuse and metal only; the dielectric specular lobe is not
   * suppressed here because hit-eval no longer exports it as a reflective receiver. */
  return false;
}

float hardware_hit_principled_metallic_coverage(int2 texel)
{
  /* Metal-reference contract: the macOS lighting pass never binds `hit_barycentric_tx`, so the
   * coverage sample reads zero there and layered Principled mirror receivers keep their diffuse
   * base + analytic direct light (textured floors/walls in metal mirrors). Binding the real
   * coverage on Vulkan (the EMERALD 3 missing-bind fix) flipped these receivers into the
   * full-metal handling (base visibility 0, direct light suppressed): flat/black mirror
   * interiors. Keep the bind for Vulkan's binding validation but replicate the validated Metal
   * runtime value until the mirror-metal matrix is re-validated with real coverage on ALL
   * backends. */
  return 0.0f;
}

float hardware_layered_receiver_principled_metallic_coverage(int2 texel)
{
  /* See `hardware_hit_principled_metallic_coverage`: Metal-reference zero coverage. */
  return 0.0f;
}

float hardware_principled_reflection_layer_visibility(bool principled_layered_scene_final,
                                                      bool proxy_payload,
                                                      float3 N,
                                                      float3 V,
                                                      ClosureUndetermined specular_cl,
                                                      float metallic_coverage)
{
  if (!principled_layered_scene_final) {
    return 1.0f;
  }
  if (specular_cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) {
    if (proxy_payload) {
      /* The bounded proxy carries only one closure family. Scale the reflection lobe by the
       * sync-time metallic coverage so intermediate Principled metallic values blend with the
       * synthesized diffuse base instead of snapping to full metal at the first non-zero step. */
      return saturate(metallic_coverage);
    }
    /* Replay shader graph already weights the metal lobe by the metallic factor. */
    return 1.0f;
  }
  return hardware_hit_reflection_layer_opacity(specular_cl);
}

float hardware_principled_base_layer_visibility(bool principled_layered_scene_final,
                                                bool proxy_payload,
                                                float metallic_coverage)
{
  if (proxy_payload && principled_layered_scene_final) {
    /* Proxy payload synthesizes the diffuse base lobe for layered Principled. Weight it by
     * (1 - metallic) to mirror the BRDF blend the replay shader graph applies natively. */
    return saturate(1.0f - metallic_coverage);
  }
  /* The replayed Principled diffuse closure is already attenuated by metallic. */
  return 1.0f;
}

ClosureUndetermined hardware_principled_metal_tinted_specular(
    bool principled_layered_scene_final,
    ClosureUndetermined specular_cl,
    ClosureUndetermined base_cl,
    float metallic_coverage)
{
  if (!principled_layered_scene_final ||
      (specular_cl.type != CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID))
  {
    return specular_cl;
  }
  /* The material hit-eval replay already packed the metal-only Principled reflection. */
  return specular_cl;
}

ClosureUndetermined hardware_hit_refracted_metal_direct_closure(ClosureUndetermined cl,
                                                                bool refracted_textured_receiver)
{
  if (refracted_textured_receiver && cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) {
    /* Analytic lights are not geometry in the Metal acceleration structure. Keep the closure on
     * the metal/specular path, but avoid a delta-like lobe becoming black except at the tiny
     * reflected-light highlight. */
    cl.data.x = max(cl.data.x, 0.25f);
  }
  return cl;
}

bool hardware_hit_preserves_screen_baseline(ClosureUndetermined primary_closure)
{
  return hardware_hit_specular_mode(primary_closure) != RAYTRACE_SPECULAR_MODE_FULL_RT;
}

bool hardware_receiver_gi_primary_is_mirror_like(int2 texel_fullres)
{
  /* Nuru Secondary GI scope: receiver GI lights diffuse surfaces seen through MIRROR-LIKE
   * scene-final reflections/refractions, where the reflected world otherwise fails closed to
   * black. Rough lobes keep their existing probe/analytic transport: feeding the traced field
   * there double-counts GI (the closed-room calibration repro doubled when the field went live
   * for every Principled specular lobe). Threshold matches the caustics mirror gate. */
  ClosureUndetermined primary_cl = gbuffer::read_bin(texel_fullres, closure_index);
  if (hardware_hit_specular_mode(primary_cl) != RAYTRACE_SPECULAR_MODE_FULL_RT) {
    return false;
  }
  return closure_apparent_roughness_get(primary_cl) < 0.32f;
}

bool hardware_primary_surface_has_full_rt_specular(int2 texel_fullres)
{
  const gbuffer::Layers gbuf = gbuffer::read_layers(texel_fullres);
  const uchar closure_count = gbuf.header.closure_len();
  for (uchar i = 0; i < GBUFFER_LAYER_MAX && i < closure_count; i++) {
    const ClosureUndetermined cl = gbuf.layer_get(i);
    if (((cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) ||
         (cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID)) &&
        (hardware_hit_specular_mode(cl) == RAYTRACE_SPECULAR_MODE_FULL_RT))
    {
      return true;
    }
  }
  return false;
}

bool hardware_hit_allows_scene_final_raster_reuse(int2 texel_fullres)
{
  if (hardware_trace_phase != HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR) {
    return true;
  }
  /* Fail closed for the whole primary surface, not only the currently resolved closure bin.
   * Full RT specular pixels can still carry extra base/specular bins in the GBuffer, and letting
   * one of those bins opt back into raster reuse reintroduces camera-relative reflected/refracted
   * patterns even though the visible primary closure is already in Full RT mode. */
  return !hardware_primary_surface_has_full_rt_specular(texel_fullres);
}

bool hardware_hit_uses_caustics()
{
  return false /* Optional Nuru caustics are outside the core port. */;
}

float3 hardware_caustics_load(int2 texel_fullres)
{
  return float3(0.0f); /* Caustics disabled: keep the core path within eight storage images. */
}

bool hardware_ray_load(int2 texel,
                       int2 &texel_fullres,
                       float4 &ray_data_im,
                       float &ray_time)
{
  if (any(lessThan(texel, int2(0))) || any(greaterThanEqual(texel, imageSize(ray_data_img).xy))) {
    return false;
  }

  ray_data_im = imageLoadFast(ray_data_img, texel);
  ray_time = imageLoadFast(ray_time_img, texel).r;
  if (ray_data_im.w == 0.0f) {
    return false;
  }

  texel_fullres = raytrace_texel_to_fullres(texel,
                                            raytrace_buf.trace_pixel_scale,
                                            1,
                                            raytrace_buf.trace_pixel_offset);
  if (false && (raytrace_buf.trace_pixel_scale > 1)) {
    texel_fullres = raytrace_representative_fullres_texel(
        texel,
        raytrace_buf.trace_pixel_scale,
        1,
        raytrace_buf.trace_pixel_offset);
  }
  if (any(lessThan(texel_fullres, int2(0))) ||
      any(greaterThanEqual(texel_fullres, textureSize(depth_tx, 0))))
  {
    return false;
  }

  return true;
}

float3 hardware_direction_unpack(float2 packed_dir)
{
  packed_dir = packed_dir * 2.0f - 1.0f;
  float3 dir = float3(
      packed_dir.x, packed_dir.y, 1.0f - abs(packed_dir.x) - abs(packed_dir.y));
  float t = clamp(-dir.z, 0.0f, 1.0f);
  dir.x += (dir.x >= 0.0f) ? -t : t;
  dir.y += (dir.y >= 0.0f) ? -t : t;
  return normalize(dir);
}

bool hardware_hit_direction_load(int2 texel, float3 &ray_direction)
{
  float2 packed_dir = float2(imageLoadFast(hit_material_img, texel).w,
                             imageLoadFast(hit_normal_img, texel).w);
  if (all(equal(packed_dir, float2(0.0f)))) {
    return false;
  }
  ray_direction = hardware_direction_unpack(packed_dir);
  return isfinite(ray_direction.x) && isfinite(ray_direction.y) && isfinite(ray_direction.z) &&
         dot(ray_direction, ray_direction) > 1.0e-10f;
}

ClosureType hardware_hit_closure_type_unpack(float packed_type)
{
  return ClosureType(uint(max(packed_type, 0.0f) + 0.5f));
}

bool hardware_hit_closure_is_specular_family(ClosureType type)
{
  return (type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) ||
         (type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
}

bool hardware_hit_closure_is_base_family(ClosureType type)
{
  return (type == CLOSURE_BSDF_DIFFUSE_ID) || (type == CLOSURE_BSDF_TRANSLUCENT_ID) ||
         (type == CLOSURE_BSSRDF_BURLEY_ID);
}

bool hardware_closure_has_transmission(ClosureType type)
{
  return (type == CLOSURE_BSDF_TRANSLUCENT_ID) ||
         (type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) ||
         (type == CLOSURE_BSSRDF_BURLEY_ID);
}

bool hardware_hit_uses_proxy_payload(int2 texel)
{
  return imageLoadFast(hit_albedo_img, texel).a < 0.0f;
}

bool hardware_hit_load(int2 texel, float3 &P_hit, float3 &V)
{
  int2 texel_fullres;
  float4 ray_data_im;
  float ray_time;
  if (!hardware_ray_load(texel, texel_fullres, ray_data_im, ray_time) || ray_time <= 0.0f) {
    return false;
  }

  float depth = reverse_z::read(texelFetch(depth_tx, texel_fullres, 0).r);
  if (!(depth > 0.0f && depth < 1.0f)) {
    return false;
  }

  float2 uv = (float2(texel_fullres) + 0.5f) * raytrace_buf.full_resolution_inv;
  float3 ray_direction = normalize(ray_data_im.xyz);
  if (!hardware_hit_direction_load(texel, ray_direction)) {
    ray_direction = normalize(ray_data_im.xyz);
  }
  P_hit = texelFetch(hit_world_position_tx, texel, 0).xyz;
  if (!(dot(P_hit, P_hit) > 1.0e-10f)) {
    float3 P = drw_point_screen_to_world(float3(uv, depth));
    P_hit = P + ray_direction * ray_time;
  }
  V = -ray_direction;
  return true;
}

bool hardware_hit_normal_load(int2 texel, float3 &N)
{
  N = imageLoadFast(hit_normal_img, texel).rgb;
  return dot(N, N) > 1.0e-10f;
}

bool hardware_hit_shadow_payload_valid(int2 texel)
{
  float3 shadow_N = imageLoadFast(hit_normal_img, texel).rgb;
  if (!(isfinite(shadow_N.x) && isfinite(shadow_N.y) && isfinite(shadow_N.z)) ||
      dot(shadow_N, shadow_N) <= 1.0e-10f)
  {
    return false;
  }

  float3 shadow_P = texelFetch(hit_world_position_tx, texel, 0).xyz;
  return isfinite(shadow_P.x) && isfinite(shadow_P.y) && isfinite(shadow_P.z) &&
         dot(shadow_P, shadow_P) > 1.0e-10f;
}

bool hardware_hit_is_preserved_layered_scene_final(int2 texel)
{
  return (imageLoadFast(hit_identity_img, texel).z & 2u) != 0u;
}

bool hardware_hit_is_preserved_transparent_scene_final(int2 texel)
{
  return (imageLoadFast(hit_identity_img, texel).z & 4u) != 0u;
}

float4 hardware_hit_transmission_layer_load(int2 texel)
{
  return texelFetch(hit_transmission_layer_tx, texel, 0);
}

/* Nuru: plain GLSL has no default struct constructor (`ObjectInfos()` only compiles through the
 * MSL path); zero every field explicitly so both backends agree on the miss value. */
ObjectInfos hardware_object_infos_zero()
{
  ObjectInfos object_infos;
  object_infos.orco_add = float3(0.0f);
  object_infos.object_attrs_offset = 0u;
  object_infos.orco_mul = float3(0.0f);
  object_infos.object_attrs_len = 0u;
  object_infos.ob_color = float4(0.0f);
  object_infos.index = 0u;
  object_infos.light_and_shadow_set_membership = 0u;
  object_infos.random = 0.0f;
  object_infos.flag = eObjectInfoFlag(0u);
  object_infos.shadow_terminator_normal_offset = 0.0f;
  object_infos.shadow_terminator_geometry_offset = 0.0f;
  object_infos._pad1 = 0.0f;
  object_infos._pad2 = 0.0f;
  return object_infos;
}

bool hardware_hit_object_infos_load(int2 texel, ObjectInfos &object_infos)
{
  const uint resource_id = imageLoadFast(hit_identity_img, texel).w;
  if (resource_id == 0xFFFFFFFFu) {
    object_infos = hardware_object_infos_zero();
    return false;
  }
  object_infos = hardware_object_infos_get(resource_id);
  return true;
}

bool hardware_hit_visible_surface_position_matches(float3 P_hit,
                                                   int2 lookup_texel,
                                                   float lookup_depth)
{
  const int2 extent = textureSize(depth_tx, 0);
  const int2 lookup_texel_x = min(lookup_texel + int2(1, 0), extent - 1);
  const int2 lookup_texel_y = min(lookup_texel + int2(0, 1), extent - 1);

  const float2 lookup_uv = (float2(lookup_texel) + 0.5f) / float2(extent);
  const float2 lookup_uv_x = (float2(lookup_texel_x) + 0.5f) / float2(extent);
  const float2 lookup_uv_y = (float2(lookup_texel_y) + 0.5f) / float2(extent);

  const float3 lookup_P = drw_point_screen_to_world(float3(lookup_uv, lookup_depth));
  const float3 lookup_Px = drw_point_screen_to_world(float3(lookup_uv_x, lookup_depth));
  const float3 lookup_Py = drw_point_screen_to_world(float3(lookup_uv_y, lookup_depth));

  const float footprint = max(length(lookup_Px - lookup_P), length(lookup_Py - lookup_P));
  const float position_tolerance_scale =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR) ? 3.0f :
                                                                                            1.5f;
  const float position_tolerance = max(footprint * position_tolerance_scale, 1.0e-3f);
  return distance(lookup_P, P_hit) <= position_tolerance;
}

bool hardware_hit_visible_surface_lookup_matches(int2 texel,
                                                 int2 lookup_texel,
                                                 float3 P_hit,
                                                 float3 N_hit,
                                                 bool allow_opposite_normals)
{
  if (any(lessThan(lookup_texel, int2(0))) ||
      any(greaterThanEqual(lookup_texel, textureSize(depth_tx, 0))))
  {
    return false;
  }

  float lookup_depth = reverse_z::read(texelFetch(depth_tx, lookup_texel, 0).r);
  if (!(lookup_depth > 0.0f && lookup_depth < 1.0f)) {
    return false;
  }

  const uint hit_object_id = imageLoadFast(hit_identity_img, texel).w;
  const gbuffer::Header gbuf_header = gbuffer::read_header(lookup_texel);
  const bool position_matches = hardware_hit_visible_surface_position_matches(
      P_hit, lookup_texel, lookup_depth);
  if (gbuf_header.use_object_id()) {
    const uint visible_object_id = gbuffer::read_object_id(lookup_texel);
    /* Sparse replay resolves real hits to a concrete object id in `hit_identity.w` after compact,
     * but only replayed payloads clear the proxy albedo marker. Miss payloads only preserve the last
     * specular surface point/normal. Let those miss payloads fall back to a normal-only match so
     * scene-final specular can still reuse the directly visible raster sample of that last metal/glass
     * surface. */
    if ((visible_object_id == 0u) ||
        (!hardware_hit_uses_proxy_payload(texel) && (visible_object_id != hit_object_id)))
    {
      return false;
    }
  }
  else if (!position_matches) {
    /* Ordinary opaque surfaces often skip the optional object-id payload entirely. Use a tight
     * position check in that case so visible floors and walls can still seed scene-final specular
     * from the already rendered raster sample instead of falling back to coarse replay lighting. */
    return false;
  }

  if (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR) {
    /* The late mirror/refraction resolve should prefer the already composed visible surface once
     * the projected hit lands on the right object/position. Requiring close normal agreement here
     * rejects valid curved-surface matches and pushes the pixel back into the coarse hit-lighting
     * fallback, which is exactly the dark/noisy artifact seen on reflected spheres and floor
     * patches. */
    return position_matches;
  }

  const gbuffer::Layers gbuf = gbuffer::read_layers(lookup_texel);
  const float normal_alignment = dot(gbuf.surface_N(), N_hit);
  return allow_opposite_normals ? abs(normal_alignment) > 0.8f : normal_alignment > 0.8f;
}

bool hardware_hit_visible_surface_lookup_refine(int2 texel,
                                                int2 lookup_texel,
                                                float3 P_hit,
                                                float3 N_hit,
                                                bool allow_opposite_normals,
                                                int2 &refined_lookup_texel)
{
  const bool scene_final_specular_phase =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
  const int search_radius = scene_final_specular_phase ? (allow_opposite_normals ? 8 : 6) :
                                                         (allow_opposite_normals ? 2 : 1);
  for (int y = -search_radius; y <= search_radius; y++) {
    for (int x = -search_radius; x <= search_radius; x++) {
      const int2 candidate = lookup_texel + int2(x, y);
      if (hardware_hit_visible_surface_lookup_matches(
              texel, candidate, P_hit, N_hit, allow_opposite_normals))
      {
        refined_lookup_texel = candidate;
        return true;
      }
    }
  }
  return false;
}

bool hardware_hit_visible_surface_lookup_texel_load(int2 texel,
                                                    float3 P_hit,
                                                    float3 N_hit,
                                                    bool allow_opposite_normals,
                                                    float2 &lookup_uv,
                                                    int2 &lookup_texel)
{
  lookup_uv = float2(0.0f);
  lookup_texel = int2(0);

  float4 ray_rad = imageLoadFast(ray_radiance_img, texel);
  uint packed_uv = floatBitsToUint(ray_rad.a);
  if (packed_uv != 0u) {
    lookup_uv = float2(float(packed_uv & 0xFFFFu) / 65535.0f,
                       float((packed_uv >> 16u) & 0xFFFFu) / 65535.0f);
    if (all(greaterThanEqual(lookup_uv, float2(0.0f))) && all(lessThan(lookup_uv, float2(1.0f)))) {
      lookup_texel = clamp(int2(lookup_uv * float2(textureSize(depth_tx, 0))),
                           int2(0),
                           textureSize(depth_tx, 0) - 1);
      if (hardware_hit_visible_surface_lookup_matches(
              texel, lookup_texel, P_hit, N_hit, allow_opposite_normals) ||
          hardware_hit_visible_surface_lookup_refine(
              texel, lookup_texel, P_hit, N_hit, allow_opposite_normals, lookup_texel))
      {
        /* Scene-final mirror/refraction is only allowed to replace the pixel from raster when the
         * projected hit still validates against the visible resolved surface. */
        lookup_uv = (float2(lookup_texel) + 0.5f) / float2(textureSize(depth_tx, 0));
        return true;
      }
    }
  }

  float3 screen_P = drw_point_world_to_screen(P_hit);
  lookup_uv = screen_P.xy;
  if (any(lessThan(lookup_uv, float2(0.0f))) || any(greaterThanEqual(lookup_uv, float2(1.0f)))) {
    return false;
  }

  lookup_texel = clamp(int2(lookup_uv * float2(textureSize(depth_tx, 0))),
                       int2(0),
                       textureSize(depth_tx, 0) - 1);
  if (hardware_hit_visible_surface_lookup_matches(
          texel, lookup_texel, P_hit, N_hit, allow_opposite_normals) ||
      hardware_hit_visible_surface_lookup_refine(
          texel, lookup_texel, P_hit, N_hit, allow_opposite_normals, lookup_texel))
  {
    lookup_uv = (float2(lookup_texel) + 0.5f) / float2(textureSize(depth_tx, 0));
    return true;
  }
  return false;
}

bool hardware_hit_environment_visibility_load(int2 texel,
                                              float3 fallback_N,
                                              HardwareEnvironmentVisibilityData &data)
{
  if (any(lessThan(texel, int2(0))) ||
      any(greaterThanEqual(texel, textureSize(hardware_rt_hit_environment_visibility_tx, 0))))
  {
    data.average_direction = float3(0.0f);
    data.visibility = 1.0f;
    data.validity = 0.0f;
    return false;
  }

  float4 visibility_data = texelFetch(hardware_rt_hit_environment_visibility_tx, texel, 0);
  data.average_direction = visibility_data.xyz;
  data.visibility = saturate(visibility_data.w);
  data.validity = hardware_environment_visibility_validity(data.average_direction, data.visibility);
  if (data.validity < 0.5f) {
    data.visibility = 1.0f;
  }
  if (dot(data.average_direction, data.average_direction) <= 1.0e-8f) {
    data.average_direction = safe_normalize(fallback_N) * (2.0f / 3.0f);
  }
  return hardware_environment_visibility_is_valid(data);
}

bool hardware_hit_visible_surface_uses_back_radiance(int2 lookup_texel)
{
  const gbuffer::Layers gbuf = gbuffer::read_layers(lookup_texel);
  const uchar closure_count = gbuf.header.closure_len();
  for (uchar i = 0; i < GBUFFER_LAYER_MAX && i < closure_count; i++) {
    const ClosureUndetermined cl = gbuf.layer_get(i);
    if (hardware_closure_has_transmission(cl.type)) {
      return true;
    }
  }
  return false;
}

bool hardware_hit_is_visible_to_main_camera(float3 P_hit)
{
  float3 screen_P = drw_point_world_to_screen(P_hit);
  float2 lookup_uv = screen_P.xy;
  if (any(lessThan(lookup_uv, float2(0.0f))) || any(greaterThanEqual(lookup_uv, float2(1.0f)))) {
    return false;
  }

  int2 lookup_texel = clamp(int2(lookup_uv * float2(textureSize(depth_tx, 0))),
                            int2(0),
                            textureSize(depth_tx, 0) - 1);
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      int2 candidate = clamp(lookup_texel + int2(x, y), int2(0), textureSize(depth_tx, 0) - 1);
      float lookup_depth = reverse_z::read(texelFetch(depth_tx, candidate, 0).r);
      if ((lookup_depth > 0.0f && lookup_depth < 1.0f) &&
          hardware_hit_visible_surface_position_matches(P_hit, candidate, lookup_depth))
      {
        return true;
      }
    }
  }
  return false;
}

bool hardware_hit_raster_radiance_load(int2 texel,
                                       float3 P_hit,
                                       float3 N_hit,
                                       bool allow_opposite_normals,
                                       bool strip_receiver_caustics,
                                       float3 &radiance)
{
  radiance = float3(0.0f);

  float2 lookup_uv;
  int2 lookup_texel;
  if (!hardware_hit_visible_surface_lookup_texel_load(
          texel, P_hit, N_hit, allow_opposite_normals, lookup_uv, lookup_texel))
  {
    return false;
  }

  if (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR) {
    radiance = hardware_hit_visible_surface_uses_back_radiance(lookup_texel) ?
                   texelFetch(radiance_back_tx, lookup_texel, 0).rgb :
                   texelFetch(radiance_front_tx, lookup_texel, 0).rgb;
  }
  else {
    radiance = hardware_hit_visible_surface_uses_back_radiance(lookup_texel) ?
                   textureLod(radiance_back_tx, lookup_uv, 0.0f).rgb :
                   textureLod(radiance_front_tx, lookup_uv, 0.0f).rgb;
  }
  if (strip_receiver_caustics) {
    /* Keep sharp visible-surface replay for layered Principled reflections, but do not fold the
     * receiver-only caustics buffer back into that rough diffuse-reflection handoff. */
    radiance = max(radiance - hardware_caustics_load(lookup_texel), float3(0.0f));
  }
  return true;
}

bool hardware_hit_visible_direct_light_load(int2 texel,
                                            float3 P_hit,
                                            float3 N_hit,
                                            bool allow_opposite_normals,
                                            float3 &radiance)
{
  radiance = float3(0.0f);

  float2 lookup_uv;
  int2 lookup_texel;
  if (!hardware_hit_visible_surface_lookup_texel_load(
          texel, P_hit, N_hit, allow_opposite_normals, lookup_uv, lookup_texel))
  {
    return false;
  }

  const int2 direct_extent = textureSize(hardware_direct_light_tx, 0);
  if (any(lessThan(lookup_texel, int2(0))) || any(greaterThanEqual(lookup_texel, direct_extent))) {
    return false;
  }

  radiance = texelFetch(hardware_direct_light_tx, lookup_texel, 0).rgb;
  return dot(radiance, radiance) > 1.0e-10f;
}

bool hardware_hit_raster_back_radiance_load(int2 texel,
                                            float3 P_hit,
                                            float3 N_hit,
                                            bool allow_opposite_normals,
                                            bool strip_receiver_caustics,
                                            float3 &radiance)
{
  radiance = float3(0.0f);

  float2 lookup_uv;
  int2 lookup_texel;
  if (!hardware_hit_visible_surface_lookup_texel_load(
          texel, P_hit, N_hit, allow_opposite_normals, lookup_uv, lookup_texel))
  {
    return false;
  }

  radiance = (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR) ?
                 texelFetch(radiance_back_tx, lookup_texel, 0).rgb :
                 textureLod(radiance_back_tx, lookup_uv, 0.0f).rgb;
  if (strip_receiver_caustics) {
    /* Keep the transmission fallback replay-owned, but do not feed the late receiver-only
     * caustics buffer back through `radiance_back_tx` when caustics are enabled. */
    radiance = max(radiance - hardware_caustics_load(lookup_texel), float3(0.0f));
  }
  return true;
}

bool hardware_receiver_needs_textured_raster_fallback(int2 texel, bool is_transmission_receiver)
{
  const uint identity_flags = is_transmission_receiver ?
                                  texelFetch(transmission_receiver_hit_identity_tx, texel, 0).z :
                                  texelFetch(layered_receiver_hit_identity_tx, texel, 0).z;
  const bool proxy_payload = is_transmission_receiver ?
                                 transmission_receiver_hit_uses_proxy_payload(texel) :
                                 layered_receiver_hit_uses_proxy_payload(texel);
  const bool scene_final_specular_phase =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
  return scene_final_specular_phase && proxy_payload &&
         (((identity_flags & 2u) != 0u) || ((identity_flags & 16u) != 0u));
}

bool hardware_layered_receiver_is_continuation_payload(int2 texel)
{
  if (!layered_receiver_hit_exists(texel)) {
    return false;
  }
  const float4 hit_base = texelFetch(layered_receiver_hit_albedo_tx, texel, 0);
  if (hit_base.a < 0.0f) {
    return true;
  }
  const ClosureType base_type = hardware_hit_closure_type_unpack(hit_base.a);
  const ClosureType specular_type = hardware_hit_closure_type_unpack(
      texelFetch(layered_receiver_hit_material_tx, texel, 0).z);
  return base_type != CLOSURE_NONE_ID || specular_type != CLOSURE_NONE_ID;
}

bool hardware_scene_final_suppress_layered_receiver_on_textured_specular(
    bool preserved_layered_scene_final,
    bool principled_layered_scene_final,
    int2 texel)
{
  /* Glossy/Glass scene-final preservation keeps the exported hit on the textured ball while the
   * continuation payload still records what the ball reflected. Compositing that receiver back in
   * replaces the replayed texture with mirror/floor patches on reflected proxies only. */
  return preserved_layered_scene_final && !principled_layered_scene_final &&
         hardware_layered_receiver_is_continuation_payload(texel);
}

bool hardware_scene_final_is_textured_specular_replay(
    bool scene_final_specular_phase,
    bool primary_is_diffuse_gi,
    bool preserved_layered_scene_final,
    bool principled_layered_scene_final,
    uint hit_identity_flags)
{
  /* Only the mirrored scene-final proxy path uses this replay contract. Direct-view refraction
   * stays on the normal material path and must not be routed through this branch. */
  return scene_final_specular_phase && !primary_is_diffuse_gi &&
         preserved_layered_scene_final && ((hit_identity_flags & 16u) != 0u);
}

bool hardware_scene_final_has_sparse_material_replay(int2 texel)
{
  /* Compact hit-eval stamps `hit_identity.w` before the surf replay runs. Only treat the hit as
   * replayed once the exported albedo payload is no longer the trace-time proxy marker. */
  return !hardware_hit_uses_proxy_payload(texel);
}

float3 hardware_scene_final_outer_texture_color(ClosureUndetermined base_cl,
                                                ClosureUndetermined specular_cl)
{
  float3 texture_color = max(base_cl.color, float3(0.0f));
  if (!(dot(texture_color, texture_color) > 1.0e-10f)) {
    texture_color = max(specular_cl.color, float3(0.0f));
  }
  if (!(dot(texture_color, texture_color) > 1.0e-10f)) {
    texture_color = float3(1.0f);
  }
  return texture_color;
}

bool hardware_scene_final_raster_radiance_at_world_position(float3 P_hit, float3 &radiance)
{
  radiance = float3(0.0f);

  float4 hpos = drw_point_world_to_homogenous(P_hit);
  if (!(abs(hpos.w) > 1.0e-8f)) {
    return false;
  }

  float2 ndc = hpos.xy / hpos.w;
  if (any(greaterThan(abs(ndc), float2(1.0f)))) {
    return false;
  }

  const int2 extent = textureSize(depth_tx, 0);
  float2 uv = ndc * 0.5f + 0.5f;
  int2 lookup_texel = int2(uv * float2(extent));
  lookup_texel = clamp(lookup_texel, int2(0), extent - 1);

  const int search_radius = 6;
  for (int y = -search_radius; y <= search_radius; y++) {
    for (int x = -search_radius; x <= search_radius; x++) {
      const int2 candidate = clamp(lookup_texel + int2(x, y), int2(0), extent - 1);
      const float candidate_depth = reverse_z::read(texelFetch(depth_tx, candidate, 0).r);
      if (!(candidate_depth > 0.0f && candidate_depth < 1.0f)) {
        continue;
      }
      if (!hardware_hit_visible_surface_position_matches(P_hit, candidate, candidate_depth)) {
        continue;
      }
      const float3 candidate_front = texelFetch(radiance_front_tx, candidate, 0).rgb;
      const float3 candidate_back = texelFetch(radiance_back_tx, candidate, 0).rgb;
      if (dot(candidate_front, candidate_front) > 1.0e-10f) {
        radiance = candidate_front;
        return true;
      }
      if (dot(candidate_back, candidate_back) > 1.0e-10f) {
        radiance = candidate_back;
        return true;
      }
    }
  }

  return false;
}

bool hardware_reflected_receiver_gi_load(int2 texel, float3 &radiance)
{
  float4 gi = texelFetch(hardware_reflected_receiver_gi_tx, texel, 0);
  radiance = max(gi.rgb, float3(0.0f));
  return (gi.a > 0.5f) && (dot(radiance, radiance) > 1.0e-10f);
}

bool hardware_layered_receiver_gi_load(int2 texel, float3 &radiance)
{
  float4 gi = texelFetch(hardware_layered_receiver_gi_tx, texel, 0);
  radiance = max(gi.rgb, float3(0.0f));
  return (gi.a > 0.5f) && (dot(radiance, radiance) > 1.0e-10f);
}

bool hardware_transmission_receiver_gi_load(int2 texel, float3 &radiance)
{
  float4 gi = texelFetch(hardware_transmission_receiver_gi_tx, texel, 0);
  radiance = max(gi.rgb, float3(0.0f));
  return (gi.a > 0.5f) && (dot(radiance, radiance) > 1.0e-10f);
}

bool hardware_secondary_photon_gi_load(int2 texel, float3 &radiance)
{
  float4 gi = texelFetch(hardware_secondary_photon_gi_tx, texel, 0);
  radiance = max(gi.rgb, float3(0.0f));
  return (gi.a > 0.5f) && (dot(radiance, radiance) > 1.0e-10f);
}

bool hardware_layered_secondary_photon_gi_load(int2 texel, float3 &radiance)
{
  float4 gi = texelFetch(hardware_layered_secondary_photon_gi_tx, texel, 0);
  radiance = max(gi.rgb, float3(0.0f));
  return (gi.a > 0.5f) && (dot(radiance, radiance) > 1.0e-10f);
}

bool hardware_transmission_secondary_photon_gi_load(int2 texel, float3 &radiance)
{
  float4 gi = texelFetch(hardware_transmission_secondary_photon_gi_tx, texel, 0);
  radiance = max(gi.rgb, float3(0.0f));
  return (gi.a > 0.5f) && (dot(radiance, radiance) > 1.0e-10f);
}

/* Indirect/base-family simplification:
 * - proxy payloads keep at most one base-family lobe,
 * - subsurface collapses to diffuse,
 * - specular-family proxy closures are dropped from the base/indirect path. */
ClosureUndetermined hardware_hit_base_closure_load(int2 texel, float3 N)
{
  float4 hit_base = imageLoadFast(hit_albedo_img, texel);
  float4 hit_material = imageLoadFast(hit_material_img, texel);
  const bool proxy_payload = hardware_hit_uses_proxy_payload(texel);
  ClosureType type = proxy_payload ? hardware_hit_closure_type_unpack(hit_material.z) :
                                     hardware_hit_closure_type_unpack(hit_base.a);
  if (proxy_payload && !hardware_hit_closure_is_base_family(type)) {
    /* Principled layered proxies must still contribute a diffuse base lobe so intermediate
     * metallic values blend with the reflection lobe instead of snapping to full metal. */
    const uint identity_flags = imageLoadFast(hit_identity_img, texel).z;
    if (hardware_scene_final_is_principled_layered(identity_flags) &&
        (type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID))
    {
      type = CLOSURE_BSDF_DIFFUSE_ID;
    }
    else {
      type = CLOSURE_NONE_ID;
    }
  }
  if (type == CLOSURE_BSSRDF_BURLEY_ID) {
    type = CLOSURE_BSDF_DIFFUSE_ID;
  }

  ClosureUndetermined cl = closure_new(type);
  cl.weight = 1.0f;
  cl.color = hit_base.rgb;
  cl.N = N;
  cl.data = float4(0.0f);

  switch (cl.type) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      cl.data.x = hit_material.x;
      cl.data.y = hit_material.y;
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      cl.data.x = hit_material.x;
      cl.data.y = hit_material.y;
      break;
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_NONE_ID:
      break;
  }
  return cl;
}

/* Direct/specular simplification:
 * - replay or proxy fallback keeps at most one dominant specular-family lobe,
 * - proxy payloads reuse the coarse base tint as the bounded fallback color,
 * - proxy-only hits do not carry thickness. */
ClosureUndetermined hardware_hit_specular_closure_load(int2 texel, float3 N)
{
  float4 hit_base = imageLoadFast(hit_albedo_img, texel);
  float4 hit_material = imageLoadFast(hit_material_img, texel);
  float4 hit_specular = imageLoadFast(hit_position_img, texel);
  const bool proxy_payload = hardware_hit_uses_proxy_payload(texel);
  const ClosureType type = hardware_hit_closure_type_unpack(hit_material.z);

  ClosureUndetermined cl = closure_new(
      hardware_hit_closure_is_specular_family(type) ? type : CLOSURE_NONE_ID);
  cl.weight = 1.0f;
  cl.color = proxy_payload ? hit_base.rgb : hit_specular.rgb;
  cl.N = N;
  cl.data = float4(0.0f);

  switch (cl.type) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      cl.data.x = hit_material.x;
      cl.data.y = hit_material.y;
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      cl.data.x = hit_material.x;
      cl.data.y = hit_material.y;
      break;
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_NONE_ID:
      break;
  }

  return cl;
}

float hardware_hit_thickness_load(int2 texel)
{
  if (hardware_hit_uses_proxy_payload(texel)) {
    return 0.0f;
  }
  return hardware_thickness_unpack(imageLoadFast(hit_position_img, texel).w);
}

bool layered_receiver_hit_exists(int2 texel)
{
  return texelFetch(layered_receiver_ray_time_tx, texel, 0).r > 0.0f;
}

float4 layered_receiver_hit_throughput_load(int2 texel)
{
  return texelFetch(layered_receiver_throughput_tx, texel, 0);
}

bool layered_receiver_hit_uses_proxy_payload(int2 texel)
{
  return texelFetch(layered_receiver_hit_albedo_tx, texel, 0).a < 0.0f;
}

bool layered_receiver_hit_direction_load(int2 texel, float3 &ray_direction)
{
  float2 packed_dir = float2(texelFetch(layered_receiver_hit_material_tx, texel, 0).w,
                             texelFetch(layered_receiver_hit_normal_tx, texel, 0).w);
  if (all(equal(packed_dir, float2(0.0f)))) {
    return false;
  }
  ray_direction = hardware_direction_unpack(packed_dir);
  return isfinite(ray_direction.x) && isfinite(ray_direction.y) && isfinite(ray_direction.z) &&
         dot(ray_direction, ray_direction) > 1.0e-10f;
}

bool layered_receiver_hit_load(int2 texel, float3 &P_hit, float3 &V)
{
  if (!layered_receiver_hit_exists(texel)) {
    return false;
  }
  float3 ray_direction;
  if (!layered_receiver_hit_direction_load(texel, ray_direction)) {
    return false;
  }
  P_hit = texelFetch(layered_receiver_world_position_tx, texel, 0).xyz;
  if (!(isfinite(P_hit.x) && isfinite(P_hit.y) && isfinite(P_hit.z)) ||
      dot(P_hit, P_hit) <= 1.0e-10f)
  {
    return false;
  }
  V = -ray_direction;
  return true;
}

bool layered_receiver_hit_normal_load(int2 texel, float3 &N)
{
  N = texelFetch(layered_receiver_hit_normal_tx, texel, 0).rgb;
  return dot(N, N) > 1.0e-10f;
}

bool layered_receiver_hit_shadow_payload_valid(int2 texel)
{
  float3 shadow_N = texelFetch(layered_receiver_hit_normal_tx, texel, 0).rgb;
  if (!(isfinite(shadow_N.x) && isfinite(shadow_N.y) && isfinite(shadow_N.z)) ||
      dot(shadow_N, shadow_N) <= 1.0e-10f)
  {
    return false;
  }

  float3 shadow_P = texelFetch(layered_receiver_world_position_tx, texel, 0).xyz;
  return isfinite(shadow_P.x) && isfinite(shadow_P.y) && isfinite(shadow_P.z) &&
         dot(shadow_P, shadow_P) > 1.0e-10f;
}

bool layered_receiver_hit_object_infos_load(int2 texel, ObjectInfos &object_infos)
{
  const uint resource_id = texelFetch(layered_receiver_hit_identity_tx, texel, 0).w;
  if (resource_id == 0xFFFFFFFFu) {
    object_infos = hardware_object_infos_zero();
    return false;
  }
  object_infos = hardware_object_infos_get(resource_id);
  return true;
}

ClosureUndetermined layered_receiver_hit_base_closure_load(int2 texel, float3 N)
{
  float4 hit_base = texelFetch(layered_receiver_hit_albedo_tx, texel, 0);
  float4 hit_material = texelFetch(layered_receiver_hit_material_tx, texel, 0);
  const bool proxy_payload = layered_receiver_hit_uses_proxy_payload(texel);
  ClosureType type = proxy_payload ? hardware_hit_closure_type_unpack(hit_material.z) :
                                     hardware_hit_closure_type_unpack(hit_base.a);
  if (proxy_payload && !hardware_hit_closure_is_base_family(type)) {
    const uint identity_flags = texelFetch(layered_receiver_hit_identity_tx, texel, 0).z;
    if (hardware_scene_final_is_principled_layered(identity_flags) &&
        (type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID))
    {
      type = CLOSURE_BSDF_DIFFUSE_ID;
    }
    else {
      type = CLOSURE_NONE_ID;
    }
  }
  if (type == CLOSURE_BSSRDF_BURLEY_ID) {
    type = CLOSURE_BSDF_DIFFUSE_ID;
  }

  ClosureUndetermined cl = closure_new(type);
  cl.weight = 1.0f;
  cl.color = hit_base.rgb;
  cl.N = N;
  cl.data = float4(0.0f);

  switch (cl.type) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      cl.data.x = hit_material.x;
      cl.data.y = hit_material.y;
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      cl.data.x = hit_material.x;
      cl.data.y = hit_material.y;
      break;
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_NONE_ID:
      break;
  }
  return cl;
}

ClosureUndetermined layered_receiver_hit_specular_closure_load(int2 texel, float3 N)
{
  float4 hit_base = texelFetch(layered_receiver_hit_albedo_tx, texel, 0);
  float4 hit_material = texelFetch(layered_receiver_hit_material_tx, texel, 0);
  float4 hit_specular = texelFetch(layered_receiver_hit_position_tx, texel, 0);
  const bool proxy_payload = layered_receiver_hit_uses_proxy_payload(texel);
  const ClosureType type = hardware_hit_closure_type_unpack(hit_material.z);

  ClosureUndetermined cl = closure_new(
      hardware_hit_closure_is_specular_family(type) ? type : CLOSURE_NONE_ID);
  cl.weight = 1.0f;
  cl.color = proxy_payload ? hit_base.rgb : hit_specular.rgb;
  cl.N = N;
  cl.data = float4(0.0f);

  switch (cl.type) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      cl.data.x = hit_material.x;
      cl.data.y = hit_material.y;
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      cl.data.x = hit_material.x;
      cl.data.y = hit_material.y;
      break;
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_NONE_ID:
      break;
  }

  return cl;
}

float layered_receiver_hit_thickness_load(int2 texel)
{
  if (layered_receiver_hit_uses_proxy_payload(texel)) {
    return 0.0f;
  }
  return hardware_thickness_unpack(texelFetch(layered_receiver_hit_position_tx, texel, 0).w);
}

bool hardware_hit_closure_has_energy(ClosureUndetermined cl)
{
  return (cl.type != CLOSURE_NONE_ID) && (dot(cl.color, cl.color) > 1.0e-10f);
}

float hardware_hit_closure_color_strength(ClosureUndetermined cl)
{
  float3 color = abs(cl.color);
  return max(color.x, max(color.y, color.z));
}


/* -------------------------------------------------------------------------------------------
 * Nuru NIS stage G2: sampled many-light estimator for GI hit shading.
 *
 * With more than HWRT_HIT_EXACT_LOCAL_LIGHTS local lights, evaluating every light analytically
 * at every GI hit texel is O(N) in LTC + shadow work. Instead: one cheap O(N) importance scan
 * (position-aware, cluster-bucketed), then K full evaluations of lights picked by the
 * NIS-shaped two-stage PMF, each weighted by 1/(K * p(y)). Unbiased: every light keeps a
 * strictly positive pick probability (importance floors + clamped multipliers), so
 * E[sum_k f(y_k) / (K p(y_k))] = sum_y f(y). The picked lights run the exact same
 * light_eval_single (LTC + shadow rays) as the full loop, into a temporary stack that is
 * scale-added into the caller's stack. ------------------------------------------------------ */

#define HWRT_HIT_SAMPLED_LIGHT_COUNT 4

void hardware_hit_sampled_local_lights(int2 texel,
                                       const bool is_transmission,
                                       ClosureLightStack &stack,
                                       float3 P,
                                       float3 Ng,
                                       float3 V,
                                       float thickness,
                                       uchar receiver_light_set,
                                       float terminator_normal_offset,
                                       float terminator_geometry_offset)
{
  const uint local_lights_len = uint(hardware_local_lights_len);
  if (local_lights_len == 0u) {
    return;
  }
  /* Cheap position-aware importance scan, bucketed by cluster. */
  float cluster_sums[HWRT_LIGHT_CLUSTER_COUNT];
  for (int c = 0; c < HWRT_LIGHT_CLUSTER_COUNT; c++) {
    cluster_sums[c] = 0.0f;
  }
  for (uint l_idx = 0u; l_idx < local_lights_len; l_idx++) {
    const uint cluster_id = l_idx % uint(HWRT_LIGHT_CLUSTER_COUNT);
    cluster_sums[cluster_id] += hardware_direct_light_local_importance(l_idx, P, true);
  }
  float cluster_multipliers[HWRT_LIGHT_CLUSTER_COUNT];
  hardware_light_cluster_multipliers(P, true, cluster_multipliers);
  float weighted_total = 0.0f;
  for (int c = 0; c < HWRT_LIGHT_CLUSTER_COUNT; c++) {
    weighted_total += cluster_sums[c] * cluster_multipliers[c];
  }
  if (!(weighted_total > 0.0f)) {
    return;
  }

  for (int pick = 0; pick < HWRT_HIT_SAMPLED_LIGHT_COUNT; pick++) {
    const float rand_cluster = interleaved_gradient_noise(
        float2(texel) + 0.5f,
        float(7 + pick * 2),
        sampling_rng_1D_get(SAMPLING_RAYTRACE_U));
    const float rand_light = interleaved_gradient_noise(
        float2(texel) + 0.5f,
        float(8 + pick * 2),
        sampling_rng_1D_get(SAMPLING_RAYTRACE_X));

    /* Stage 1: cluster. */
    uint picked_cluster = 0u;
    {
      const float target = clamp(rand_cluster, 0.0f, 0.999999f) * weighted_total;
      float accum = 0.0f;
      for (int c = 0; c < HWRT_LIGHT_CLUSTER_COUNT; c++) {
        const float weighted = cluster_sums[c] * cluster_multipliers[c];
        if (weighted > 0.0f) {
          picked_cluster = uint(c);
        }
        accum += weighted;
        if (accum >= target && weighted > 0.0f) {
          break;
        }
      }
    }
    const float cluster_sum = cluster_sums[picked_cluster];
    if (!(cluster_sum > 0.0f)) {
      continue;
    }
    /* Stage 2: light within the cluster. */
    uint picked_light = 0xFFFFFFFFu;
    float picked_importance = 0.0f;
    {
      const float target = clamp(rand_light, 0.0f, 0.999999f) * cluster_sum;
      float accum = 0.0f;
      for (uint l_idx = 0u; l_idx < local_lights_len; l_idx++) {
        if (l_idx % uint(HWRT_LIGHT_CLUSTER_COUNT) != picked_cluster)
        {
          continue;
        }
        const float importance = hardware_direct_light_local_importance(l_idx, P, true);
        accum += importance;
        picked_light = l_idx;
        picked_importance = importance;
        if (accum >= target) {
          break;
        }
      }
    }
    if (picked_light == 0xFFFFFFFFu || !(picked_importance > 0.0f)) {
      continue;
    }
    const float pick_pdf = (cluster_multipliers[picked_cluster] * picked_importance) /
                           weighted_total;
    const float sample_weight = 1.0f /
                                (float(HWRT_HIT_SAMPLED_LIGHT_COUNT) * max(pick_pdf, 1.0e-8f));

    /* Full evaluation of the picked light into a temporary stack, scale-added back. */
    ClosureLightStack temp_stack = stack;
    for (int i = 0; i < LIGHT_CLOSURE_EVAL_COUNT; i++) {
      temp_stack.cl[i].light_shadowed = float3(0.0f);
      temp_stack.cl[i].light_unshadowed = float3(0.0f);
    }
    light_eval_single(picked_light,
                      false,
                      is_transmission,
                      temp_stack,
                      P,
                      Ng,
                      V,
                      thickness,
                      receiver_light_set,
                      terminator_normal_offset,
                      terminator_geometry_offset);
    for (int i = 0; i < LIGHT_CLOSURE_EVAL_COUNT; i++) {
      stack.cl[i].light_shadowed += temp_stack.cl[i].light_shadowed * sample_weight;
      stack.cl[i].light_unshadowed += temp_stack.cl[i].light_unshadowed * sample_weight;
    }
  }
}

bool hardware_hit_use_exact_local_lights()
{
  return (uint(hardware_local_lights_len) > 0u) && (uint(hardware_local_lights_len) <= 8u);
}

LightData hardware_hit_exact_local_light(uint local_light_index)
{
  return hardware_light_get(local_light_index);
}

void hardware_hit_light_eval_exact_local(uint local_light_index,
                                         const bool is_transmission,
                                         ClosureLightStack &stack,
                                         float3 P,
                                         float3 Ng,
                                         float3 V,
                                         float thickness,
                                         uchar receiver_light_set,
                                         float terminator_normal_offset,
                                         float terminator_geometry_offset)
{
  LightData light = hardware_hit_exact_local_light(local_light_index);

  if (!light_linking_affects_receiver(light.light_set_membership, receiver_light_set)) {
    return;
  }

#if defined(SPECIALIZED_SHADOW_PARAMS)
  int ray_count = shadow_ray_count;
  int ray_step_count = shadow_ray_step_count;
#else
  int ray_count = uniform_buf.shadow.ray_count;
  int ray_step_count = uniform_buf.shadow.step_count;
#endif

  LightVector lv = light_vector_get(light, false, P);
  bool is_translucent_with_thickness = is_transmission &&
                                       (stack.cl[0].type == LIGHT_TRANSLUCENT_WITH_THICKNESS);
  float attenuation = light_attenuation_surface(light, false, lv);

  if (!is_translucent_with_thickness) {
    attenuation *= light_attenuation_facing(light, lv.L, lv.dist, stack.cl[0].N, is_transmission);
  }

  if (attenuation < LIGHT_ATTENUATION_THRESHOLD) {
    return;
  }

  float3 shadow = float3(1.0f);
  bool evaluate_shadow = light.tilemap_index != LIGHT_NO_SHADOW;
#if defined(SHADOW_DISPATCH_HAS_HARDWARE_RT)
  evaluate_shadow = evaluate_shadow || (use_hardware_rt_shadows && light.cast_shadow);
#endif
  if (evaluate_shadow) {
    shadow = shadow_eval_dispatch(local_light_index,
                                  light,
                                  false,
                                  is_transmission,
                                  is_translucent_with_thickness,
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
    stack.cl[0].N = lv.L;
    attenuation *= M_1_PI;
  }

  light_eval_single_closure(light, lv, stack.cl[0], V, attenuation, shadow);
  if (!is_transmission) {
#if LIGHT_CLOSURE_EVAL_COUNT > 1
    light_eval_single_closure(light, lv, stack.cl[1], V, attenuation, shadow);
#endif
#if LIGHT_CLOSURE_EVAL_COUNT > 2
    light_eval_single_closure(light, lv, stack.cl[2], V, attenuation, shadow);
#endif
#if LIGHT_CLOSURE_EVAL_COUNT > 3
#  error
#endif
  }
}

bool hardware_hit_allows_raster_reuse(int2 texel,
                                      bool preserve_screen_baseline,
                                      bool has_replayed_material,
                                      float3 existing_radiance,
                                      ClosureUndetermined base_cl,
                                      ClosureUndetermined specular_cl)
{
  if (preserve_screen_baseline) {
    return false;
  }
  if (has_replayed_material) {
    return false;
  }
  if (dot(existing_radiance, existing_radiance) > 1.0e-10f) {
    return false;
  }
  return hardware_hit_closure_has_energy(base_cl) || hardware_hit_closure_has_energy(specular_cl);
}

bool hardware_hit_closure_uses_environment_visibility(ClosureUndetermined cl,
                                                      bool primary_is_diffuse_gi)
{
  /* Nuru: diffuse-GI primaries must not be excluded here. Their hit points still evaluate the
   * world probe for the hit material's specular closure; without the traced hit-domain
   * environment-visibility mask that term is unoccluded and sky/HDRI light floods enclosed
   * interiors through the GI signal whenever HWRT reflections/refractions enable the
   * environment feature. The hit visibility buffer is traced for every per-closure dispatch
   * (including GI), so it is always valid when `use_hardware_rt_environment_visibility` is. */
  return use_hardware_rt_environment_visibility;
}

void hardware_hit_closure_light_terms(int2 texel_fullres,
                                      int2 texel,
                                      float3 P_hit,
                                      float3 N,
                                      float3 V,
                                      ClosureUndetermined cl,
                                      float thickness,
                                      bool primary_is_diffuse_gi,
                                      float3 &direct_radiance,
                                      float3 &probe_radiance,
                                      bool &probe_uses_world)
{
  direct_radiance = float3(0.0f);
  probe_radiance = float3(0.0f);
  probe_uses_world = false;
  if (!hardware_hit_closure_has_energy(cl)) {
    return;
  }

  const bool scene_final_specular_phase =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
  const bool is_transmission = hardware_closure_has_transmission(cl.type);
  const bool is_diffuse_family = (cl.type == CLOSURE_BSDF_DIFFUSE_ID) ||
                                 (cl.type == CLOSURE_BSSRDF_BURLEY_ID);
  const bool scene_final_reflected_diffuse = scene_final_specular_phase && !primary_is_diffuse_gi &&
                                            is_diffuse_family &&
                                            hardware_receiver_gi_primary_is_mirror_like(
                                                texel_fullres);
  const uint hit_identity_flags = imageLoadFast(hit_identity_img, texel).z;
  const bool scene_final_replayed_textured_receiver =
      scene_final_specular_phase && !primary_is_diffuse_gi &&
      ((hit_identity_flags & 16u) != 0u) && hardware_scene_final_has_sparse_material_replay(texel);
  const bool direct_lit_refracted_textured_receiver =
      scene_final_specular_phase && !primary_is_diffuse_gi && ((hit_identity_flags & 16u) != 0u);
  const bool suppress_scene_final_direct_hit_light = hardware_scene_final_suppress_direct_hit_light(
      primary_is_diffuse_gi,
      hardware_hit_uses_proxy_payload(texel),
      hit_identity_flags,
      cl.type,
      hardware_hit_principled_metallic_coverage(texel));
  const float cl_thickness = is_transmission ? thickness : 0.0f;
  uchar receiver_light_set = 0u;
  float normal_offset = 0.0f;
  float geometry_offset = 0.0f;
  ObjectInfos object_infos;
  if (hardware_hit_object_infos_load(texel, object_infos)) {
    receiver_light_set = receiver_light_set_get(object_infos);
    normal_offset = object_infos.shadow_terminator_normal_offset;
    geometry_offset = object_infos.shadow_terminator_geometry_offset;
  }
  shadow_dispatch_texel_fullres = texel;
  shadow_dispatch_visibility_source = HWRT_SHADOW_VISIBILITY_MAIN_HIT;
  shadow_dispatch_allow_transmission_hardware_rt = false;
  shadow_dispatch_force_unshadowed = primary_is_diffuse_gi || suppress_scene_final_direct_hit_light ||
                                     scene_final_replayed_textured_receiver;
  shadow_dispatch_use_hardware_rt = false;
  if (!primary_is_diffuse_gi && !shadow_dispatch_force_unshadowed &&
      (use_hardware_rt_shadows ||
       (use_hardware_rt_environment_visibility && !direct_lit_refracted_textured_receiver)))
  {
    shadow_dispatch_use_hardware_rt = hardware_hit_shadow_payload_valid(texel);
  }
  ClosureLightStack stack;
  ClosureUndetermined light_cl = hardware_hit_refracted_metal_direct_closure(
      cl, direct_lit_refracted_textured_receiver);
  stack.cl[0] = is_transmission ? closure_light_new(light_cl, V, cl_thickness) :
                                  closure_light_new(light_cl, V);
  LIGHT_FOREACH_BEGIN_DIRECTIONAL (light_cull_buf, l_idx) {
    light_eval_single(
        l_idx,
        true,
        is_transmission,
        stack,
        P_hit,
        N,
        V,
        cl_thickness,
        receiver_light_set,
        normal_offset,
        geometry_offset);
  }
  LIGHT_FOREACH_END

  if (hardware_hit_use_exact_local_lights()) {
    for (uint local_light_index = 0u; local_light_index < uint(hardware_local_lights_len);
         local_light_index++)
    {
      hardware_hit_light_eval_exact_local(local_light_index,
                                          is_transmission,
                                          stack,
                                          P_hit,
                                          N,
                                          V,
                                          cl_thickness,
                                          receiver_light_set,
                                          normal_offset,
                                          geometry_offset);
    }
  }
  else {
    /* Nuru NIS G2: many lights -> K sampled full evaluations instead of O(N). */
    hardware_hit_sampled_local_lights(texel_fullres,
                                      is_transmission,
                                      stack,
                                      P_hit,
                                      N,
                                      V,
                                      cl_thickness,
                                      receiver_light_set,
                                      normal_offset,
                                      geometry_offset);
  }

  eevee::LightProbeSample samp = lightprobe_sample(float2(texel_fullres), P_hit, N, V);
  probe_uses_world = lightprobe_uses_world(samp);
  float3 probe_light = lightprobe_eval(samp, cl, P_hit, V, cl_thickness);
  const bool diffuse_gi_world_probe = primary_is_diffuse_gi && is_diffuse_family &&
                                      probe_uses_world;

  if (diffuse_gi_world_probe) {
    /* Primary diffuse GI should transport surface lighting from the hit, not turn occluded RT
     * shadows into a world-probe fill. Keep world transport owned by primary environment visibility
     * and specular paths so colored wall bounce can dominate the shadow. */
    probe_light = float3(0.0f);
    probe_uses_world = false;
  }
  else if (!use_hardware_environment && probe_uses_world)
  {
    probe_light = float3(0.0f);
  }
  else if (scene_final_replayed_textured_receiver &&
           hardware_hit_closure_is_specular_family(cl.type) && probe_uses_world)
  {
    /* Hit-eval replay owns the material. Sample the world along the reflected specular direction
     * (matching direct-view mirror BRDF sampling), but still attenuate by the traced dome
     * visibility: an unmasked world sample floods sealed interiors with sky light. */
    LightProbeRay probe_ray = bxdf_lightprobe_ray(cl, P_hit, V, hardware_thickness(cl_thickness));
    probe_light = lightprobe_eval_with_direction(
        samp, cl, P_hit, V, cl_thickness, probe_ray.dominant_direction);
    probe_light *= hardware_environment_visibility_load_filtered(texel_fullres, N).visibility;
  }
  else if (suppress_scene_final_direct_hit_light &&
           hardware_hit_closure_is_specular_family(cl.type) && probe_uses_world)
  {
    LightProbeRay probe_ray = bxdf_lightprobe_ray(cl, P_hit, V, hardware_thickness(cl_thickness));
    probe_light = lightprobe_eval_with_direction(
        samp, cl, P_hit, V, cl_thickness, probe_ray.dominant_direction);
    /* Dome-occlusion contract: see the replayed-receiver branch above. */
    probe_light *= hardware_environment_visibility_load_filtered(texel_fullres, N).visibility;
  }
  else if (hardware_hit_closure_uses_environment_visibility(cl, primary_is_diffuse_gi) &&
           probe_uses_world)
  {
    HardwareEnvironmentVisibilityData env_visibility;
    if (hardware_hit_environment_visibility_load(texel, N, env_visibility)) {
      LightProbeRay probe_ray = bxdf_lightprobe_ray(cl, P_hit, V, hardware_thickness(cl_thickness));
      float3 world_direction = is_diffuse_family ?
                                   hardware_environment_visibility_direction(
                                       env_visibility, probe_ray.dominant_direction, N) :
                                   probe_ray.dominant_direction;
      probe_light = lightprobe_eval_with_direction(
          samp, cl, P_hit, V, cl_thickness, world_direction);
      if (is_diffuse_family) {
        float diffuse_world_visibility = square(saturate((env_visibility.visibility - 0.05f) / 0.95f));
        probe_light *= diffuse_world_visibility;
      }
      else {
        probe_light *= env_visibility.visibility;
      }
    }
  }


  float3 shading_color = max(cl.color, float3(0.0f));
  direct_radiance = suppress_scene_final_direct_hit_light ?
                        float3(0.0f) :
                        (stack.cl[0].light_shadowed * shading_color);
  if (suppress_scene_final_direct_hit_light || scene_final_replayed_textured_receiver) {
    if (hardware_hit_closure_is_specular_family(cl.type)) {
      probe_radiance = probe_light * shading_color;
      return;
    }
    probe_radiance = float3(0.0f);
    probe_uses_world = false;
    return;
  }
  if (is_diffuse_family) {
    probe_radiance = probe_light * shading_color;
    return;
  }
  probe_radiance = probe_light * shading_color;
}

void layered_receiver_hit_closure_light_terms(int2 texel_fullres,
                                              int2 texel,
                                              float3 P_hit,
                                              float3 N,
                                              float3 V,
                                              ClosureUndetermined cl,
                                              float thickness,
                                              bool primary_is_diffuse_gi,
                                              bool scene_final_textured_continuation_unshadowed,
                                              float3 &direct_radiance,
                                              float3 &probe_radiance)
{
  direct_radiance = float3(0.0f);
  probe_radiance = float3(0.0f);
  if (!hardware_hit_closure_has_energy(cl)) {
    return;
  }

  const bool scene_final_specular_phase =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
  const bool is_transmission = hardware_closure_has_transmission(cl.type);
  const bool is_diffuse_family = (cl.type == CLOSURE_BSDF_DIFFUSE_ID) ||
                                 (cl.type == CLOSURE_BSSRDF_BURLEY_ID);
  const bool scene_final_reflected_diffuse = scene_final_specular_phase && !primary_is_diffuse_gi &&
                                            is_diffuse_family &&
                                            hardware_receiver_gi_primary_is_mirror_like(
                                                texel_fullres);
  const uint hit_identity_flags = texelFetch(layered_receiver_hit_identity_tx, texel, 0).z;
  const bool direct_lit_refracted_textured_receiver =
      scene_final_specular_phase && !primary_is_diffuse_gi && ((hit_identity_flags & 16u) != 0u);
  const bool suppress_scene_final_direct_hit_light = hardware_scene_final_suppress_direct_hit_light(
      primary_is_diffuse_gi,
      layered_receiver_hit_uses_proxy_payload(texel),
      hit_identity_flags,
      cl.type,
      hardware_layered_receiver_principled_metallic_coverage(texel));
  const float cl_thickness = is_transmission ? thickness : 0.0f;
  uchar receiver_light_set = 0u;
  float normal_offset = 0.0f;
  float geometry_offset = 0.0f;
  ObjectInfos object_infos;
  if (layered_receiver_hit_object_infos_load(texel, object_infos)) {
    receiver_light_set = receiver_light_set_get(object_infos);
    normal_offset = object_infos.shadow_terminator_normal_offset;
    geometry_offset = object_infos.shadow_terminator_geometry_offset;
  }
  shadow_dispatch_texel_fullres = texel;
  shadow_dispatch_visibility_source = HWRT_SHADOW_VISIBILITY_LAYERED_RECEIVER;
  shadow_dispatch_allow_transmission_hardware_rt = true;
  shadow_dispatch_force_unshadowed =
      primary_is_diffuse_gi || scene_final_textured_continuation_unshadowed;
  shadow_dispatch_use_hardware_rt = false;
  if (!shadow_dispatch_force_unshadowed &&
      (use_hardware_rt_shadows ||
       (use_hardware_rt_environment_visibility && !direct_lit_refracted_textured_receiver)))
  {
    shadow_dispatch_use_hardware_rt = layered_receiver_hit_shadow_payload_valid(texel);
  }
  ClosureLightStack stack;
  ClosureUndetermined light_cl = hardware_hit_refracted_metal_direct_closure(
      cl, direct_lit_refracted_textured_receiver);
  stack.cl[0] = is_transmission ? closure_light_new(light_cl, V, cl_thickness) :
                                  closure_light_new(light_cl, V);
  LIGHT_FOREACH_BEGIN_DIRECTIONAL (light_cull_buf, l_idx) {
    light_eval_single(
        l_idx,
        true,
        is_transmission,
        stack,
        P_hit,
        N,
        V,
        cl_thickness,
        receiver_light_set,
        normal_offset,
        geometry_offset);
  }
  LIGHT_FOREACH_END

  if (hardware_hit_use_exact_local_lights()) {
    for (uint local_light_index = 0u; local_light_index < uint(hardware_local_lights_len);
         local_light_index++)
    {
      hardware_hit_light_eval_exact_local(local_light_index,
                                          is_transmission,
                                          stack,
                                          P_hit,
                                          N,
                                          V,
                                          cl_thickness,
                                          receiver_light_set,
                                          normal_offset,
                                          geometry_offset);
    }
  }
  else {
    /* Nuru NIS G2: many lights -> K sampled full evaluations instead of O(N). */
    hardware_hit_sampled_local_lights(texel_fullres,
                                      is_transmission,
                                      stack,
                                      P_hit,
                                      N,
                                      V,
                                      cl_thickness,
                                      receiver_light_set,
                                      normal_offset,
                                      geometry_offset);
  }

  eevee::LightProbeSample samp = lightprobe_sample(float2(texel_fullres), P_hit, N, V);
  const bool probe_uses_world = lightprobe_uses_world(samp);
  float3 probe_light = lightprobe_eval(samp, cl, P_hit, V, cl_thickness);
  const bool diffuse_gi_world_probe = primary_is_diffuse_gi && is_diffuse_family &&
                                      probe_uses_world;

  if (diffuse_gi_world_probe) {
    probe_light = float3(0.0f);
  }
  else if (primary_is_diffuse_gi && probe_uses_world) {
    /* Nuru: receiver shading has no hit-domain environment-visibility buffer. For diffuse-GI
     * primaries an unmasked world probe at the receiver leaks sky/HDRI into enclosed interiors;
     * fail closed and let the traced GI transport own world lighting. */
    probe_light = float3(0.0f);
  }
  else if (!use_hardware_environment && probe_uses_world)
  {
    probe_light = float3(0.0f);
  }
  else if (scene_final_textured_continuation_unshadowed && probe_uses_world)
  {
    /* Continuation is folded onto a replayed mirror proxy at the parent texel. The parent-texel
     * dome mask is constant across the reflected surface (it can print a horizontal terminator
     * on textured spheres), but leaving the world sample unmasked floods sealed interiors with
     * sky light through every mirror. Occlusion correctness owns this trade: attenuate. */
    if (hardware_hit_closure_is_specular_family(cl.type)) {
      LightProbeRay probe_ray = bxdf_lightprobe_ray(cl, P_hit, V, hardware_thickness(cl_thickness));
      probe_light = lightprobe_eval_with_direction(
          samp, cl, P_hit, V, cl_thickness, probe_ray.dominant_direction);
    }
    else {
      probe_light = lightprobe_eval(samp, cl, P_hit, V, cl_thickness);
    }
    probe_light *= hardware_environment_visibility_load_filtered(texel_fullres, N).visibility;
  }
  else if (hardware_hit_closure_uses_environment_visibility(cl, primary_is_diffuse_gi) &&
           probe_uses_world)
  {
    /* Receiver shading does not have its own environment-visibility buffer yet, but transmission
     * receivers still need the traced transmitted/world direction rather than the front mirror texel
     * fallback or they lose the HDRI/world contribution entirely on miss. Attenuate by the
     * parent-texel dome visibility (dome-occlusion contract, see above). */
    if (hardware_hit_closure_is_specular_family(cl.type)) {
      LightProbeRay probe_ray = bxdf_lightprobe_ray(cl, P_hit, V, hardware_thickness(cl_thickness));
      probe_light = lightprobe_eval_with_direction(
          samp, cl, P_hit, V, cl_thickness, probe_ray.dominant_direction);
      probe_light *= hardware_environment_visibility_load_filtered(texel_fullres, N).visibility;
    }
  }

  float3 shading_color = max(cl.color, float3(0.0f));
  if (scene_final_textured_continuation_unshadowed) {
    direct_radiance = suppress_scene_final_direct_hit_light ?
                          float3(0.0f) :
                          (stack.cl[0].light_unshadowed * shading_color);
    probe_radiance = probe_light * shading_color;
    return;
  }
  direct_radiance = suppress_scene_final_direct_hit_light ?
                        float3(0.0f) :
                        (stack.cl[0].light_shadowed * shading_color);
  if (suppress_scene_final_direct_hit_light) {
    probe_radiance = float3(0.0f);
    return;
  }
  probe_radiance = probe_light * shading_color;
}

float3 layered_receiver_hit_radiance_resolve(int2 texel,
                                               int2 texel_fullres,
                                               bool primary_is_diffuse_gi,
                                               bool scene_final_textured_continuation_unshadowed)
{
  float3 P_hit, V;
  if (!layered_receiver_hit_load(texel, P_hit, V)) {
    return float3(0.0f);
  }

  float3 N;
  if (!layered_receiver_hit_normal_load(texel, N)) {
    return float3(0.0f);
  }

  if (hardware_receiver_needs_textured_raster_fallback(texel, false)) {
    float3 raster_radiance;
    if (hardware_scene_final_raster_radiance_at_world_position(P_hit, raster_radiance)) {
      float4 carried_throughput = layered_receiver_hit_throughput_load(texel);
      if (carried_throughput.a > 0.5f) {
        raster_radiance *= max(carried_throughput.rgb, float3(0.0f));
      }
      return raster_radiance;
    }
  }

  float thickness = layered_receiver_hit_thickness_load(texel);
  ClosureUndetermined base_cl = layered_receiver_hit_base_closure_load(texel, N);
  ClosureUndetermined specular_cl = layered_receiver_hit_specular_closure_load(texel, N);
  const uint layered_identity_flags = texelFetch(layered_receiver_hit_identity_tx, texel, 0).z;
  const float layered_metallic_coverage =
      hardware_layered_receiver_principled_metallic_coverage(texel);
  const bool layered_principled_scene_final = hardware_scene_final_is_principled_layered(
      layered_identity_flags);
  const ClosureUndetermined layered_specular_lighting_cl =
      hardware_principled_metal_tinted_specular(layered_principled_scene_final,
                                                specular_cl,
                                                base_cl,
                                                layered_metallic_coverage);

  float3 radiance = texelFetch(layered_receiver_ray_radiance_tx, texel, 0).rgb;
  float3 base_direct = float3(0.0f);
  float3 base_probe = float3(0.0f);
  float3 specular_direct = float3(0.0f);
  float3 specular_probe = float3(0.0f);
  layered_receiver_hit_closure_light_terms(texel_fullres,
                                           texel,
                                           P_hit,
                                           N,
                                           V,
                                           base_cl,
                                           thickness,
                                           primary_is_diffuse_gi,
                                           scene_final_textured_continuation_unshadowed,
                                           base_direct,
                                           base_probe);
  layered_receiver_hit_closure_light_terms(texel_fullres,
                                           texel,
                                           P_hit,
                                           N,
                                           V,
                                           layered_specular_lighting_cl,
                                           thickness,
                                           primary_is_diffuse_gi,
                                           scene_final_textured_continuation_unshadowed,
                                           specular_direct,
                                           specular_probe);
  const bool scene_final_layered_diffuse_receiver =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR) &&
      ((base_cl.type == CLOSURE_BSDF_DIFFUSE_ID) || (base_cl.type == CLOSURE_BSSRDF_BURLEY_ID)) &&
      hardware_receiver_gi_primary_is_mirror_like(texel_fullres);
  float3 layered_receiver_gi_radiance = float3(0.0f);
  /* Nuru Secondary GI: traced per-pixel GI for the diffuse surface seen through a layered
   * (Principled metallic) mirror. Texture stays cleared when the toggle is off. */
  const bool scene_final_layered_receiver_gi =
      scene_final_layered_diffuse_receiver &&
      hardware_layered_receiver_gi_load(texel, layered_receiver_gi_radiance);
  float3 layered_secondary_photon_gi_radiance = float3(0.0f);
  const bool scene_final_layered_secondary_photon_gi = false;
  const bool layered_proxy_payload = layered_receiver_hit_uses_proxy_payload(texel);
  const float layered_base_visibility = hardware_principled_base_layer_visibility(
      layered_principled_scene_final, layered_proxy_payload, layered_metallic_coverage);
  const float layered_specular_visibility = hardware_principled_reflection_layer_visibility(
      layered_principled_scene_final,
      layered_proxy_payload,
      N,
      V,
      layered_specular_lighting_cl,
      layered_metallic_coverage);
  const bool layered_specular_texture_tint_only = hardware_hit_uses_specular_texture_tint_only(
      layered_identity_flags, layered_metallic_coverage, base_cl, specular_cl);
  if (!layered_specular_texture_tint_only) {
    radiance += base_direct * layered_base_visibility;
  }
  radiance += specular_direct * layered_specular_visibility;
  const bool add_probe_terms =
      primary_is_diffuse_gi ||
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
  if (scene_final_layered_receiver_gi) {
    radiance += (layered_receiver_gi_radiance * max(base_cl.color, float3(0.0f))) /
                max(uniform_buf.clamp.indirect_scale, 1.0e-4f);
    if (add_probe_terms) {
      radiance += specular_probe * layered_specular_visibility;
    }
  }
  else if (add_probe_terms)
  {
    if (!layered_specular_texture_tint_only) {
      radiance += base_probe * layered_base_visibility;
    }
    radiance += specular_probe * layered_specular_visibility;
  }
  if (scene_final_layered_secondary_photon_gi) {
    radiance += layered_secondary_photon_gi_radiance /
                max(uniform_buf.clamp.indirect_scale, 1.0e-4f);
  }
  float4 carried_throughput = layered_receiver_hit_throughput_load(texel);
  if (carried_throughput.a > 0.5f) {
    radiance *= max(carried_throughput.rgb, float3(0.0f));
  }
  return radiance;
}

bool transmission_receiver_hit_exists(int2 texel)
{
  return texelFetch(transmission_receiver_ray_time_tx, texel, 0).r > 0.0f;
}

float4 transmission_receiver_hit_throughput_load(int2 texel)
{
  return texelFetch(transmission_receiver_throughput_tx, texel, 0);
}

bool transmission_receiver_hit_uses_proxy_payload(int2 texel)
{
  return texelFetch(transmission_receiver_hit_albedo_tx, texel, 0).a < 0.0f;
}

bool transmission_receiver_hit_direction_load(int2 texel, float3 &ray_direction)
{
  float2 packed_dir = float2(texelFetch(transmission_receiver_hit_material_tx, texel, 0).w,
                             texelFetch(transmission_receiver_hit_normal_tx, texel, 0).w);
  if (all(equal(packed_dir, float2(0.0f)))) {
    return false;
  }
  ray_direction = hardware_direction_unpack(packed_dir);
  return isfinite(ray_direction.x) && isfinite(ray_direction.y) && isfinite(ray_direction.z) &&
         dot(ray_direction, ray_direction) > 1.0e-10f;
}

bool transmission_receiver_hit_load(int2 texel, float3 &P_hit, float3 &V)
{
  if (!transmission_receiver_hit_exists(texel)) {
    return false;
  }
  float3 ray_direction;
  if (!transmission_receiver_hit_direction_load(texel, ray_direction)) {
    return false;
  }
  P_hit = texelFetch(transmission_receiver_world_position_tx, texel, 0).xyz;
  if (!(isfinite(P_hit.x) && isfinite(P_hit.y) && isfinite(P_hit.z)) ||
      dot(P_hit, P_hit) <= 1.0e-10f)
  {
    return false;
  }
  V = -ray_direction;
  return true;
}

bool transmission_receiver_hit_normal_load(int2 texel, float3 &N)
{
  N = texelFetch(transmission_receiver_hit_normal_tx, texel, 0).rgb;
  return dot(N, N) > 1.0e-10f;
}

bool transmission_receiver_hit_shadow_payload_valid(int2 texel)
{
  float3 shadow_N = texelFetch(transmission_receiver_hit_normal_tx, texel, 0).rgb;
  if (!(isfinite(shadow_N.x) && isfinite(shadow_N.y) && isfinite(shadow_N.z)) ||
      dot(shadow_N, shadow_N) <= 1.0e-10f)
  {
    return false;
  }

  float3 shadow_P = texelFetch(transmission_receiver_world_position_tx, texel, 0).xyz;
  return isfinite(shadow_P.x) && isfinite(shadow_P.y) && isfinite(shadow_P.z) &&
         dot(shadow_P, shadow_P) > 1.0e-10f;
}

bool transmission_receiver_hit_object_infos_load(int2 texel, ObjectInfos &object_infos)
{
  const uint resource_id = texelFetch(transmission_receiver_hit_identity_tx, texel, 0).w;
  if (resource_id == 0xFFFFFFFFu) {
    object_infos = hardware_object_infos_zero();
    return false;
  }
  object_infos = hardware_object_infos_get(resource_id);
  return true;
}

ClosureUndetermined transmission_receiver_hit_base_closure_load(int2 texel, float3 N)
{
  float4 hit_base = texelFetch(transmission_receiver_hit_albedo_tx, texel, 0);
  float4 hit_material = texelFetch(transmission_receiver_hit_material_tx, texel, 0);
  const bool proxy_payload = transmission_receiver_hit_uses_proxy_payload(texel);
  ClosureType type = proxy_payload ? hardware_hit_closure_type_unpack(hit_material.z) :
                                     hardware_hit_closure_type_unpack(hit_base.a);
  if (proxy_payload && !hardware_hit_closure_is_base_family(type)) {
    type = CLOSURE_NONE_ID;
  }
  if (type == CLOSURE_BSSRDF_BURLEY_ID) {
    type = CLOSURE_BSDF_DIFFUSE_ID;
  }

  ClosureUndetermined cl = closure_new(type);
  cl.weight = 1.0f;
  cl.color = hit_base.rgb;
  cl.N = N;
  cl.data = float4(0.0f);

  switch (cl.type) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      cl.data.x = hit_material.x;
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      cl.data.x = hit_material.x;
      cl.data.y = hit_material.y;
      break;
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_NONE_ID:
      break;
  }
  return cl;
}

ClosureUndetermined transmission_receiver_hit_specular_closure_load(int2 texel, float3 N)
{
  float4 hit_base = texelFetch(transmission_receiver_hit_albedo_tx, texel, 0);
  float4 hit_material = texelFetch(transmission_receiver_hit_material_tx, texel, 0);
  float4 hit_specular = texelFetch(transmission_receiver_hit_position_tx, texel, 0);
  const bool proxy_payload = transmission_receiver_hit_uses_proxy_payload(texel);
  const ClosureType type = hardware_hit_closure_type_unpack(hit_material.z);

  ClosureUndetermined cl = closure_new(
      hardware_hit_closure_is_specular_family(type) ? type : CLOSURE_NONE_ID);
  cl.weight = 1.0f;
  cl.color = proxy_payload ? hit_base.rgb : hit_specular.rgb;
  cl.N = N;
  cl.data = float4(0.0f);

  switch (cl.type) {
    case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      cl.data.x = hit_material.x;
      break;
    case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
      cl.data.x = hit_material.x;
      cl.data.y = hit_material.y;
      break;
    case CLOSURE_BSDF_TRANSLUCENT_ID:
    case CLOSURE_BSDF_DIFFUSE_ID:
    case CLOSURE_BSSRDF_BURLEY_ID:
    case CLOSURE_NONE_ID:
      break;
  }

  return cl;
}

float transmission_receiver_hit_thickness_load(int2 texel)
{
  if (transmission_receiver_hit_uses_proxy_payload(texel)) {
    return 0.0f;
  }
  return hardware_thickness_unpack(texelFetch(transmission_receiver_hit_position_tx, texel, 0).w);
}

void transmission_receiver_hit_closure_light_terms(int2 texel_fullres,
                                                   int2 texel,
                                                   float3 P_hit,
                                                   float3 N,
                                                   float3 V,
                                                   ClosureUndetermined cl,
                                                   float thickness,
                                                   bool primary_is_diffuse_gi,
                                                   float3 &direct_radiance,
                                                   float3 &probe_radiance)
{
  direct_radiance = float3(0.0f);
  probe_radiance = float3(0.0f);
  if (!hardware_hit_closure_has_energy(cl)) {
    return;
  }

  const bool scene_final_specular_phase =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
  const bool is_transmission = hardware_closure_has_transmission(cl.type);
  const bool is_diffuse_family = (cl.type == CLOSURE_BSDF_DIFFUSE_ID) ||
                                 (cl.type == CLOSURE_BSSRDF_BURLEY_ID);
  const bool scene_final_reflected_diffuse = scene_final_specular_phase && !primary_is_diffuse_gi &&
                                            is_diffuse_family &&
                                            hardware_receiver_gi_primary_is_mirror_like(
                                                texel_fullres);
  const uint hit_identity_flags = texelFetch(transmission_receiver_hit_identity_tx, texel, 0).z;
  const bool direct_lit_refracted_textured_receiver =
      scene_final_specular_phase && !primary_is_diffuse_gi && ((hit_identity_flags & 16u) != 0u);
  const bool suppress_scene_final_direct_hit_light = hardware_scene_final_suppress_direct_hit_light(
      primary_is_diffuse_gi,
      transmission_receiver_hit_uses_proxy_payload(texel),
      hit_identity_flags,
      cl.type,
      0.0f);
  const float cl_thickness = is_transmission ? thickness : 0.0f;
  uchar receiver_light_set = 0u;
  float normal_offset = 0.0f;
  float geometry_offset = 0.0f;
  ObjectInfos object_infos;
  if (transmission_receiver_hit_object_infos_load(texel, object_infos)) {
    receiver_light_set = receiver_light_set_get(object_infos);
    normal_offset = object_infos.shadow_terminator_normal_offset;
    geometry_offset = object_infos.shadow_terminator_geometry_offset;
  }
  shadow_dispatch_texel_fullres = texel;
  shadow_dispatch_visibility_source = HWRT_SHADOW_VISIBILITY_TRANSMISSION_RECEIVER;
  shadow_dispatch_allow_transmission_hardware_rt = true;
  shadow_dispatch_force_unshadowed = primary_is_diffuse_gi;
  shadow_dispatch_use_hardware_rt = false;
  if (!primary_is_diffuse_gi &&
      (use_hardware_rt_shadows ||
       (use_hardware_rt_environment_visibility && !direct_lit_refracted_textured_receiver)))
  {
    shadow_dispatch_use_hardware_rt = transmission_receiver_hit_shadow_payload_valid(texel);
  }
  ClosureLightStack stack;
  ClosureUndetermined light_cl = hardware_hit_refracted_metal_direct_closure(
      cl, direct_lit_refracted_textured_receiver);
  stack.cl[0] = is_transmission ? closure_light_new(light_cl, V, cl_thickness) :
                                  closure_light_new(light_cl, V);
  LIGHT_FOREACH_BEGIN_DIRECTIONAL (light_cull_buf, l_idx) {
    light_eval_single(
        l_idx,
        true,
        is_transmission,
        stack,
        P_hit,
        N,
        V,
        cl_thickness,
        receiver_light_set,
        normal_offset,
        geometry_offset);
  }
  LIGHT_FOREACH_END

  if (hardware_hit_use_exact_local_lights()) {
    for (uint local_light_index = 0u; local_light_index < uint(hardware_local_lights_len);
         local_light_index++)
    {
      hardware_hit_light_eval_exact_local(local_light_index,
                                          is_transmission,
                                          stack,
                                          P_hit,
                                          N,
                                          V,
                                          cl_thickness,
                                          receiver_light_set,
                                          normal_offset,
                                          geometry_offset);
    }
  }
  else {
    /* Nuru NIS G2: many lights -> K sampled full evaluations instead of O(N). */
    hardware_hit_sampled_local_lights(texel_fullres,
                                      is_transmission,
                                      stack,
                                      P_hit,
                                      N,
                                      V,
                                      cl_thickness,
                                      receiver_light_set,
                                      normal_offset,
                                      geometry_offset);
  }

  eevee::LightProbeSample samp = lightprobe_sample(float2(texel_fullres), P_hit, N, V);
  float3 probe_light = lightprobe_eval(samp, cl, P_hit, V, cl_thickness);
  if (primary_is_diffuse_gi && lightprobe_uses_world(samp)) {
    /* Nuru: same fail-closed rule as the layered receiver above. Transmission receivers have no
     * hit-domain environment visibility; an unmasked world probe here floods enclosed interiors
     * with sky/HDRI through the diffuse-GI transport when refractions are enabled. */
    probe_light = float3(0.0f);
  }
  else if (!use_hardware_environment && lightprobe_uses_world(samp))
  {
    probe_light = float3(0.0f);
  }
  else if (hardware_hit_closure_uses_environment_visibility(cl, primary_is_diffuse_gi) &&
           lightprobe_uses_world(samp))
  {
    /* Keep receiver ownership consistent with the existing layered-reflection resolve. */
    probe_light *= 1.0f;
  }

  float3 shading_color = max(cl.color, float3(0.0f));
  direct_radiance = suppress_scene_final_direct_hit_light ?
                        float3(0.0f) :
                        (stack.cl[0].light_shadowed * shading_color);
  if (suppress_scene_final_direct_hit_light) {
    probe_radiance = float3(0.0f);
    return;
  }
  probe_radiance = probe_light * shading_color;
}

float3 transmission_receiver_hit_radiance_resolve(int2 texel,
                                                  int2 texel_fullres,
                                                  bool primary_is_diffuse_gi)
{
  float3 P_hit, V;
  if (!transmission_receiver_hit_load(texel, P_hit, V)) {
    return float3(0.0f);
  }

  float3 N;
  if (!transmission_receiver_hit_normal_load(texel, N)) {
    return float3(0.0f);
  }

  if (hardware_receiver_needs_textured_raster_fallback(texel, true)) {
    float3 raster_radiance;
    if (hardware_scene_final_raster_radiance_at_world_position(P_hit, raster_radiance)) {
      float4 carried_throughput = transmission_receiver_hit_throughput_load(texel);
      if (carried_throughput.a > 0.5f) {
        raster_radiance *= max(carried_throughput.rgb, float3(0.0f));
      }
      return raster_radiance;
    }
  }

  float thickness = transmission_receiver_hit_thickness_load(texel);
  ClosureUndetermined base_cl = transmission_receiver_hit_base_closure_load(texel, N);
  ClosureUndetermined specular_cl = transmission_receiver_hit_specular_closure_load(texel, N);

  float3 radiance = texelFetch(transmission_receiver_ray_radiance_tx, texel, 0).rgb;
  const bool direct_lit_refracted_textured_receiver =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR) &&
      !primary_is_diffuse_gi &&
      ((texelFetch(transmission_receiver_hit_identity_tx, texel, 0).z & 16u) != 0u);
  const bool replayed_reflective_receiver =
      direct_lit_refracted_textured_receiver &&
      hardware_hit_closure_is_specular_family(specular_cl.type);
  /* Nuru: the metal trace skips multiplying its proxy color into `transmission_throughput`
   * when the metal is itself a replay receiver, so the env reflection accumulated into
   * `transmission_receiver_ray_radiance_tx` is untinted environment radiance. Apply the metal
   * color here so the checker / textured base of a metal seen through refraction is visible in
   * the reflected radiance. `specular_cl.color` already resolves to the textured replay color
   * when hit-eval ran, and to the proxy color in the coarse-only path. */
  if (replayed_reflective_receiver) {
    float3 metal_color = max(specular_cl.color, float3(0.0f));
    if (!(dot(metal_color, metal_color) > 1.0e-10f)) {
      metal_color = max(base_cl.color, float3(0.0f));
    }
    if ((dot(radiance, radiance) > 1.0e-10f) &&
        (dot(metal_color, metal_color) > 1.0e-10f))
    {
      radiance *= metal_color;
    }
  }

  float3 base_direct = float3(0.0f);
  float3 base_probe = float3(0.0f);
  float3 specular_direct = float3(0.0f);
  float3 specular_probe = float3(0.0f);
  transmission_receiver_hit_closure_light_terms(
      texel_fullres, texel, P_hit, N, V, base_cl, thickness, primary_is_diffuse_gi, base_direct, base_probe);
  transmission_receiver_hit_closure_light_terms(texel_fullres,
                                                texel,
                                                P_hit,
                                                N,
                                                V,
                                                specular_cl,
                                                thickness,
                                                primary_is_diffuse_gi,
                                                specular_direct,
                                                specular_probe);
  const bool scene_final_transmission_diffuse_receiver =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR) &&
      ((base_cl.type == CLOSURE_BSDF_DIFFUSE_ID) || (base_cl.type == CLOSURE_BSSRDF_BURLEY_ID)) &&
      hardware_receiver_gi_primary_is_mirror_like(texel_fullres);
  float3 transmission_receiver_gi_radiance = float3(0.0f);
  /* Nuru Secondary GI: traced per-pixel GI for the diffuse surface seen through glass. */
  const bool scene_final_transmission_receiver_gi =
      scene_final_transmission_diffuse_receiver &&
      hardware_transmission_receiver_gi_load(texel, transmission_receiver_gi_radiance);
  float3 transmission_secondary_photon_gi_radiance = float3(0.0f);
  const bool scene_final_transmission_secondary_photon_gi = false;
  radiance += base_direct + specular_direct;
  const bool add_probe_terms =
      primary_is_diffuse_gi ||
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
  if (scene_final_transmission_receiver_gi) {
    radiance += (transmission_receiver_gi_radiance * max(base_cl.color, float3(0.0f))) /
                max(uniform_buf.clamp.indirect_scale, 1.0e-4f);
    if (add_probe_terms) {
      radiance += specular_probe;
    }
  }
  else if (add_probe_terms) {
    radiance += base_probe + specular_probe;
  }
  if (scene_final_transmission_secondary_photon_gi) {
    radiance += transmission_secondary_photon_gi_radiance /
                max(uniform_buf.clamp.indirect_scale, 1.0e-4f);
  }
  float4 carried_throughput = transmission_receiver_hit_throughput_load(texel);
  if (carried_throughput.a > 0.5f) {
    radiance *= max(carried_throughput.rgb, float3(0.0f));
  }
  return radiance;
}

bool hardware_primary_surface_position_load(int2 texel_fullres, float3 &P)
{
  if (any(lessThan(texel_fullres, int2(0))) ||
      any(greaterThanEqual(texel_fullres, textureSize(depth_tx, 0))))
  {
    return false;
  }
  float depth = reverse_z::read(texelFetch(depth_tx, texel_fullres, 0).r);
  if (!(depth > 0.0f && depth < 1.0f)) {
    return false;
  }
  float2 uv = (float2(texel_fullres) + 0.5f) * raytrace_buf.full_resolution_inv;
  P = drw_point_screen_to_world(float3(uv, depth));
  return true;
}

float hardware_hit_caustic_focus(int2 texel, int2 texel_fullres, float3 P_hit, float3 V)
{
  float compression = 0.0f;
  float3 hit_px, hit_py, dummy_V;
  float3 primary_P;
  if (hardware_hit_load(texel + int2(1, 0), hit_px, dummy_V) &&
      hardware_hit_load(texel + int2(0, 1), hit_py, dummy_V))
  {
    float3 primary_px;
    float3 primary_py;
    const int step = max(1,
                         (raytrace_buf.trace_pixel_scale +
                          1 - 1) /
                             max(1, 1));
    if (hardware_primary_surface_position_load(texel_fullres, primary_P) &&
        hardware_primary_surface_position_load(texel_fullres + int2(step, 0), primary_px) &&
        hardware_primary_surface_position_load(texel_fullres + int2(0, step), primary_py))
    {
      float primary_area = length(primary_px - primary_P) * length(primary_py - primary_P);
      float receiver_area = length(hit_px - P_hit) * length(hit_py - P_hit);
      if ((primary_area > 1.0e-6f) && (receiver_area > 1.0e-6f)) {
        compression = saturate((primary_area / receiver_area - 1.0f) * 0.03f);
      }
    }
  }

  float bending = 0.0f;
  if (hardware_primary_surface_position_load(texel_fullres, primary_P)) {
    float3 primary_V = drw_world_incident_vector(primary_P);
    bending = saturate((1.0f - abs(dot(normalize(primary_V), normalize(V)))) * 4.0f);
  }

  float focus_seed = max(compression, bending * 0.5f);
  if (!(focus_seed > 1.0e-6f)) {
    focus_seed = 0.25f;
  }

  float sharpness = 1.0f + log2(float(max(1, 1))) *
                               0.5f;
  float gain = 1.0f + log2(float(max(1, 1))) * 0.2f;
  return pow(focus_seed, sharpness) * gain;
}

bool hardware_hit_caustics_eligible(ClosureUndetermined primary_closure, ClosureUndetermined base_cl)
{
  if (!hardware_hit_uses_caustics() || !hardware_hit_closure_has_energy(base_cl) ||
      !hardware_hit_closure_has_energy(primary_closure))
  {
    return false;
  }
  if ((base_cl.type != CLOSURE_BSDF_DIFFUSE_ID) && (base_cl.type != CLOSURE_BSSRDF_BURLEY_ID)) {
    return false;
  }
  /* Caustics-only ownership is limited to reflective/refractive transport. Sharp non-diffuse
   * closures like translucent fallbacks should not broaden this receiver buffer into a generic
   * indirect-light path. */
  if (!hardware_hit_closure_is_specular_family(primary_closure.type)) {
    return false;
  }

  return closure_apparent_roughness_get(primary_closure) < 0.32f;
}

float hardware_hit_caustics_roughness_weight(ClosureUndetermined primary_closure)
{
  float roughness = closure_apparent_roughness_get(primary_closure);
  return 1.0f - smoothstep(0.08f, 0.32f, roughness);
}

bool hardware_hit_reflected_receiver_caustics_eligible(bool scene_final_specular_phase,
                                                       ClosureUndetermined primary_closure,
                                                       ClosureUndetermined base_cl)
{
  /* Receiver caustics are written to the diffuse caustics buffer and combined on diffuse
   * closures only. Injecting the same term into scene-final reflection radiance blows out metal
   * (Principled diffuse base + sharp reflection) with an extra 2.5x boost. */
  return false;
}

float3 hardware_hit_caustics_target(int2 texel,
                                    int2 texel_fullres,
                                    float3 P_hit,
                                    float3 V,
                                    float3 direct_light,
                                    float3 probe_light,
                                    bool probe_uses_world,
                                    float3 transport_seed)
{
  float focus = hardware_hit_caustic_focus(texel, texel_fullres, P_hit, V);
  const float sample_gain = 0.15f +
                            log2(float(max(1, 1))) *
                                0.05f;
  /* Keep receiver caustics focused. Reuse direct lighting plus the world/HDRI-side probe term,
   * but do not let generic local diffuse probe energy broaden the caustics buffer. */
  float3 caustic_source = max(
      direct_light + (probe_uses_world ? (probe_light * 0.25f) : float3(0.0f)), float3(0.0f));
  if (dot(caustic_source, caustic_source) <= 1.0e-10f) {
    /* When the broad precombine baseline is fail-closed, black-world local-light scenes can lose
     * the old internal caustic seed entirely. Borrow a small amount of resolved transport only for
     * the receiver-caustics buffer so the focused late pass stays alive without restoring visible
     * precombine radiance. */
    caustic_source = max(transport_seed * 0.25f, float3(0.0f));
  }
  if (dot(caustic_source, caustic_source) <= 1.0e-10f) {
    return float3(0.0f);
  }
  const float3 caustic_seed = max(caustic_source, float3(0.35f));
  return caustic_seed * max(focus, 0.20f) * sample_gain;
}

float3 hardware_hit_caustics_resolve(int2 texel,
                                     int2 texel_fullres,
                                     float3 P_hit,
                                     float3 N_hit,
                                     float3 V,
                                     float3 base_direct,
                                     float3 base_probe,
                                     bool base_probe_uses_world,
                                     float3 transport_seed,
                                     float caustics_weight)
{
  float2 receiver_uv;
  int2 receiver_texel;
  if (!hardware_hit_visible_surface_lookup_texel_load(
          texel, P_hit, N_hit, true, receiver_uv, receiver_texel))
  {
    return float3(0.0f);
  }
  const float3 target = hardware_hit_caustics_target(texel,
                                                     texel_fullres,
                                                     P_hit,
                                                     V,
                                                     base_direct,
                                                     base_probe,
                                                     base_probe_uses_world,
                                                     transport_seed);
  if (dot(target, target) <= 1.0e-10f) {
    return float3(0.0f);
  }
  const float history_blend = clamp(
      4.0f / float(max(1, 1)), 0.05f, 0.5f);
  const float3 resolved = mix(hardware_caustics_load(receiver_texel), target * caustics_weight, history_blend);
  /* Optional caustics output is not allocated by the core HWRT port. */
  return resolved;
}

bool hardware_environment_miss_load(int2 texel,
                                    int2 &texel_fullres,
                                    Ray &ray,
                                    float3 &V,
                                    float &ray_pdf_inv,
                                    bool &preserve_existing_radiance)
{
  float4 ray_data_im;
  float ray_time;
  if (!hardware_ray_load(texel, texel_fullres, ray_data_im, ray_time) || ray_time != -3.0f)
  {
    return false;
  }

  ClosureUndetermined miss_closure = gbuffer::read_bin(texel_fullres, closure_index);
  preserve_existing_radiance = hardware_hit_preserves_screen_baseline(miss_closure);
  float depth = reverse_z::read(texelFetch(depth_tx, texel_fullres, 0).r);
  if (!(depth > 0.0f && depth < 1.0f)) {
    return false;
  }

  float2 uv = (float2(texel_fullres) + 0.5f) * raytrace_buf.full_resolution_inv;
  float3 P = drw_point_screen_to_world(float3(uv, depth));
  float3 stored_origin = imageLoadFast(hit_position_img, texel).xyz;
  if (dot(stored_origin, stored_origin) > 1.0e-10f) {
    P = stored_origin;
  }
  float3 miss_direction = ray_data_im.xyz;
  if (!hardware_hit_direction_load(texel, miss_direction)) {
    miss_direction = normalize(miss_direction);
  }
  ray.direction = miss_direction;
  V = -ray.direction;
  ray.origin = P;
  ray_pdf_inv = ray_data_im.w;

  if (closure_index == 0) {
    const gbuffer::Header gbuf_header = gbuffer::read_header(texel_fullres);
    const float thickness = hardware_thickness_value(gbuffer::read_thickness(gbuf_header, texel_fullres));
    if (thickness != 0.0f) {
      ClosureUndetermined cl = gbuffer::read_bin(texel_fullres, closure_index);
      ray = raytrace_thickness_ray_amend(ray, cl, V, hardware_thickness(thickness));
    }
  }

  return true;
}

float3 hardware_environment_miss_tint_load(int2 texel)
{
  ClosureType miss_type = hardware_hit_closure_type_unpack(imageLoadFast(hit_material_img, texel).z);
  if (!hardware_hit_closure_is_specular_family(miss_type)) {
    return float3(1.0f);
  }
  float3 miss_tint = max(imageLoadFast(hit_albedo_img, texel).rgb, float3(0.0f));
  return (dot(miss_tint, miss_tint) > 1.0e-10f) ? miss_tint : float3(1.0f);
}

float3 hardware_hit_normal_estimate(int2 texel, float3 P_hit, float3 V)
{
  float3 Px, Vx;
  float3 Py, Vy;
  bool valid_x = hardware_hit_load(texel + int2(1, 0), Px, Vx);
  bool valid_y = hardware_hit_load(texel + int2(0, 1), Py, Vy);

  float3 N = V;
  if (valid_x && valid_y) {
    float3 candidate = cross(Px - P_hit, Py - P_hit);
    float len_sq = dot(candidate, candidate);
    if (len_sq > 1.0e-10f) {
      N = candidate * inversesqrt(len_sq);
    }
  }

  if (dot(N, V) < 0.0f) {
    N = -N;
  }
  return N;
}

bool hardware_hit_payload_exists(int2 texel)
{
  float3 hit_normal = imageLoadFast(hit_normal_img, texel).rgb;
  return dot(hit_normal, hit_normal) > 1.0e-10f;
}

bool hardware_hit_has_sparse_replay_radiance(int2 texel)
{
  return !hardware_hit_uses_proxy_payload(texel);
}

void main()
{
  constexpr uint tile_size = RAYTRACE_GROUP_SIZE;
  uint2 tile_coord = unpackUvec2x16(tiles_coord_buf[gl_WorkGroupID.x]);
  int2 texel = int2(gl_LocalInvocationID.xy + tile_coord * tile_size);

  if (any(greaterThanEqual(texel, imageSize(ray_data_img).xy))) {
    return;
  }
  Ray miss_ray;
  int2 miss_texel_fullres;
  float3 miss_V;
  float ray_pdf_inv;
  bool preserve_existing_radiance = true;
  if (hardware_environment_miss_load(
          texel, miss_texel_fullres, miss_ray, miss_V, ray_pdf_inv, preserve_existing_radiance))
  {
    ClosureUndetermined miss_closure = gbuffer::read_bin(miss_texel_fullres, closure_index);
    const bool miss_is_diffuse_gi = (miss_closure.type == CLOSURE_BSDF_DIFFUSE_ID) ||
                                    (miss_closure.type == CLOSURE_BSSRDF_BURLEY_ID);
    const bool scene_final_specular_phase =
        (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
    const bool precombine_specular_caustics_phase = !scene_final_specular_phase &&
                                                    !miss_is_diffuse_gi &&
                                                    hardware_hit_uses_caustics();
    float3 radiance = (!precombine_specular_caustics_phase && preserve_existing_radiance) ?
                          imageLoadFast(ray_radiance_img, texel).rgb :
                          float3(0.0f);
    float3 Ng = miss_ray.direction;
    eevee::LightProbeSample samp = lightprobe_sample(float2(miss_texel_fullres), miss_ray.origin, Ng, miss_V);
    ClosureType miss_proxy_type = hardware_hit_closure_type_unpack(imageLoadFast(hit_material_img, texel).z);
    float3 miss_N;
    float3 raster_radiance;
    if (scene_final_specular_phase &&
        hardware_hit_allows_scene_final_raster_reuse(miss_texel_fullres) &&
        hardware_hit_normal_load(texel, miss_N) &&
        hardware_hit_raster_radiance_load(
            texel,
            miss_ray.origin,
            miss_N,
            hardware_closure_has_transmission(miss_proxy_type),
            false,
            raster_radiance))
    {
      radiance = colorspace_brightness_clamp_max(radiance + raster_radiance,
                                                 uniform_buf.clamp.surface_indirect);
      imageStoreFast(ray_time_img, texel, float4(10000.0f));
      imageStoreFast(ray_radiance_img, texel, float4(radiance, 0.0f));
      return;
    }

    samp.volume_irradiance = spherical_harmonics::clamp_energy(samp.volume_irradiance,
                                                               uniform_buf.clamp.surface_indirect);
    float3 world_direction = miss_ray.direction;
    float environment_visibility = 1.0f;
    if (lightprobe_uses_world(samp)) {
      /* Hardware world misses must obey the same traced dome occlusion as the classic screen
       * path (see `eevee_ray_trace_screen_comp.glsl`): an unmasked world fallback floods
       * enclosed interiors with sky/HDRI light through every specular miss. Keep specular
       * misses on their traced direction but attenuate them by the receiver's cross-filtered
       * dome visibility; diffuse-GI misses additionally bend toward the visible dome. Without
       * the visibility buffer, fail closed exactly like the screen path. */
      if (use_hardware_rt_environment_visibility) {
        HardwareEnvironmentVisibilityData env_visibility =
            hardware_environment_visibility_load_filtered(miss_texel_fullres, Ng);
        if (miss_is_diffuse_gi) {
          world_direction = hardware_environment_visibility_direction(
              env_visibility, miss_ray.direction, Ng);
        }
        environment_visibility = env_visibility.visibility;
      }
      else {
        environment_visibility = 0.0f;
      }
    }
    float3 incoming_radiance = lightprobe_eval_direction(
        samp, miss_ray.origin, world_direction, ray_pdf_inv);
    incoming_radiance *= hardware_environment_miss_tint_load(texel);
    if (!precombine_specular_caustics_phase) {
      radiance += incoming_radiance * environment_visibility;
    }
    radiance = colorspace_brightness_clamp_max(radiance, uniform_buf.clamp.surface_indirect);
    imageStoreFast(ray_time_img, texel, float4(10000.0f));
    imageStoreFast(ray_radiance_img, texel, float4(radiance, 0.0f));
    return;
  }

  /* Disabled/unsupported families carry native Eevee (or the preceding GI phase) unchanged.
   * They have no payload, but retain positive ray time for the native spatial denoiser. */
  if (!hardware_hit_payload_exists(texel)) {
    return;
  }

  /* Only Hybrid-style ownership may preserve a screen-space first hit baseline. If screen-owned
   * radiance leaks into a Full RT specular pixel, fail closed to the Hardware result instead of
   * mixing both paths. */
  int2 primary_texel_fullres = raytrace_texel_to_fullres(
      texel,
      raytrace_buf.trace_pixel_scale,
      1,
      raytrace_buf.trace_pixel_offset);
  if (false && (raytrace_buf.trace_pixel_scale > 1)) {
    primary_texel_fullres = raytrace_representative_fullres_texel(
        texel,
        raytrace_buf.trace_pixel_scale,
        1,
        raytrace_buf.trace_pixel_offset);
  }
  ClosureUndetermined primary_closure = gbuffer::read_bin(primary_texel_fullres, closure_index);
  const bool primary_is_diffuse_gi = (primary_closure.type == CLOSURE_BSDF_DIFFUSE_ID) ||
                                     (primary_closure.type == CLOSURE_BSSRDF_BURLEY_ID);
  const bool scene_final_specular_phase =
      (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
  const bool precombine_specular_caustics_phase = !scene_final_specular_phase &&
                                                  !primary_is_diffuse_gi &&
                                                  hardware_hit_uses_caustics();
  const bool allow_scene_final_raster_reuse = hardware_hit_allows_scene_final_raster_reuse(
      primary_texel_fullres);
  const bool preserve_screen_baseline = hardware_hit_preserves_screen_baseline(primary_closure);
  float3 screen_radiance = imageLoadFast(ray_radiance_img, texel).rgb;
  float screen_ray_time = imageLoadFast(ray_time_img, texel).x;
  if (!precombine_specular_caustics_phase && preserve_screen_baseline &&
      !hardware_hit_payload_exists(texel) &&
      dot(screen_radiance, screen_radiance) > 1.0e-10f &&
      screen_ray_time > 0.0f &&
      screen_ray_time < 9999.0f)
  {
    return;
  }

  float3 P_hit, V;
  if (!hardware_hit_load(texel, P_hit, V)) {
    if (precombine_specular_caustics_phase) {
      imageStoreFast(ray_time_img, texel, float4(10000.0f));
      imageStoreFast(ray_radiance_img, texel, float4(0.0f));
    }
    return;
  }

  int2 texel_fullres = raytrace_texel_to_fullres(texel,
                                                 raytrace_buf.trace_pixel_scale,
                                                 1,
                                                 raytrace_buf.trace_pixel_offset);
  if (false && (raytrace_buf.trace_pixel_scale > 1)) {
    texel_fullres = raytrace_representative_fullres_texel(
        texel,
        raytrace_buf.trace_pixel_scale,
        1,
        raytrace_buf.trace_pixel_offset);
  }

  float3 N;
  if (!hardware_hit_normal_load(texel, N)) {
    N = hardware_hit_normal_estimate(texel, P_hit, V);
  }
  float thickness = hardware_hit_thickness_load(texel);
  ClosureUndetermined base_cl = hardware_hit_base_closure_load(texel, N);
  ClosureUndetermined specular_cl = hardware_hit_specular_closure_load(texel, N);
  const bool hit_has_replayed_material = !hardware_hit_uses_proxy_payload(texel);
  const uint hit_identity_flags = imageLoadFast(hit_identity_img, texel).z;
  const bool preserved_layered_scene_final = hardware_hit_is_preserved_layered_scene_final(texel);
  const bool preserved_transparent_scene_final = hardware_hit_is_preserved_transparent_scene_final(
      texel);
  const bool preserved_scene_final_composite = preserved_layered_scene_final ||
                                               preserved_transparent_scene_final;
  const bool direct_lit_refracted_textured_receiver =
      scene_final_specular_phase && !primary_is_diffuse_gi &&
      ((hit_identity_flags & 16u) != 0u);
  const bool primary_requests_resolved_surface =
      !primary_is_diffuse_gi &&
      (hardware_hit_specular_mode(primary_closure) == RAYTRACE_SPECULAR_MODE_FULL_RT);
  const bool hit_prefers_back_radiance = hardware_closure_has_transmission(specular_cl.type);
  const bool allow_diffuse_world_seed = use_hardware_environment || primary_is_diffuse_gi;
  const bool allow_sparse_replay_seed = allow_diffuse_world_seed ||
                                        preserved_transparent_scene_final ||
                                        (scene_final_specular_phase &&
                                         primary_requests_resolved_surface);
  const float4 transmission_layer = hardware_hit_transmission_layer_load(texel);
  const bool has_transmission_layer = preserved_scene_final_composite &&
                                      (transmission_layer.a > 0.5f);
  const float3 transmission_layer_color = max(transmission_layer.rgb, float3(0.0f));
  const float principled_metallic_coverage = hardware_hit_principled_metallic_coverage(texel);
  const bool principled_layered_scene_final =
      hardware_scene_final_is_principled_layered(hit_identity_flags);
  const ClosureUndetermined specular_lighting_cl = hardware_principled_metal_tinted_specular(
      principled_layered_scene_final, specular_cl, base_cl, principled_metallic_coverage);
  const float principled_base_layer_visibility = hardware_principled_base_layer_visibility(
      principled_layered_scene_final, !hit_has_replayed_material, principled_metallic_coverage);
  const float principled_reflection_layer_visibility =
      hardware_principled_reflection_layer_visibility(
          principled_layered_scene_final,
          !hit_has_replayed_material,
          N,
          V,
          specular_lighting_cl,
          principled_metallic_coverage);
  const bool specular_texture_tint_only = hardware_hit_uses_specular_texture_tint_only(
      hit_identity_flags, principled_metallic_coverage, base_cl, specular_cl);
  const bool scene_final_textured_specular_replay =
      hardware_scene_final_is_textured_specular_replay(
          scene_final_specular_phase,
          primary_is_diffuse_gi,
          preserved_layered_scene_final,
          principled_layered_scene_final,
          hit_identity_flags);
  const bool scene_final_sparse_material_replay =
      scene_final_textured_specular_replay && hardware_scene_final_has_sparse_material_replay(texel);
  const bool has_scene_final_receiver_payload =
      (preserved_scene_final_composite && layered_receiver_hit_exists(texel)) ||
      (has_transmission_layer && transmission_receiver_hit_exists(texel));
  float3 caustic_transport_seed = float3(0.0f);

  float3 radiance = float3(0.0f);
  const bool use_scene_final_textured_direct_view_raster = false;

  if (!use_scene_final_textured_direct_view_raster) {
    radiance = (!precombine_specular_caustics_phase && preserve_screen_baseline) ?
                   imageLoadFast(ray_radiance_img, texel).rgb :
                   float3(0.0f);
    if (!precombine_specular_caustics_phase && !preserve_screen_baseline && allow_sparse_replay_seed &&
        hardware_hit_has_sparse_replay_radiance(texel))
    {
      radiance = imageLoadFast(ray_radiance_img, texel).rgb;
    }
    if (direct_lit_refracted_textured_receiver && hardware_hit_uses_proxy_payload(texel) &&
        hardware_hit_closure_is_specular_family(specular_cl.type) &&
        dot(radiance, radiance) > 1.0e-10f)
    {
      float3 metal_color = max(specular_cl.color, float3(0.0f));
      if (!(dot(metal_color, metal_color) > 1.0e-10f)) {
        metal_color = max(base_cl.color, float3(0.0f));
      }
      radiance *= metal_color;
    }
  }
  float3 raster_radiance;
  if (!use_scene_final_textured_direct_view_raster &&
      !precombine_specular_caustics_phase && scene_final_specular_phase && !primary_is_diffuse_gi &&
      allow_scene_final_raster_reuse && !has_scene_final_receiver_payload &&
      !scene_final_textured_specular_replay &&
      hardware_hit_raster_radiance_load(
          texel, P_hit, N, hit_prefers_back_radiance, false, raster_radiance))
  {
    /* When a scene-final mirror exhausts its bounce budget on another specular object, the
     * terminal hit must resolve to the composed surface color. Shading that last glass/metal hit
     * as a fresh light receiver makes the final mirror bounces look opaque. */
    radiance += raster_radiance;
  }
  else if (!use_scene_final_textured_direct_view_raster && precombine_specular_caustics_phase &&
      primary_requests_resolved_surface &&
      hardware_hit_raster_radiance_load(
          texel,
          P_hit,
          N,
          hit_prefers_back_radiance,
          hardware_hit_uses_caustics(),
          raster_radiance))
  {
    /* Keep the visible result fail-closed to RT. Raster may only seed the later receiver-caustics
     * buffer when the preferred direct/world caustic seed is black. */
    caustic_transport_seed = raster_radiance;
  }
  else if (!use_scene_final_textured_direct_view_raster &&
      !precombine_specular_caustics_phase && !scene_final_specular_phase &&
      primary_is_diffuse_gi && !hardware_hit_uses_caustics() &&
      hardware_hit_visible_direct_light_load(texel, P_hit, N, true, raster_radiance))
  {
    /* Primary RT shadows are evaluated before this pass. Diffuse GI can therefore transport the
     * current visible wall/surface direct-light energy instead of re-estimating it from a secondary
     * shadow query that can collapse colored bounce to black. */
    radiance += raster_radiance;
  }
  else if (!use_scene_final_textured_direct_view_raster &&
      !precombine_specular_caustics_phase && !scene_final_specular_phase &&
      allow_scene_final_raster_reuse && !primary_requests_resolved_surface &&
      !hardware_hit_uses_caustics() &&
      hardware_hit_allows_raster_reuse(
          texel, preserve_screen_baseline, hit_has_replayed_material, radiance, base_cl, specular_cl) &&
      hardware_hit_raster_radiance_load(
          texel, P_hit, N, hit_prefers_back_radiance, false, raster_radiance))
  {
    radiance += raster_radiance;
  }
  else if (!use_scene_final_textured_direct_view_raster) {
    float3 base_direct = float3(0.0f);
    float3 base_probe = float3(0.0f);
    bool base_probe_uses_world = false;
    float3 specular_direct = float3(0.0f);
    float3 specular_probe = float3(0.0f);
    bool specular_probe_uses_world = false;
    float3 layered_receiver_radiance = float3(0.0f);
    float3 transmission_layer_radiance = float3(0.0f);
    hardware_hit_closure_light_terms(
        texel_fullres,
        texel,
        P_hit,
        N,
        V,
        base_cl,
        thickness,
        primary_is_diffuse_gi,
        base_direct,
        base_probe,
        base_probe_uses_world);
    hardware_hit_closure_light_terms(texel_fullres,
                                     texel,
                                     P_hit,
                                     N,
                                     V,
                                     specular_lighting_cl,
                                     thickness,
                                     primary_is_diffuse_gi,
                                     specular_direct,
                                     specular_probe,
                                     specular_probe_uses_world);
    if (preserved_scene_final_composite && layered_receiver_hit_exists(texel)) {
      layered_receiver_radiance = layered_receiver_hit_radiance_resolve(
          texel,
          texel_fullres,
          primary_is_diffuse_gi,
          scene_final_textured_specular_replay && scene_final_sparse_material_replay);
    }
    if (has_transmission_layer && transmission_receiver_hit_exists(texel)) {
      transmission_layer_radiance = transmission_receiver_hit_radiance_resolve(
          texel, texel_fullres, primary_is_diffuse_gi);
    }
    else if (has_transmission_layer) {
      hardware_hit_raster_back_radiance_load(
          texel, P_hit, N, true, hardware_hit_uses_caustics(), transmission_layer_radiance);
    }

    const bool scene_final_diffuse_receiver =
        scene_final_specular_phase &&
        ((base_cl.type == CLOSURE_BSDF_DIFFUSE_ID) || (base_cl.type == CLOSURE_BSSRDF_BURLEY_ID)) &&
        hardware_receiver_gi_primary_is_mirror_like(texel_fullres);
    float3 visible_receiver_radiance = float3(0.0f);
    /* Full RT scene-final mirrors must not reuse already-composed raster receiver color as
     * secondary GI. That fallback reflects texture/direct-light structures and double-scaled GI
     * into the mirror when the receiver-photon pass is missing a sample. */
    const bool scene_final_visible_diffuse_receiver = false;
    float3 receiver_gi_radiance = float3(0.0f);
    /* Nuru Secondary GI (Stage A revival): the dedicated receiver-GI kernel traces a small dome
     * from the reflected diffuse hit and samples the light tree (suns + locals) with shadow-ray
     * visibility. Gated by the Secondary GI scene toggle through the dispatch: when disabled the
     * texture stays cleared and the load below returns false. */
    const bool receiver_gi_texture_loaded = hardware_reflected_receiver_gi_load(
        texel, receiver_gi_radiance);
    const bool scene_final_receiver_gi = scene_final_diffuse_receiver &&
                                         receiver_gi_texture_loaded;



    float3 secondary_photon_gi_radiance = float3(0.0f);
    const bool scene_final_secondary_photon_gi = false;
    if (!precombine_specular_caustics_phase) {
      const bool add_probe_terms = primary_is_diffuse_gi || scene_final_specular_phase;
      if (scene_final_receiver_gi) {
        /* Scene-final mirrors use GI traced from the actual reflected diffuse hit. */
        radiance += base_direct;
        radiance += (receiver_gi_radiance * max(base_cl.color, float3(0.0f))) /
                    max(uniform_buf.clamp.indirect_scale, 1.0e-4f);
      }
      else if (scene_final_visible_diffuse_receiver) {
        /* Mirrors can reflect the resolved GI that is already visible for the receiver in the
         * main view. This is only used after the traced hit validates against the visible surface,
         * so off-camera receivers still stay owned by the traced/probe fallback paths. The
         * scene-final resolve applies the indirect scale later, while the visible combined buffer
         * is already scaled. Undo that scale here to avoid brightening reflected screen GI twice.
         * This substitutes only the diffuse/base receiver term; glossy continuation still uses the
         * traced closure below so secondary/probe GI remains visible in reflections. */
        radiance += visible_receiver_radiance / max(uniform_buf.clamp.indirect_scale, 1.0e-4f);
      }
      else {
        if (!specular_texture_tint_only) {
          radiance += base_direct * principled_base_layer_visibility;
          if (add_probe_terms) {
            radiance += base_probe * principled_base_layer_visibility;
          }
        }
      }
      if (!(scene_final_textured_specular_replay && scene_final_sparse_material_replay)) {
        radiance += specular_direct * principled_reflection_layer_visibility;
        if (add_probe_terms) {
          radiance += specular_probe * principled_reflection_layer_visibility;
        }
      }
      else if (add_probe_terms) {
        /* Replayed mirror proxies keep analytic direct off the terminal hit, but still need the
         * unshadowed world specular lobe on the replayed material itself. */
        radiance += specular_probe * principled_reflection_layer_visibility;
      }
      if (scene_final_secondary_photon_gi) {
        radiance += secondary_photon_gi_radiance /
                    max(uniform_buf.clamp.indirect_scale, 1.0e-4f);
      }
      const bool suppress_layered_receiver_on_textured_specular =
          hardware_scene_final_suppress_layered_receiver_on_textured_specular(
              preserved_layered_scene_final, principled_layered_scene_final, texel);
      if (preserved_layered_scene_final &&
          dot(layered_receiver_radiance, layered_receiver_radiance) > 1.0e-10f)
      {
        /* Metal reflection layer composite: mirror the Principled transmission fade pattern.
         * The replayed shader graph bakes the metallic factor into `specular_lighting_cl.color`
         * (= base_color * metallic). The proxy fallback stores the untinted base color on the
         * reflection slot, so weight it explicitly by the sync-time metallic coverage so the
         * world-reflection contribution fades smoothly with the metallic slider. */
        const float3 metal_layer_color = max(specular_lighting_cl.color, float3(0.0f));
        if (scene_final_textured_specular_replay && scene_final_sparse_material_replay)
        {
          radiance += layered_receiver_radiance * metal_layer_color *
                      principled_reflection_layer_visibility;
        }
        else if (scene_final_textured_specular_replay && !scene_final_sparse_material_replay &&
            !suppress_layered_receiver_on_textured_specular)
        {
          radiance += layered_receiver_radiance * metal_layer_color *
                      principled_reflection_layer_visibility;
        }
        else if (!suppress_layered_receiver_on_textured_specular &&
                 layered_receiver_hit_uses_proxy_payload(texel))
        {
          radiance += layered_receiver_radiance * metal_layer_color *
                      principled_reflection_layer_visibility;
        }
        else if (!suppress_layered_receiver_on_textured_specular) {
          radiance += layered_receiver_radiance * principled_reflection_layer_visibility;
        }
      }
      if (has_transmission_layer &&
          dot(transmission_layer_radiance, transmission_layer_radiance) > 1.0e-10f)
      {
        radiance += transmission_layer_radiance * transmission_layer_color;
      }
      if (preserved_transparent_scene_final &&
          dot(layered_receiver_radiance, layered_receiver_radiance) > 1.0e-10f)
      {
        radiance += layered_receiver_radiance;
      }
    }

    if (!scene_final_specular_phase && hardware_hit_caustics_eligible(primary_closure, base_cl))
    {
      hardware_hit_caustics_resolve(
          texel,
          texel_fullres,
          P_hit,
          N,
          V,
          base_direct,
          base_probe,
          base_probe_uses_world,
          caustic_transport_seed,
          hardware_hit_caustics_roughness_weight(primary_closure));
    }
    if (hardware_hit_reflected_receiver_caustics_eligible(
            scene_final_specular_phase, primary_closure, base_cl))
    {
      float3 scene_final_caustics = hardware_hit_caustics_target(texel,
                                                                 texel_fullres,
                                                                 P_hit,
                                                                 V,
                                                                 base_direct,
                                                                 base_probe,
                                                                 base_probe_uses_world,
                                                                 caustic_transport_seed);
      if (has_transmission_layer) {
        float3 transmission_P;
        float3 transmission_V;
        if (transmission_receiver_hit_load(texel, transmission_P, transmission_V)) {
          scene_final_caustics = hardware_hit_caustics_target(texel,
                                                              texel_fullres,
                                                              transmission_P,
                                                              transmission_V,
                                                              base_direct,
                                                              base_probe,
                                                              base_probe_uses_world,
                                                              caustic_transport_seed);
        }
        scene_final_caustics *= max(transmission_layer_color, float3(0.0f));
      }
      else {
        scene_final_caustics *= 2.5f;
      }
      scene_final_caustics *= hardware_hit_caustics_roughness_weight(primary_closure);
      radiance += scene_final_caustics;
    }
  }
  radiance = colorspace_brightness_clamp_max(radiance, uniform_buf.clamp.surface_indirect);
  imageStoreFast(ray_radiance_img, texel, float4(radiance, 0.0f));
}
