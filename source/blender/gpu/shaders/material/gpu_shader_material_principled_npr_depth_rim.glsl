/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef GPU_SHADER_MATERIAL_PRINCIPLED_NPR_DEPTH_RIM_GLSL
#define GPU_SHADER_MATERIAL_PRINCIPLED_NPR_DEPTH_RIM_GLSL

/* The caller disables this effect for Blended materials and partial alpha. In particular,
 * MAT_TRANSPARENT is not a Blended flag: binary Dithered materials also define it.
 * Probe captures must not sample depth from a different surface or view.
 * Codegen defines MAT_HIZ_DATA/MAT_RAYCAST when depth and object IDs are available. */
#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_HIZ_DATA) && defined(MAT_RAYCAST) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER)) && \
    !defined(MAT_PROBE_CAPTURE) && !defined(MAT_CAPTURE) && \
    !defined(MAT_BAKE_COLOR)

bool npr_v2_depth_rim_sample(float2 pixel,
                           float2 view_extent,
                           float3 plane_depth,
                           float center_z,
                           float depth_tolerance,
                           float depth_threshold,
                           uint center_id)
{
  if (any(lessThan(pixel, float2(0.0f))) || any(greaterThanEqual(pixel, view_extent))) {
    return false;
  }
  int2 texel = int2(pixel);
  float screen_depth = texelFetch(hiz_tx, texel, 0).r;
  if (!(screen_depth >= 0.0f && screen_depth <= 1.0f)) {
    return false;
  }
  if (screen_depth == 1.0f) {
    return true;
  }
  float sample_z = drw_depth_screen_to_view(screen_depth);
  /* View Z is negative in front of the camera. A nearer occluder is not a rim on
   * the hidden surface, even when the current tangent plane slopes towards it. */
  if (sample_z >= center_z - depth_tolerance) {
    return false;
  }
  /* This mode follows visible object silhouettes, not Goo-style curvature.
   * Even a large tangent-plane residual on the same object is not an edge:
   * accepting it makes smooth spheres develop wide rings inside their outline.
   * Background was handled independently before reading its possibly stale ID. */
  uint sample_id = texelFetch(object_id_tx, texel, 0).r;
  if (sample_id == center_id) {
    return false;
  }

  /* Compare at the sampled texel center, not the continuous search position.
   * Screen depth is affine over a plane, including perspective projection. */
  float2 screen_uv = (float2(texel) + 0.5f) / view_extent;
  float expected_depth = dot(plane_depth.xy, screen_uv) + plane_depth.z;
  float expected_z = center_z;
  if (expected_depth > 0.0f && expected_depth < 1.0f) {
    expected_z = drw_depth_screen_to_view(expected_depth);
  }
  /* Near a tangent-plane horizon the extrapolated point is behind the camera
   * or outside its clip range. Fall back to the visible center depth, not NaN. */
  /* The external surface can lie in front of the extrapolated plane while still
   * behind the current visible point. The actual-depth check above already
   * rejects foreground, so either sign of the non-coplanar residual is valid. */
  return abs(expected_z - sample_z) > max(depth_threshold, depth_tolerance);
}

#endif

