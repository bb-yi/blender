/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Evaluate shadowing using shadow map ray-tracing.
 */

#include "draw_math_geom_lib.glsl"
#include "draw_view.bsl.hh"
#include "eevee_light_lib.bsl.hh"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_shadow.bsl.hh"
#include "eevee_thickness_lib.bsl.hh"
#include "eevee_uniform.bsl.hh"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_vector_safe_lib.glsl"
#include "gpu_shader_ray_utils_lib.glsl"

namespace eevee {

/* ---------------------------------------------------------------------- */
/** \name Shadow Map Tracing loop
 * \{ */

#define SHADOW_TRACING_INVALID_HISTORY FLT_MAX

struct ShadowMapTracingState {
  /* Occluder ray coordinate at previous valid depth sample. */
  float2 occluder_history;
  /* Time slope between previous valid sample (N-1) and the one before that (N-2). */
  float occluder_slope;
  /* Multiplier and bias to the ray step quickly compute ray time. */
  float ray_step_mul;
  float ray_step_bias;
  /* State of the trace. */
  float ray_time;
  bool hit;
};

ShadowMapTracingState shadow_map_trace_init(int sample_count, float step_offset)
{
  ShadowMapTracingState state;
  state.occluder_history = float2(SHADOW_TRACING_INVALID_HISTORY);
  state.occluder_slope = SHADOW_TRACING_INVALID_HISTORY;
  /* We trace the ray in reverse. From 1.0 (light) to 0.0f (shading point). */
  state.ray_step_mul = -1.0f / float(sample_count);
  state.ray_step_bias = 1.0f + step_offset * state.ray_step_mul;
  state.hit = false;
  return state;
}

struct ShadowTracingSample {
  /**
   * Occluder position in ray space.
   * `x` component is just the normalized distance from the ray start to the ray end.
   * `y` component is signed distance to the ray, positive if on the light side of the ray.
   */
  float2 occluder;
  bool skip_sample;
};

/**
 * We trace from a point on the light towards the shading point.
 *
 * This reverse tracing allows to approximate the geometry behind occluders while minimizing
 * light-leaks.
 */
void shadow_map_trace_hit_check(ShadowMapTracingState &state,
                                ShadowTracingSample samp,
                                bool is_last_sample)
{
  bool is_behind_occluder = samp.occluder.y > 1e-6f;

  if (samp.skip_sample) {
    /* Skip empty tiles since they do not contain actual depth information.
     * Not doing so would change the z gradient history. */
  }
  else if (state.occluder_history.x == SHADOW_TRACING_INVALID_HISTORY) {
    /* First sample, regular depth compare since we do not have history values. */
    state.hit = is_behind_occluder || (is_last_sample && (samp.occluder.x > state.ray_time));
    state.occluder_history = samp.occluder;
  }
  else if (is_behind_occluder && (state.occluder_slope != SHADOW_TRACING_INVALID_HISTORY)) {
    /* Extrapolate last known valid occluder and check if it crossed the ray.
     * Note that we only want to check if the extrapolated occluder is above the ray at a certain
     * time value, we don't actually care about the correct value. So we replace the complex
     * problem of trying to get the extrapolation in shadow map space into the extrapolation at
     * ray_time in ray space. This is equivalent as both functions have the same roots. */
    float delta_time = state.ray_time - state.occluder_history.x;
    float extrapolated_occluder_y = abs(state.occluder_history.y) +
                                    state.occluder_slope * delta_time;
    /* NOTE: We use the absolute of the function to account for all occluders configurations.
     * The test just checks if it doesn't extrapolate in the other Y region. */
    state.hit = extrapolated_occluder_y < 0.0f;
  }
  else {
    /* Compute current occluder slope and record history for when the ray goes behind a surface. */
    float2 delta = samp.occluder - state.occluder_history;
    /* Clamping the slope to a minimum avoid light leaking. */
    /* TODO(@fclem): Expose as parameter? */
    const float min_slope = tan(M_PI * 0.25f);
    state.occluder_slope = max(min_slope, abs(delta.y / delta.x));
    state.occluder_history = samp.occluder;
    /* Intersection test. Intersect if above the ray time. */
    state.hit = is_behind_occluder || (is_last_sample && (samp.occluder.x > state.ray_time));
  }
}

/**
 * This need to be instantiated for each `ShadowRay*` type.
 * This way we can implement `shadow_map_trace_sample` for each type without too much code
 * duplication.
 * Most of the code is wrapped into functions to avoid to debug issues inside macro code.
 */
template<typename ShadowRayType>
bool shadow_map_trace([[resource_table]] ShadowRenderData &srd,
                      ShadowRayType ray,
                      int sample_count,
                      float step_offset)
{
  ShadowMapTracingState state = shadow_map_trace_init(sample_count, step_offset);
  for (int i = 0; (i <= sample_count) && (i <= SHADOW_MAX_STEP) && (state.hit == false); i++)
  { /* Saturate to always cover the shading point position when i == sample_count. */
    state.ray_time = square(saturate(float(i) * state.ray_step_mul + state.ray_step_bias));

    ShadowTracingSample samp = shadow_map_trace_sample(srd, state, ray);

    shadow_map_trace_hit_check(state, samp, i == sample_count);
  }
  return state.hit;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Directional Shadow Map Tracing
 * \{ */

struct ShadowRayDirectional {
  /* Ray in light rotated space. But not translated. */
  float3 origin;
  float3 direction;
  /* Convert form local light position to ray oriented position where X axis is the ray. */
  float3 local_ray_up;
  LightData light;
};

/* `lP` is supposed to be in light rotated space. But not translated. */
ShadowRayDirectional shadow_ray_generate_directional(LightData light,
                                                     float2 random_2d,
                                                     float3 lP,
                                                     float texel_radius,
                                                     float receiver_pixel_size)
{
  float clip_near = orderedIntBitsToFloat(light.clip_near);
  /* Assumed to be non-null. */
  float dist_to_near_plane = -lP.z - clip_near;
  /* Trace in a radius that is covered by low resolution page inflation. */
  float max_tracing_distance = texel_radius * float(SHADOW_PAGE_RES << SHADOW_TILEMAP_LOD);
  float max_tracing_angle_cos = cos_from_tan(max_tracing_distance / dist_to_near_plane);
  /* Taking max of cosines to get the minimum of the angles. */
  float shadow_angle_cos = max(light.sun().shadow_angle_cos, max_tracing_angle_cos);

  /* Light shape is 1 unit away from the shading point. */
  float3 direction = sample_uniform_cone(random_2d, shadow_angle_cos);

  /* It only make sense to trace where there can be occluder. Clamp by distance to near plane. */
  direction *= max(texel_radius, dist_to_near_plane / direction.z);

  ShadowRayDirectional ray;
  ray.origin = lP;
  ray.direction = direction;
  ray.light = light;
  if (receiver_pixel_size > 0.0f) {
    /* Coverage still selects a containing layer. Fold the per-receiver screen demand
     * into the existing minimum before tracing, so it is constant across every step.
     * The bias is already included in that demand and must not scale coverage. */
    float requested_scale = receiver_pixel_size * (0.5f * SHADOW_MAP_MAX_RES) * exp2(light.lod_bias);
    ray.light.lod_min = max(light.lod_min, float(shadow_directional_scale_level_ceil(requested_scale)));
    ray.light.lod_bias = 0.0f;
  }
  /* TODO(fclem): We can simplify this using the ray direction construction. */
  ray.local_ray_up = safe_normalize(
      cross(cross(float3(0.0f, 0.0f, -1.0f), ray.direction), ray.direction));
  return ray;
}

ShadowTracingSample shadow_map_trace_sample([[resource_table]] ShadowRenderData &srd,
                                            ShadowMapTracingState state,
                                            ShadowRayDirectional &ray)
{
  /* Ray position is ray local position with origin at light origin. */
  float3 ray_pos = ray.origin + ray.direction * state.ray_time;

  ShadowCoordinates coord = shadow_directional_coordinates(ray.light, ray_pos);

  float depth = srd.read_depth(coord);
  /* Distance from near plane. */
  float clip_near = orderedIntBitsToFloat(ray.light.clip_near);
  float3 occluder_pos = float3(ray_pos.xy, -depth - clip_near);
  /* Transform to ray local space. */
  float3 ray_local_occluder = occluder_pos - ray.origin;

  ShadowTracingSample samp;
  samp.occluder.x = dot(ray_local_occluder, ray.direction) / length_squared(ray.direction);
  samp.occluder.y = dot(ray_local_occluder, ray.local_ray_up);
  samp.skip_sample = (depth == -1.0f);
  return samp;
}

template bool shadow_map_trace<ShadowRayDirectional>(ShadowRenderData &,
                                                     ShadowRayDirectional,
                                                     int,
                                                     float);

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Punctual Shadow Map Tracing
 * \{ */

struct ShadowRayPunctual {
  /* Light space shadow ray origin and direction. */
  float3 origin;
  float3 direction;
  /* Convert form local light position to ray oriented position where X axis is the ray. */
  float3 local_ray_up;
  /* Tile-map to sample. */
  int light_tilemap_index;
  LightData light;
};

/* Return ray in UV clip space [0..1]. */
ShadowRayPunctual shadow_ray_generate_punctual(LightData light, float2 random_2d, float3 lP)
{
  if (light.type == LIGHT_RECT) {
    random_2d = random_2d * 2.0f - 1.0f;
  }
  else {
    random_2d = sample_disk(random_2d);
  }

  float clip_near = intBitsToFloat(light.clip_near);
  float shape_radius = light.spot().local.shadow_radius;
  /* Clamp to a minimum value to avoid `local_ray_up` being degenerate. Could be revisited as the
   * issue might reappear at different zoom level. */
  shape_radius = max(0.00002f, shape_radius);

  float3 point_on_light_shape;
  if (is_area_light(light.type)) {
    random_2d *= light.area().size * light.area().shadow_scale;

    point_on_light_shape = float3(random_2d, 0.0f);
  }
  else {
    float dist;
    float3 lL = normalize_and_get_length(lP, dist);
    /* Disk rotated towards light vector. */
    float3 right, up;
    make_orthonormal_basis(lL, right, up);

    if (is_sphere_light(light.type)) {
      shape_radius = light_sphere_disk_radius(shape_radius, dist);
    }
    random_2d *= shape_radius;

    point_on_light_shape = right * random_2d.x + up * random_2d.y;
  }

  /* Avoid numerical issue when random point is exactly on the light center. */
  if (length_squared(point_on_light_shape) < 1e-8f) {
    point_on_light_shape = float3(1e-6f);
  }

  float3 direction = point_on_light_shape - lP;

  float3 shadow_position = light.local().local.shadow_position;
  /* Clip the ray to not cross the near plane.
   * Avoid traces that starts on tiles that have not been queried, creating noise. */
  float clip_distance = max(0.0f, length(lP - shadow_position) - clip_near);
  /* Still clamp to a minimal size to avoid issue with zero length vectors. */
  direction *= saturate(1e-6f + clip_distance * inversesqrt(length_squared(direction)));

  /* Compute the ray again. */
  ShadowRayPunctual ray;
  /* Transform to shadow local space. */
  ray.origin = lP - shadow_position;
  ray.direction = direction + shadow_position;
  ray.light_tilemap_index = light.tilemap_index;
  ray.local_ray_up = safe_normalize(cross(cross(ray.origin, ray.direction), ray.direction));
  ray.light = light;
  return ray;
}

ShadowTracingSample shadow_map_trace_sample([[resource_table]] ShadowRenderData &srd,
                                            ShadowMapTracingState state,
                                            ShadowRayPunctual &ray)
{
  float3 receiver_pos = ray.origin + ray.direction * state.ray_time;
  int face_id = shadow_punctual_face_index_get(receiver_pos);
  float3 face_pos = shadow_punctual_local_position_to_face_local(face_id, receiver_pos);
  ShadowCoordinates coord = shadow_punctual_coordinates(ray.light, face_pos, face_id);

  float radial_occluder_depth = srd.read_depth(coord);
  float3 occluder_pos = receiver_pos * (radial_occluder_depth / length(receiver_pos));

  /* Transform to ray local space. */
  float3 ray_local_occluder = occluder_pos - ray.origin;

  ShadowTracingSample samp;
  samp.occluder.x = dot(ray_local_occluder, ray.direction) / length_squared(ray.direction);
  samp.occluder.y = dot(ray_local_occluder, ray.local_ray_up);
  samp.skip_sample = (radial_occluder_depth == -1.0f);
  return samp;
}

template bool shadow_map_trace<ShadowRayPunctual>(ShadowRenderData &,
                                                  ShadowRayPunctual,
                                                  int,
                                                  float);

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Shadow Evaluation
 * \{ */

/* Compute the world space offset of the shading position required for
 * stochastic percentage closer filtering of shadow-maps. */
float3 shadow_pcf_offset(float3 L, float3 Ng, float2 random)
{
  /* Angle between Light and normal. */
  float cos_theta = abs(dot(L, Ng));
  float sin_theta = sin_from_cos(cos_theta);
  /* Slope of the receiver plane with respect to light direction. Equal to `tan(theta)`.
   * Stop at 45 degrees angle to avoid large bias and peter panning artifacts. */
  float cone_height = saturate(sin_theta * safe_rcp(cos_theta));
  /* We choose a random disk distribution because it is rotationally invariant.
   * This saves us the trouble of getting the correct orientation for punctual. */
  float distance_to_center = sqrt(random.x);
  float2 disk_sample = sample_circle(random.y) * distance_to_center;
  /* Set the samples on a cone up to 45 degree. */
  float3 cone_sample = float3(disk_sample, distance_to_center * cone_height);
  /* Setup the cone around the light vector. */
  float3 pcf_offset = from_up_axis(L) * cone_sample;
  /* Offset the cone in normal direction to avoid self shadowing
   * when angle is greater than 45 degrees. */
  pcf_offset += Ng * saturate(sin_theta - cos_theta);
  return pcf_offset;
}

/**
 * Returns the world space radius of a shadow map texel at a given position.
 * This is a smooth (not discretized to the LOD transitions) conservative (always above actual
 * density) estimate value.
 */
float shadow_texel_radius_at_position([[resource_table]] const Uniform &uni,
                                      [[resource_table]] const draw::View &views,
                                      LightData light,
                                      const bool is_directional,
                                      float3 P)
{
  /* For direction, footprint of the sampled clipmap (or cascade) at the given position.
   * For punctual, footprint of the tilemap at given position scaled by the LOD level.
   * Each of these a smooth upper bound estimation (will transition smoothly to the next level). */
  float scale = 1.0f;
  if (is_directional) {
    float3 lP = transform_direction_transposed(light.object_to_world, P);
    lP -= light.position();
    LightSunData sun = light.sun();
    if (light.type == LIGHT_SUN) {
      const ViewMatrices view = views.get(0);
      float pixel_size = shadow_screen_pixel_size(uni.uniform_buf.shadow.film_pixel_radius,
                                                  view.is_perspective(), view.z_distance(P));
      scale = shadow_directional_clipmap_scale(light, lP, pixel_size);
    }
    else {
      /* Uniform distribution everywhere. No distance scaling.
       * shadow_directional_level_fractional returns the cascade level, but all levels have the
       * same density as the level 0. So the effective density only depends on the `lod_bias`. */
      scale = exp2(float(sun.clipmap_lod_min));
    }
  }
  else {
    const ViewMatrices view = views.get(0);

    float3 lP = light_world_to_local_point(light, P);
    lP -= light.local().local.shadow_position;
    /* Simplification of `exp2(shadow_punctual_level_fractional)`. */
    scale = shadow_punctual_pixel_ratio(light,
                                        lP,
                                        view.is_perspective(),
                                        view.z_distance(P),
                                        uni.uniform_buf.shadow.film_pixel_radius);
    /* This gives the size of pixels at Z = 1. */
    scale = 1.0f / scale;
    scale = min(scale, float(1 << SHADOW_TILEMAP_LOD));
    /* Now scale by distance to the light. */
    scale *= reduce_max(abs(lP));
  }
  /* Pixel bounding radius inside a tilemap of unit scale.
   * Take only half of it because we want the radius and not the diameter. */
  constexpr float texel_radius = M_SQRT2 / SHADOW_MAP_MAX_RES;
  return texel_radius * scale;
}

/**
 * Compute the amount of offset to add to the shading point in the normal direction to avoid self
 * shadowing caused by aliasing artifacts. This is on top of the slope bias computed in the shadow
 * render shader to avoid aliasing issues of other polygons. The slope bias only fixes the self
 * shadowing from the current polygon, which is not enough in cases with adjacent polygons with
 * very different slopes.
 */
float shadow_normal_offset(float3 Ng, float3 L, float texel_radius)
{
  /* Attenuate depending on light angle. */
  float cos_theta = abs(dot(Ng, L));
  float slope_offset = sin_from_cos(cos_theta);

  /* Ng might have been quantized. Compensate the error by scaling the offset. */
  const float max_angular_quantization_error = 0.534f; /* Radians. */
  const float max_error_cos_inv = 1.0f / cos(max_angular_quantization_error);
  /* The scaling is only to fix the self shadowing we need another bias for shadowing of adjacent
   * polygons. */
  const float max_error_adjacent_polygon = 0.195f; /* Eye-balled. */
  float biased_offset = slope_offset * max_error_cos_inv + max_error_adjacent_polygon;

  return biased_offset * texel_radius;
}

float shadow_terminator_offset(float3 N,
                               float3 L,
                               float shadow_terminator_normal_offset,
                               float shadow_terminator_geometry_offset)
{
  const float offset_cutoff = shadow_terminator_geometry_offset;

  if (shadow_terminator_geometry_offset == 0.0) {
    return 0.0;
  }

  float cos_theta = dot(N, L);
  const float offset_amount = saturate(1.0f - cos_theta / offset_cutoff);
  return offset_amount * shadow_terminator_normal_offset;
}

#define SHADOW_NODE_STABLE_MAX_RAY 32

/** Keep numerical ray offsets smaller than nearby features at fine shadow resolutions. */
float3 shadow_ray_offset(float3 P, float3 Ng, float texel_radius)
{
  /* The generic ray offset advances by up to 256 ULP. At tiny scene scales that can move the
   * receiver through an entire blocker. Keep its established offset for ordinary scenes, but
   * bound each component by the shadow footprint and a world-coordinate error allowance. */
  float3 offset_bound = max(float3(4.0f * texel_radius), abs(P) * (4.0f * FLT_EPSILON));
  return P + clamp(offset_ray(P, Ng) - P, -offset_bound, offset_bound);
}

float2 shadow_stable_hammersley_2d(int sample_index, int sample_count, float2 rotation)
{
  return fract(hammersley_2d(sample_index, sample_count) + rotation);
}

/**
 * Deterministic shadow evaluation for node outputs that need stable grayscale penumbra values in
 * a single frame, without relying on Eevee's temporal accumulation.
 */
float shadow_eval_stable([[resource_table]] ShadowRenderData &srd,
                         LightData light,
                         const bool is_directional,
                         const bool is_transmission,
                         bool is_translucent_with_thickness,
                         Thickness thickness,
                         float3 P,
                         float3 Ng,
                         float3 N,
                         float terminator_normal_offset,
                         float terminator_geometry_offset,
                         int ray_count,
                         int ray_step_count)
{
  int stable_ray_count = clamp(ray_count, 1, SHADOW_NODE_STABLE_MAX_RAY);
  int stable_step_count = clamp(ray_step_count, 1, SHADOW_MAX_STEP);

  [[resource_table]] const Uniform &uni = srd.uniforms;
  [[resource_table]] const draw::View &views = srd.views;

  float distance_to_shadow;
  float3 L;
  if (is_directional) {
    L = light.z_axis();
  }
  else {
    L = light.position() + light.local().local.shadow_position - P;
    L = normalize_and_get_length(L, distance_to_shadow);
  }

  bool is_facing_light = (dot(Ng, L) > 0.0f);
  float3 N_bias = (is_transmission && !is_facing_light) ? reflect(Ng, L) : Ng;
  float texel_radius = shadow_texel_radius_at_position(uni, views, light, is_directional, P);

  float3 P_base = P;
  if (is_transmission && !is_facing_light) {
    P_base += abs(is_directional ? thickness.value() :
                                   min(thickness.value(), distance_to_shadow - 0.01f)) *
              L;
  }

  P_base = shadow_ray_offset(P_base, N_bias, texel_radius);
  P_base += N_bias * shadow_normal_offset(Ng, L, texel_radius);
  P_base += N * shadow_terminator_offset(
                    N, L, terminator_normal_offset, terminator_geometry_offset);

  float surface_hit = 0.0f;
  const ViewMatrices receiver_view = views.get(0);
  float receiver_pixel_size = is_directional ?
      shadow_screen_pixel_size(uni.uniform_buf.shadow.film_pixel_radius,
                               receiver_view.is_perspective(), receiver_view.z_distance(P)) : 0.0f;
  for (int ray_index = 0; ray_index < SHADOW_NODE_STABLE_MAX_RAY; ray_index++) {
    if (ray_index >= stable_ray_count) {
      break;
    }

    float2 ray_tap = shadow_stable_hammersley_2d(
        ray_index, stable_ray_count, float2(0.0f, 0.0f));
    float2 pcf_tap = shadow_stable_hammersley_2d(
        ray_index, stable_ray_count, float2(0.5f, 0.25f));
    float3 P_sample = P_base +
                      (light.filter_radius * texel_radius) * shadow_pcf_offset(L, Ng, pcf_tap);
    float3 lP = is_directional ? light_world_to_local_direction(light, P_sample) :
                                 light_world_to_local_point(light, P_sample);

    bool has_hit;
    if (is_directional) {
      ShadowRayDirectional clip_ray = shadow_ray_generate_directional(
          light, ray_tap, lP, texel_radius,
          receiver_pixel_size);
      has_hit = shadow_map_trace(srd, clip_ray, stable_step_count, 0.5f);
    }
    else {
      ShadowRayPunctual clip_ray = shadow_ray_generate_punctual(light, ray_tap, lP);
      has_hit = shadow_map_trace(srd, clip_ray, stable_step_count, 0.5f);
    }
    surface_hit += float(has_hit);
  }

  return saturate(1.0f - surface_hit / float(stable_ray_count));
}

/**
 * Evaluate shadowing by casting rays toward the light direction.
 * Returns light visibility.
 */
float shadow_eval_seeded([[resource_table]] ShadowRenderData &srd,
                         LightData light,
                         const bool is_directional,
                         const bool is_transmission,
                         bool is_translucent_with_thickness,
                         Thickness thickness, /* Only used if is_transmission is true. */
                         float3 P,
                         float3 Ng,
                         float3 N,
                         float terminator_normal_offset,
                         float terminator_geometry_offset,
                         int ray_count,
                         int ray_step_count,
                         float3 random_shadow_3d,
                         float2 random_pcf_2d)
{
  [[resource_table]] const Uniform &uni = srd.uniforms;
  [[resource_table]] const draw::View &views = srd.views;

  float distance_to_shadow;
  /* Direction towards the shadow center (punctual) or direction (direction).
   * Not the same as the light vector if the shadow is jittered. */
  float3 L;
  if (is_directional) {
    L = light.z_axis();
  }
  else {
    L = light.position() + light.local().local.shadow_position - P;
    L = normalize_and_get_length(L, distance_to_shadow);
  }

  bool is_facing_light = (dot(Ng, L) > 0.0f);
  /* Still bias the transmission surfaces towards the light if they are facing away. */
  float3 N_bias = (is_transmission && !is_facing_light) ? reflect(Ng, L) : Ng;

  /* Shadow map texel radius at the receiver position. */
  float texel_radius = shadow_texel_radius_at_position(uni, views, light, is_directional, P);

  if (is_transmission && !is_facing_light) {
    /* Ideally, we should bias using the chosen ray direction. In practice, this conflict with our
     * shadow tile usage tagging system as the sampling position becomes heavily shifted from the
     * tagging position. This is the same thing happening with missing tiles with large radii. */
    P += abs(is_directional ? thickness.value() :
                              min(thickness.value(), distance_to_shadow - 0.01f)) *
         L;
  }
  /* Avoid self intersection with respect to numerical precision. */
  P = shadow_ray_offset(P, N_bias, texel_radius);
  /* Stochastic Percentage Closer Filtering. */
  P += (light.filter_radius * texel_radius) * shadow_pcf_offset(L, Ng, random_pcf_2d);
  /* Add normal bias to avoid aliasing artifacts. */
  P += N_bias * shadow_normal_offset(Ng, L, texel_radius);

  /* Bias more to avoid terminator artifacts. */
  P += N * shadow_terminator_offset(N, L, terminator_normal_offset, terminator_geometry_offset);

  float3 lP = is_directional ? light_world_to_local_direction(light, P) :
                               light_world_to_local_point(light, P);

  float surface_hit = 0.0f;
  const ViewMatrices receiver_view = views.get(0);
  float receiver_pixel_size = is_directional ?
      shadow_screen_pixel_size(uni.uniform_buf.shadow.film_pixel_radius,
                               receiver_view.is_perspective(), receiver_view.z_distance(P)) : 0.0f;
  for (int ray_index = 0; ray_index < ray_count && ray_index < SHADOW_MAX_RAY; ray_index++) {
    float2 random_ray_2d = fract(hammersley_2d(ray_index, ray_count) + random_shadow_3d.xy);

    bool has_hit;
    if (is_directional) {
      ShadowRayDirectional clip_ray = shadow_ray_generate_directional(
          light, random_ray_2d, lP, texel_radius,
          receiver_pixel_size);
      has_hit = shadow_map_trace(srd, clip_ray, ray_step_count, random_shadow_3d.z);
    }
    else {
      ShadowRayPunctual clip_ray = shadow_ray_generate_punctual(light, random_ray_2d, lP);
      has_hit = shadow_map_trace(srd, clip_ray, ray_step_count, random_shadow_3d.z);
    }

    surface_hit += float(has_hit);
  }
  /* Average samples. */
  return saturate(1.0f - surface_hit / float(ray_count));
}

constexpr uint SHADOW_CASTER_UNKNOWN_ID = 0xFFFFFFFFu;

struct ShadowCasterTraceResult {
  bool hit;
  uint caster_id;
};

uint shadow_caster_id_read([[resource_table]] ShadowRenderData &srd, ShadowCoordinates coord)
{
  ShadowSamplingTile tile = shadow_tile_load(
      srd.shadow_tilemaps_tx, coord.tilemap_tile, coord.tilemap_index);
  if (!tile.is_valid) {
    return SHADOW_CASTER_UNKNOWN_ID;
  }

  constexpr uint page_shift = uint(SHADOW_PAGE_LOD);
  constexpr uint page_mask = ~(0xFFFFFFFFu << uint(SHADOW_PAGE_LOD));

  uint2 texel = coord.tilemap_texel;
  texel += uint2(tile.lod_offset << SHADOW_PAGE_LOD);
  uint2 texel_page = (texel >> tile.lod) & page_mask;
  texel = (uint2(tile.page.xy) << page_shift) | texel_page;

  uint packed_value = texelFetch(
                          srd.shadow_caster_atlas_tx, int3(int2(texel), int(tile.page.z)), 0)
                          .r;
  if (packed_value == 0xFFFFFFFFu) {
    return SHADOW_CASTER_UNKNOWN_ID;
  }
  return packed_value & 0xFFFFu;
}

ShadowCoordinates shadow_caster_sample_coord(ShadowRayDirectional ray, float ray_time)
{
  float3 ray_pos = ray.origin + ray.direction * ray_time;
  return shadow_directional_coordinates(ray.light, ray_pos);
}

ShadowCoordinates shadow_caster_sample_coord(ShadowRayPunctual ray, float ray_time)
{
  float3 receiver_pos = ray.origin + ray.direction * ray_time;
  int face_id = shadow_punctual_face_index_get(receiver_pos);
  float3 face_pos = shadow_punctual_local_position_to_face_local(face_id, receiver_pos);
  return shadow_punctual_coordinates(ray.light, face_pos, face_id);
}

template<typename ShadowRayType>
ShadowCasterTraceResult shadow_trace_caster_id([[resource_table]] ShadowRenderData &srd,
                                               ShadowRayType ray,
                                               int sample_count,
                                               float step_offset)
{
  ShadowMapTracingState state = shadow_map_trace_init(sample_count, step_offset);
  uint caster_id = SHADOW_CASTER_UNKNOWN_ID;
  for (int i = 0; (i <= sample_count) && (i <= SHADOW_MAX_STEP) && (state.hit == false); i++)
  { /* Saturate to always cover the shading point position when i == sample_count. */
    state.ray_time = square(saturate(float(i) * state.ray_step_mul + state.ray_step_bias));

    ShadowTracingSample samp = shadow_map_trace_sample(srd, state, ray);
    if (!samp.skip_sample) {
      caster_id = shadow_caster_id_read(srd, shadow_caster_sample_coord(ray, state.ray_time));
    }
    shadow_map_trace_hit_check(state, samp, i == sample_count);
  }

  ShadowCasterTraceResult result;
  result.hit = state.hit;
  result.caster_id = state.hit ? caster_id : SHADOW_CASTER_UNKNOWN_ID;
  return result;
}

template ShadowCasterTraceResult shadow_trace_caster_id<ShadowRayDirectional>(
    ShadowRenderData &, ShadowRayDirectional, int, float);
template ShadowCasterTraceResult shadow_trace_caster_id<ShadowRayPunctual>(
    ShadowRenderData &, ShadowRayPunctual, int, float);

float3 shadow_caster_classification_seeded_ex([[resource_table]] ShadowRenderData &srd,
                                           LightData light,
                                           const bool is_directional,
                                           uint receiver_id,
                                           float3 P,
                                           float3 Ng,
                                           float3 N,
                                           float terminator_normal_offset,
                                           float terminator_geometry_offset,
                                           int ray_count,
                                           int ray_step_count,
                                           float3 random_shadow_3d,
                                           float2 random_pcf_2d,
                                           const bool is_transmission,
                                           Thickness thickness)
{
  [[resource_table]] const Uniform &uni = srd.uniforms;
  [[resource_table]] const draw::View &views = srd.views;

  if (light.tilemap_index == LIGHT_NO_SHADOW || !uni.uniform_buf.shadow.use_caster_atlas) {
    return float3(1.0f, 0.0f, 0.0f);
  }

  int safe_ray_count = clamp(ray_count, 1, SHADOW_MAX_RAY);
  int safe_step_count = clamp(ray_step_count, 1, SHADOW_MAX_STEP);
  uint self_id = receiver_id & 0xFFFFu;

  float distance_to_shadow;
  float3 L;
  if (is_directional) {
    L = light.z_axis();
  }
  else {
    L = light.position() + light.local().local.shadow_position - P;
    L = normalize_and_get_length(L, distance_to_shadow);
  }

  float texel_radius = shadow_texel_radius_at_position(uni, views, light, is_directional, P);
  bool is_facing_light = dot(Ng, L) > 0.0f;
  float3 N_bias = (is_transmission && !is_facing_light) ? reflect(Ng, L) : Ng;
  if (is_transmission && !is_facing_light) {
    P += abs(is_directional ? thickness.value() :
                             min(thickness.value(), distance_to_shadow - 0.01f)) * L;
  }
  P = shadow_ray_offset(P, N_bias, texel_radius);
  P += (light.filter_radius * texel_radius) * shadow_pcf_offset(L, Ng, random_pcf_2d);
  P += N_bias * shadow_normal_offset(Ng, L, texel_radius);
  P += N * shadow_terminator_offset(N, L, terminator_normal_offset, terminator_geometry_offset);

  float3 lP = is_directional ? light_world_to_local_direction(light, P) :
                               light_world_to_local_point(light, P);

  float self_hit = 0.0f;
  float cast_hit = 0.0f;
  float unknown_hit = 0.0f;
  const ViewMatrices receiver_view = views.get(0);
  float receiver_pixel_size = is_directional ?
      shadow_screen_pixel_size(uni.uniform_buf.shadow.film_pixel_radius,
                               receiver_view.is_perspective(), receiver_view.z_distance(P)) : 0.0f;
  for (int ray_index = 0; ray_index < SHADOW_MAX_RAY; ray_index++) {
    if (ray_index >= safe_ray_count) {
      break;
    }

    float2 random_ray_2d = fract(hammersley_2d(ray_index, safe_ray_count) + random_shadow_3d.xy);
    ShadowCasterTraceResult trace_result;
    if (is_directional) {
      ShadowRayDirectional clip_ray = shadow_ray_generate_directional(
          light, random_ray_2d, lP, texel_radius,
          receiver_pixel_size);
      trace_result = shadow_trace_caster_id(srd, clip_ray, safe_step_count, random_shadow_3d.z);
    }
    else {
      ShadowRayPunctual clip_ray = shadow_ray_generate_punctual(light, random_ray_2d, lP);
      trace_result = shadow_trace_caster_id(srd, clip_ray, safe_step_count, random_shadow_3d.z);
    }

    if (!trace_result.hit) {
      continue;
    }
    if (trace_result.caster_id == SHADOW_CASTER_UNKNOWN_ID) {
      unknown_hit += 1.0f;
    }
    else if (trace_result.caster_id == self_id) {
      self_hit += 1.0f;
    }
    else {
      cast_hit += 1.0f;
    }
  }

  float inv_count = 1.0f / float(safe_ray_count);
  return float3(saturate(1.0f - (self_hit + cast_hit + unknown_hit) * inv_count),
                saturate(self_hit * inv_count),
                saturate(cast_hit * inv_count));
}

/* Preserve the Shader Info reflection-only contract exactly. Native NPR transmission uses the
 * explicit thickness variant above after the engine has amended the GBuffer thickness. */
float3 shadow_caster_classification_seeded([[resource_table]] ShadowRenderData &srd,
                                           LightData light,
                                           const bool is_directional,
                                           uint receiver_id,
                                           float3 P,
                                           float3 Ng,
                                           float3 N,
                                           float terminator_normal_offset,
                                           float terminator_geometry_offset,
                                           int ray_count,
                                           int ray_step_count,
                                           float3 random_shadow_3d,
                                           float2 random_pcf_2d)
{
  return shadow_caster_classification_seeded_ex(srd, light, is_directional, receiver_id, P, Ng, N,
                                                terminator_normal_offset, terminator_geometry_offset,
                                                ray_count, ray_step_count, random_shadow_3d,
                                                random_pcf_2d, false, Thickness::zero());
}

float3 shadow_caster_classification_seeded(LightData light,
                                           const bool is_directional,
                                           uint receiver_id,
                                           float3 P,
                                           float3 Ng,
                                           float3 N,
                                           float terminator_normal_offset,
                                           float terminator_geometry_offset,
                                           int ray_count,
                                           int ray_step_count,
                                           float3 random_shadow_3d,
                                           float2 random_pcf_2d)
{
  [[resource_table]] ShadowRenderData &srd = resource_table_get(eevee::ShadowRenderData);
  return shadow_caster_classification_seeded(srd,
                                             light,
                                             is_directional,
                                             receiver_id,
                                             P,
                                             Ng,
                                             N,
                                             terminator_normal_offset,
                                             terminator_geometry_offset,
                                             ray_count,
                                             ray_step_count,
                                             random_shadow_3d,
                                             random_pcf_2d);
}
float shadow_eval([[resource_table]] ShadowRenderData &srd,
                  LightData light,
                  const bool is_directional,
                  const bool is_transmission,
                  bool is_translucent_with_thickness,
                  float2 frag_co,
                  Thickness thickness, /* Only used if is_transmission is true. */
                  float3 P,
                  float3 Ng,
                  float3 N,
                  float terminator_normal_offset,
                  float terminator_geometry_offset,
                  int ray_count,
                  int ray_step_count)
{
  /* Case of surfel light eval. */
  float3 random_shadow_3d = float3(0.5f);
  float2 random_pcf_2d = float2(0.0f);

  if (srd.shadow_random) [[static_branch]] {
    [[resource_table]] const Sampling sampling = srd.sampling;
    [[resource_table]] const UtilityTexture util_tx = srd.util_tx;
    float3 blue_noise_3d = util_tx.fetch(frag_co, UTIL_BLUE_NOISE_LAYER).rgb;
    random_shadow_3d = fract(blue_noise_3d + sampling.rng_3D_get(SAMPLING_SHADOW_U));
    random_pcf_2d = fract(blue_noise_3d.xy + sampling.rng_2D_get(SAMPLING_SHADOW_X));
  }

  return shadow_eval_seeded(srd,
                            light,
                            is_directional,
                            is_transmission,
                            is_translucent_with_thickness,
                            thickness,
                            P,
                            Ng,
                            N,
                            terminator_normal_offset,
                            terminator_geometry_offset,
                            ray_count,
                            ray_step_count,
                            random_shadow_3d,
                            random_pcf_2d);
}

float shadow_eval(LightData light,
                  const bool is_directional,
                  const bool is_transmission,
                  bool is_translucent_with_thickness,
                  float thickness,
                  float3 P,
                  float3 Ng,
                  float3 N,
                  float terminator_normal_offset,
                  float terminator_geometry_offset,
                  int ray_count,
                  int ray_step_count)
{
  [[resource_table]] ShadowRenderData &srd = resource_table_get(eevee::ShadowRenderData);
  float2 frag_co = float2(0.0f);
#ifdef GPU_FRAGMENT_SHADER
  frag_co = gl_FragCoord.xy;
#endif
  return shadow_eval(srd,
                     light,
                     is_directional,
                     is_transmission,
                     is_translucent_with_thickness,
                     frag_co,
                     Thickness::from(thickness, ThicknessMode::Slab),
                     P,
                     Ng,
                     N,
                     terminator_normal_offset,
                     terminator_geometry_offset,
                     ray_count,
                     ray_step_count);
}

float shadow_eval_seeded(LightData light,
                         const bool is_directional,
                         const bool is_transmission,
                         bool is_translucent_with_thickness,
                         float thickness,
                         float3 P,
                         float3 Ng,
                         float3 N,
                         float terminator_normal_offset,
                         float terminator_geometry_offset,
                         int ray_count,
                         int ray_step_count,
                         float3 random_shadow_3d,
                         float2 random_pcf_2d)
{
  [[resource_table]] ShadowRenderData &srd = resource_table_get(eevee::ShadowRenderData);
  return shadow_eval_seeded(srd,
                            light,
                            is_directional,
                            is_transmission,
                            is_translucent_with_thickness,
                            Thickness::from(thickness, ThicknessMode::Slab),
                            P,
                            Ng,
                            N,
                            terminator_normal_offset,
                            terminator_geometry_offset,
                            ray_count,
                            ray_step_count,
                            random_shadow_3d,
                            random_pcf_2d);
}

float shadow_eval_stable(LightData light,
                         const bool is_directional,
                         const bool is_transmission,
                         bool is_translucent_with_thickness,
                         float thickness,
                         float3 P,
                         float3 Ng,
                         float3 N,
                         float terminator_normal_offset,
                         float terminator_geometry_offset,
                         int ray_count,
                         int ray_step_count)
{
  [[resource_table]] ShadowRenderData &srd = resource_table_get(eevee::ShadowRenderData);
  return shadow_eval_stable(srd,
                            light,
                            is_directional,
                            is_transmission,
                            is_translucent_with_thickness,
                            Thickness::from(thickness, ThicknessMode::Slab),
                            P,
                            Ng,
                            N,
                            terminator_normal_offset,
                            terminator_geometry_offset,
                            ray_count,
                            ray_step_count);
}

/** \} */

#if defined(MAT_NODE_SOFT_SHADOW) || defined(CREATE_INFO_eevee_shadow_node_cache_Surface)
/* Stable, classified single-frame PCSS for directional, point and area lights. No frame seed,
 * screen-space neighborhood reconstruction, or stochastic debanding is used here. */
struct ShadowNodePCSS {
  LightData light;
  float3 P;
  float3 Ng;
  float pixel_size;
  bool directional;
  bool classify;
  uint receiver_id;
};

float3 shadow_node_pcss_plane_point(ShadowNodePCSS ctx, float3 candidate)
{
  if (ctx.directional) {
    if (abs(ctx.Ng.z) > 1e-4f) {
      candidate.z = ctx.P.z - dot(ctx.Ng.xy, candidate.xy - ctx.P.xy) / ctx.Ng.z;
    }
    return candidate;
  }
  float denominator = dot(ctx.Ng, candidate);
  if (abs(denominator) > 1e-8f) {
    float scale = dot(ctx.Ng, ctx.P) / denominator;
    if (scale > 0.0f && scale < 4.0f) {
      return candidate * scale;
    }
  }
  return candidate;
}

ShadowCoordinates shadow_node_pcss_coordinates(ShadowNodePCSS ctx, float3 lP)
{
  if (ctx.directional) {
    return shadow_directional_coordinates(ctx.light, lP, ctx.pixel_size);
  }
  int face = shadow_punctual_face_index_get(lP);
  return shadow_punctual_coordinates(
      ctx.light, shadow_punctual_local_position_to_face_local(face, lP), face);
}

float shadow_node_pcss_reference(ShadowNodePCSS ctx, float3 lP)
{
  return ctx.directional ? -lP.z - orderedIntBitsToFloat(ctx.light.clip_near) : length(lP);
}

float4 shadow_node_pcss_compare([[resource_table]] ShadowRenderData &srd,
                                ShadowNodePCSS ctx,
                                float3 lP)
{
  ShadowCoordinates coord = shadow_node_pcss_coordinates(ctx, lP);
  float depth = srd.read_depth(coord);
  if (depth < 0.0f) {
    return float4(-1.0f);
  }
  if (depth >= shadow_node_pcss_reference(ctx, lP)) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  if (!ctx.classify) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }
  uint caster = shadow_caster_id_read(srd, coord);
  if (caster == SHADOW_CASTER_UNKNOWN_ID) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }
  return caster == ctx.receiver_id ? float4(0.0f, 1.0f, 0.0f, 0.0f) :
                                     float4(0.0f, 0.0f, 1.0f, 0.0f);
}

float4 shadow_node_pcss_bilinear([[resource_table]] ShadowRenderData &srd,
                                 ShadowNodePCSS ctx,
                                 float3 lP)
{
  ShadowCoordinates center_coord = shadow_node_pcss_coordinates(ctx, lP);
  ShadowSamplingTile tile = shadow_tile_load(
      srd.shadow_tilemaps_tx, center_coord.tilemap_tile, center_coord.tilemap_index);
  if (!tile.is_valid) {
    return float4(-1.0f);
  }
  float stride = exp2(float(tile.lod));
  float2 uv;
  float3 face_P = lP;
  int face = 0;
  float texel_world;
  if (ctx.directional) {
    LightSunData sun = ctx.light.sun();
    int level = shadow_directional_level(ctx.light, lP - ctx.light.position(), ctx.pixel_size);
    int physical_level = ctx.light.type == LIGHT_SUN_ORTHO ? sun.clipmap_lod_min : level;
    int2 offset = shadow_decompress_grid_offset(ctx.light.type,
                                                sun.clipmap_base_offset_neg,
                                                sun.clipmap_base_offset_pos,
                                                level - sun.clipmap_lod_min);
    uv = (lP.xy - sun.clipmap_origin) * exp2(float(-physical_level)) -
         float2(offset) / float(SHADOW_TILEMAP_RES) + 0.5f;
    texel_world = exp2(float(physical_level)) * stride / float(SHADOW_MAP_MAX_RES);
  }
  else {
    face = shadow_punctual_face_index_get(lP);
    face_P = shadow_punctual_local_position_to_face_local(face, lP);
    uv = face_P.xy / max(abs(face_P.z), 1e-8f) * 0.5f + 0.5f;
    texel_world = 2.0f * abs(face_P.z) * stride / float(SHADOW_MAP_MAX_RES);
  }
  float2 fraction = fract(uv * (float(SHADOW_MAP_MAX_RES) / stride) - 0.5f);
#  if !defined(GPU_METAL)
  /* Metal's emulated atomic samplers expose texelFetch only. Keep the reference path there. */
  /* Fetch four comparison depths together only when all corners provably use this
   * virtual page, physical LOD and light projection. Boundaries use the reference path. */
  int2 grid_base = int2(floor(uv * (float(SHADOW_MAP_MAX_RES) / stride) - 0.5f));
  int grid_size = int(float(SHADOW_MAP_MAX_RES) / stride);
  uint2 fine_base = uint2(max(grid_base, int2(0))) * uint(stride);
  uint2 fine_end = uint2(max(grid_base + 1, int2(0))) * uint(stride);
  bool interior = all(greaterThanEqual(grid_base, int2(0))) &&
                  all(lessThan(grid_base + 1, int2(grid_size))) &&
                  all(equal(fine_base >> SHADOW_PAGE_LOD, center_coord.tilemap_tile)) &&
                  all(equal(fine_end >> SHADOW_PAGE_LOD, center_coord.tilemap_tile));
  /* World-space corner reconstruction must have enough mantissa to agree with the
   * virtual grid. Preserve the reference path at extreme coordinate/texel ratios. */
  interior = interior && reduce_max(abs(face_P.xy)) * 1.2e-7f < texel_world * 0.125f;
  if (ctx.directional) {
    float2 extent_near = max(abs(lP.xy - ctx.light.position().xy) - texel_world, float2(0.0f));
    float2 extent_far = abs(lP.xy - ctx.light.position().xy) + texel_world;
    int current_level = center_coord.tilemap_index - ctx.light.tilemap_index +
                        ctx.light.sun().clipmap_lod_min;
    interior = interior && ctx.light.type == LIGHT_SUN &&
               shadow_directional_level(ctx.light, float3(extent_near, 0.0f), ctx.pixel_size) ==
                   current_level &&
               shadow_directional_level(ctx.light, float3(extent_far, 0.0f), ctx.pixel_size) ==
                   current_level;
  }
  uint2 virtual_texel = fine_base + uint2(tile.lod_offset << SHADOW_PAGE_LOD);
  uint2 page_texel = (virtual_texel >> tile.lod) & uint2(SHADOW_PAGE_RES - 1);
  /* Gather must stay inside the resolved physical page even for nonzero LOD offsets. */
  interior = interior && all(lessThan(page_texel, uint2(SHADOW_PAGE_RES - 1)));
  if (interior) {
    uint2 atlas_texel = (uint2(tile.page.xy) << SHADOW_PAGE_LOD) | page_texel;
    float2 atlas_extent = float2(textureSize(srd.shadow_atlas_tx, 0).xy);
    float3 gather_uv = float3((float2(atlas_texel) + 1.0f) / atlas_extent, float(tile.page.z));
    /* Gather order is (01,11,10,00), reordered to the reference loop's (00,10,01,11). */
    float4 depths = uintBitsToFloat(textureGather(srd.shadow_atlas_tx, gather_uv)).wzxy;
    uint4 casters = uint4(SHADOW_CASTER_UNKNOWN_ID);
    if (ctx.classify) {
      casters = textureGather(srd.shadow_caster_atlas_tx, gather_uv).wzxy;
    }
    float4 sample_x = face_P.x + (float4(0.0f, 1.0f, 0.0f, 1.0f) - fraction.x) * texel_world;
    float4 sample_y = face_P.y + (float4(0.0f, 0.0f, 1.0f, 1.0f) - fraction.y) * texel_world;
    float4 references;
    if (ctx.directional) {
      float4 sample_z = float4(face_P.z);
      if (abs(ctx.Ng.z) > 1e-4f) {
        sample_z = ctx.P.z -
                   (ctx.Ng.x * (sample_x - ctx.P.x) + ctx.Ng.y * (sample_y - ctx.P.y)) / ctx.Ng.z;
      }
      references = -sample_z - orderedIntBitsToFloat(ctx.light.clip_near);
    }
    else {
      float3 face_N = shadow_punctual_local_position_to_face_local(face, ctx.Ng);
      float4 denominator = face_N.x * sample_x + face_N.y * sample_y + face_N.z * face_P.z;
      float4 scale = dot(ctx.Ng, ctx.P) / denominator;
      float4 valid_scale = float4(greaterThan(abs(denominator), float4(1e-8f))) *
                           float4(greaterThan(scale, float4(0.0f))) *
                           float4(lessThan(scale, float4(4.0f)));
      scale = mix(float4(1.0f), scale, greaterThan(valid_scale, float4(0.5f)));
      references = sqrt(sample_x * sample_x + sample_y * sample_y + square(face_P.z)) * scale;
    }
    float4 occluded = float4(lessThan(depths, references));
    float4 known = ctx.classify ? float4(notEqual(casters, uint4(SHADOW_CASTER_UNKNOWN_ID))) :
                                  float4(0.0f);
    float4 self = float4(equal(casters & 0xFFFFu, uint4(ctx.receiver_id))) * known * occluded;
    float4 cast_occlusion = known * occluded - self;
    float4 weights = float4(1.0f - fraction.x, fraction.x, 1.0f - fraction.x, fraction.x) *
                     float4(1.0f - fraction.y, 1.0f - fraction.y, fraction.y, fraction.y);
    return float4(dot(1.0f - occluded, weights),
                  dot(self, weights),
                  dot(cast_occlusion, weights),
                  dot(occluded * (1.0f - known), weights));
  }
#  endif
  float4 sum = float4(0.0f);
  float weight_sum = 0.0f;
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      float2 shift = (float2(x, y) - fraction) * texel_world;
      float3 sample_P = face_P + float3(shift, 0.0f);
      if (!ctx.directional) {
        sample_P = shadow_punctual_face_local_to_local_position(face, sample_P);
      }
      sample_P = shadow_node_pcss_plane_point(ctx, sample_P);
      float4 value = shadow_node_pcss_compare(srd, ctx, sample_P);
      float2 weights = mix(1.0f - fraction, fraction, float2(x, y));
      float weight = weights.x * weights.y;
      if (value.x >= 0.0f) {
        sum += value * weight;
        weight_sum += weight;
      }
    }
  }
  return weight_sum > 1e-6f ? sum / weight_sum : float4(-1.0f);
}

/* Compare a blocker at the center of its actual stored texel. Comparing the depth of a
 * neighboring texel with the unsnapped receiver can mistake a sloped receiver for a blocker. */
float2 shadow_node_pcss_blocker([[resource_table]] ShadowRenderData &srd,
                                ShadowNodePCSS ctx,
                                float3 lP)
{
  ShadowCoordinates coord = shadow_node_pcss_coordinates(ctx, lP);
  ShadowSamplingTile tile = shadow_tile_load(
      srd.shadow_tilemaps_tx, coord.tilemap_tile, coord.tilemap_index);
  if (!tile.is_valid) {
    return float2(-1.0f);
  }
  float stride = exp2(float(tile.lod));
  float2 uv;
  float3 face_P = lP;
  float texel_world;
  int face = 0;
  if (ctx.directional) {
    LightSunData sun = ctx.light.sun();
    int level = shadow_directional_level(ctx.light, lP - ctx.light.position(), ctx.pixel_size);
    int physical_level = ctx.light.type == LIGHT_SUN_ORTHO ? sun.clipmap_lod_min : level;
    int2 offset = shadow_decompress_grid_offset(ctx.light.type,
                                                sun.clipmap_base_offset_neg,
                                                sun.clipmap_base_offset_pos,
                                                level - sun.clipmap_lod_min);
    uv = (lP.xy - sun.clipmap_origin) * exp2(float(-physical_level)) -
         float2(offset) / float(SHADOW_TILEMAP_RES) + 0.5f;
    texel_world = exp2(float(physical_level)) * stride / float(SHADOW_MAP_MAX_RES);
  }
  else {
    face = shadow_punctual_face_index_get(lP);
    face_P = shadow_punctual_local_position_to_face_local(face, lP);
    uv = face_P.xy / max(abs(face_P.z), 1e-8f) * 0.5f + 0.5f;
    texel_world = 2.0f * abs(face_P.z) * stride / float(SHADOW_MAP_MAX_RES);
  }
  float2 fraction = fract(uv * (float(SHADOW_MAP_MAX_RES) / stride));
  face_P.xy += (0.5f - fraction) * texel_world;
  float3 sample_P = ctx.directional ? face_P :
                                      shadow_punctual_face_local_to_local_position(face, face_P);
  sample_P = shadow_node_pcss_plane_point(ctx, sample_P);
  return float2(srd.read_depth(coord), shadow_node_pcss_reference(ctx, sample_P));
}

float4 shadow_node_pcss_core([[resource_table]] ShadowRenderData &srd,
                             LightData light,
                             bool directional,
                             float3 P,
                             float3 Ng,
                             float3 N,
                             float normal_offset,
                             float geometry_offset,
                             int quality,
                             bool classify,
                             uint receiver_id,
                             float radius_override,
                             bool estimate_only,
                             float2 filter_rotation,
                             int filter_multiplier)
{
  if (light.tilemap_index == LIGHT_NO_SHADOW) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  [[resource_table]] const Uniform &uni = srd.uniforms;
  [[resource_table]] const draw::View &views = srd.views;
  const ViewMatrices view = views.get(0);
  float texel_radius = shadow_texel_radius_at_position(uni, views, light, directional, P);
  float3 L = directional ? light.z_axis() :
                           normalize(light.position() + light.local().local.shadow_position - P);
  P = shadow_ray_offset(P, Ng, texel_radius) + Ng * shadow_normal_offset(Ng, L, texel_radius) +
      N * shadow_terminator_offset(N, L, normal_offset, geometry_offset);
  ShadowNodePCSS ctx;
  ctx.light = light;
  ctx.P = directional ? light_world_to_local_direction(light, P) :
                        light_world_to_local_point(light, P) - light.local().local.shadow_position;
  ctx.Ng = light_world_to_local_direction(light, Ng);
  ctx.pixel_size = shadow_screen_pixel_size(
      uni.uniform_buf.shadow.film_pixel_radius, view.is_perspective(), view.z_distance(P));
  ctx.directional = directional;
  ctx.classify = classify && bool(uni.uniform_buf.shadow.use_caster_atlas);
  ctx.receiver_id = receiver_id & 0xFFFFu;
  float4 center = shadow_node_pcss_bilinear(srd, ctx, ctx.P);
  if (center.x < 0.0f) {
    return center;
  }
  float source_radius = directional ? tan(light.sun().shadow_angle) :
                                      light.local().local.shadow_radius;
  bool area_light = !directional && is_area_light(light.type);
  float2 area_size = float2(0.0f);
  if (area_light) {
    area_size = light.area().size * light.area().shadow_scale;
    source_radius = reduce_max(area_size);
  }
  if (source_radius <= 1e-8f) {
    return estimate_only ? float4(0.0f, 1.0f, 0.0f, 0.0f) : center;
  }
  /* A cached zero means the blocker search found none, matching the direct path's early return. */
  if (!estimate_only && radius_override == 0.0f) {
    return center;
  }
  float receiver_depth = shadow_node_pcss_reference(ctx, ctx.P);
  float supported_radius = max(receiver_depth, source_radius);
  if (directional) {
    ShadowCoordinates center_coord = shadow_node_pcss_coordinates(ctx, ctx.P);
    ShadowSamplingTile center_tile = shadow_tile_load(
        srd.shadow_tilemaps_tx, center_coord.tilemap_tile, center_coord.tilemap_index);
    int level = light.type == LIGHT_SUN_ORTHO ?
                    light.sun().clipmap_lod_min :
                    shadow_directional_level(light, ctx.P - light.position(), ctx.pixel_size);
    float page_width = exp2(float(level + int(center_tile.lod))) / float(SHADOW_TILEMAP_RES);
    /* Directional maps reserve one page of padding around the visible footprint. */
    supported_radius = max(page_width * 0.75f, texel_radius);
  }
  /* Local lights have a complete coarse cubemap behind the visible fine pages. A filter's
   * physical extent must not change when the center crosses a fine/coarse page boundary. */
  float search_radius = directional ? source_radius * max(receiver_depth, 0.0f) :
                                      min(source_radius * max(receiver_depth, 0.0f) /
                                              max(intBitsToFloat(light.clip_near), 1e-4f),
                                          receiver_depth * 0.25f);
  search_radius = min(search_radius, supported_radius);
  float3 right = float3(1.0f, 0.0f, 0.0f), up = float3(0.0f, 1.0f, 0.0f);
  if (area_light) {
    right = float3(area_size.x / source_radius, 0.0f, 0.0f);
    up = float3(0.0f, area_size.y / source_radius, 0.0f);
  }
  else if (!directional) {
    make_orthonormal_basis(normalize(ctx.P), right, up);
  }
  float blocker_radius_sum = 0.0f;
  float blocker_weight_sum = 0.0f;
  int blockers = 0;
  int valid_search = 0;
  int search_count = clamp(quality * 2, 8, 32);
  float filter_radius = radius_override;
  if (radius_override < 0.0f) {
    for (int i = 0; i < 32; i++) {
      if (i >= search_count) {
        break;
      }
      /* Cover both contact-scale blockers and the outer search footprint. A uniform
       * wide disk can miss a small ring entirely between its center and first radius. */
      float2 search_sample = hammersley_2d(i, search_count);
      search_sample.x = search_sample.x * search_sample.x * search_sample.x;
      float2 disk = sample_disk(search_sample);
      float3 sample_P = shadow_node_pcss_plane_point(
          ctx, ctx.P + (right * disk.x + up * disk.y) * search_radius);
      float2 blocker = shadow_node_pcss_blocker(srd, ctx, sample_P);
      if (blocker.x < 0.0f) {
        continue;
      }
      valid_search++;
      if (blocker.x < blocker.y) {
        float separation = blocker.y - blocker.x;
        /* Each search tap has its own receiver depth on the tangent plane. Accumulating
         * center_depth - tap_depth creates false penumbra changes on oblique receivers. */
        /* Nearly coincident depths must not count as a full blocker: doing so makes tiny
         * receiver-depth errors abruptly change the normalization of the penumbra. */
        float weight = separation / (separation + 2.0f * texel_radius);
        blocker_radius_sum += weight * (directional ?
                                            source_radius * separation :
                                            source_radius * separation / max(blocker.x, 1e-6f));
        blocker_weight_sum += weight;
        blockers++;
      }
    }
    if (blockers == 0) {
      if (estimate_only) {
        return valid_search > 0 ? float4(0.0f) : float4(-1.0f);
      }
      return valid_search > 0 ? center : float4(-1.0f);
    }
    filter_radius = blocker_radius_sum / max(blocker_weight_sum, 1e-8f);
  }
  filter_radius = max(filter_radius, texel_radius * light.filter_radius);
  filter_radius = min(filter_radius, supported_radius);
  if (estimate_only) {
    return float4(filter_radius, blocker_weight_sum / float(search_count), 0.0f, 0.0f);
  }
  if (filter_radius <= 1e-8f) {
    return center;
  }
  /* No center/ring uniform-region shortcut: a thin blocker or hole can lie between
   * probes. Every selected filter position contributes to the same classified kernel. */
  int filter_count = clamp(quality * filter_multiplier, 16, 256);
  float4 total = float4(0.0f);
  float total_weight = 0.0f;
  for (int i = 0; i < 256; i++) {
    if (i >= filter_count) {
      break;
    }
    float2 filter_sample = hammersley_2d(i, filter_count);
    if (light.type == LIGHT_RECT && filter_multiplier < 8) {
      /* Translate the square sequence rather than rotating the rectangular emitter itself. */
      filter_sample = fract(filter_sample + filter_rotation * 0.5f + 0.5f);
    }
    /* Opposite Vogel pairs cover the disk without Hammersley's repeated angular spokes.
     * Pairing also keeps the filter centered with the same fixed sample budget. */
    float2 disk_sample = float2((float(i / 2) + 0.5f) / float(filter_count / 2),
                                fract(float(i / 2) * 0.61803398875f + float(i & 1) * 0.5f));
    float2 disk = light.type == LIGHT_RECT ? filter_sample * 2.0f - 1.0f :
                                             sample_disk(disk_sample);
    if (light.type != LIGHT_RECT) {
      disk = float2(disk.x * filter_rotation.x - disk.y * filter_rotation.y,
                    disk.x * filter_rotation.y + disk.y * filter_rotation.x);
    }
    float3 sample_P = shadow_node_pcss_plane_point(
        ctx, ctx.P + (right * disk.x + up * disk.y) * filter_radius);
    float4 value = shadow_node_pcss_bilinear(srd, ctx, sample_P);
    if (value.x >= 0.0f) {
      total += value;
      total_weight += 1.0f;
    }
  }
  return total_weight > 0.0f ? saturate(total / total_weight) : center;
}

float4 shadow_node_pcss(LightData light,
                        bool directional,
                        float3 P,
                        float3 Ng,
                        float3 N,
                        float normal_offset,
                        float geometry_offset,
                        int quality,
                        bool classify,
                        uint receiver_id)
{
  [[resource_table]] ShadowRenderData &srd = resource_table_get(eevee::ShadowRenderData);
  return shadow_node_pcss_core(srd,
                               light,
                               directional,
                               P,
                               Ng,
                               N,
                               normal_offset,
                               geometry_offset,
                               quality,
                               classify,
                               receiver_id,
                               -1.0f,
                               false,
                               float2(1.0f, 0.0f),
                               8);
}

#endif

}  // namespace eevee