float npr_v2_depth_rim(float width_px,
                       float softness,
                       float depth_threshold,
                       float angle_degrees,
                       float arc_length,
                       float arc_softness)
{
#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_HIZ_DATA) && defined(MAT_RAYCAST) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER)) && \
    !defined(MAT_PROBE_CAPTURE) && !defined(MAT_CAPTURE) && \
    !defined(MAT_BAKE_COLOR)
  if (!(width_px > 0.0f) || !(arc_length > 0.0f)) {
    return 0.0f;
  }
  float2 view_extent = float2(uniform_buf.film.render_extent);
  int2 depth_extent = textureSize(hiz_tx, 0);
  int2 object_extent = textureSize(object_id_tx, 0);
  if (any(lessThan(view_extent, float2(1.0f))) ||
      any(greaterThan(view_extent, float2(depth_extent))) ||
      any(greaterThan(view_extent, float2(object_extent))))
  {
    return 0.0f;
  }
  float2 center_pixel = gl_FragCoord.xy;
  if (any(lessThan(center_pixel, float2(0.0f))) ||
      any(greaterThanEqual(center_pixel, view_extent)))
  {
    return 0.0f;
  }
  float center_depth = texelFetch(hiz_tx, int2(center_pixel), 0).r;
  if (!(center_depth >= 0.0f && center_depth < 1.0f)) {
    return 0.0f;
  }
  uint center_id = texelFetch(object_id_tx, int2(center_pixel), 0).r;
  float center_z = drw_depth_screen_to_view(center_depth);
  float3 view_position = drw_point_world_to_view(g_data.P);
  float depth_tolerance = max(abs(center_z) * 1e-4f, 1e-5f);
  if (abs(view_position.z - center_z) > depth_tolerance) {
    return 0.0f;
  }

  float3 view_normal = to_float3x3(drw_view().viewmat) * g_data.Ng;
  float4 view_plane = float4(view_normal, -dot(view_normal, view_position));
  float4 clip_plane = transpose(drw_view().wininv) * view_plane;
  float2 slope = float2(0.0f);
  if (abs(clip_plane.z) > 1e-20f) {
    slope = -clip_plane.xy / clip_plane.z;
  }
  float2 center_uv = (float2(int2(center_pixel)) + 0.5f) / view_extent;
  float3 plane_depth = float3(slope, center_depth - dot(slope, center_uv));

  /* Width is the half-coverage boundary in actual render pixels. The one-pixel
   * minimum transition is coverage AA, not a change to the artistic softness.
   * A screen-space effect cannot search further than the image diagonal. */
  float width = min(width_px, length(view_extent));
  float transition = max(width * saturate(softness), 1.0f);
  float search_radius = width + 0.5f * transition;
  bool full_arc = arc_length >= 1.0f;
  float nearest_distance = search_radius;
  float nearest_turns = 0.0f;
  bool found_edge = false;

  /* Sixteen directions bound straight-edge width variation to about 1.9%.
   * Keep this loop bounded and do not unroll the depth-search body. Unlike the
   * Curvature node, no unrelated curvature response is evaluated here. */
  for (int direction_index = 0; direction_index < 16; direction_index++) {
    /* Rotate the whole circle, but do not discard directions outside the arc:
     * a diagonal ray in the right arc can hit a nearer top edge. Angular gating
     * must describe the nearest visible contour, not any ray that crosses it. */
    float turns = float(direction_index) * (1.0f / 16.0f) +
                  (full_arc ? 0.0f : angle_degrees / 360.0f);
    float angle = turns * (2.0f * M_PI);
    float2 direction = float2(cos(angle), sin(angle));

    /* Clip the search to real pixels. Reading HiZ padding or treating a screen
     * border as background would draw a bright frame around cropped objects. */
    float far_distance = search_radius;
    for (int axis = 0; axis < 2; axis++) {
      if (direction[axis] > 1e-6f) {
        far_distance = min(far_distance,
                           (view_extent[axis] - 0.5f - center_pixel[axis]) / direction[axis]);
      }
      else if (direction[axis] < -1e-6f) {
        far_distance = min(far_distance, (0.5f - center_pixel[axis]) / direction[axis]);
      }
    }
    if (far_distance <= 0.0f ||
        !npr_v2_depth_rim_sample(center_pixel + direction * far_distance,
                                 view_extent, plane_depth, center_z,
                                 depth_tolerance, depth_threshold, center_id))
    {
      continue;
    }
    float near_distance = 0.0f;
    for (int search_index = 0; search_index < 4; search_index++) {
      float middle = 0.5f * (near_distance + far_distance);
      if (npr_v2_depth_rim_sample(center_pixel + direction * middle,
                                  view_extent, plane_depth, center_z,
                                  depth_tolerance, depth_threshold, center_id))
      {
        far_distance = middle;
      }
      else {
        near_distance = middle;
      }
    }
    float distance = 0.5f * (near_distance + far_distance);
    if (!found_edge || distance < nearest_distance) {
      nearest_distance = distance;
      nearest_turns = turns;
      found_edge = true;
    }
    if (full_arc && distance <= width - 0.5f * transition) {
      return 1.0f;
    }
  }
  if (!found_edge) {
    return 0.0f;
  }
  float arc = 1.0f;
  if (!full_arc) {
    float half_arc = saturate(arc_length) * 0.5f;
    float arc_transition = min(saturate(arc_softness) * 0.5f, half_arc);
    float centered = abs(fract(nearest_turns - angle_degrees / 360.0f + 0.5f) - 0.5f);
    arc = arc_transition > 0.0f ?
              1.0f - smoothstep(half_arc - arc_transition, half_arc, centered) :
              1.0f - step(half_arc, centered);
  }
  float coverage = 1.0f - smoothstep(width - 0.5f * transition,
                                    width + 0.5f * transition, nearest_distance);
  return saturate(arc * coverage);
#else
  return 0.0f;
#endif
}

#endif
