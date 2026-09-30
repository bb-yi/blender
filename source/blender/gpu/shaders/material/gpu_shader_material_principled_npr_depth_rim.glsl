/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef GPU_SHADER_MATERIAL_PRINCIPLED_NPR_DEPTH_RIM_GLSL
#define GPU_SHADER_MATERIAL_PRINCIPLED_NPR_DEPTH_RIM_GLSL

/* Independent rotating depth taps. Threshold is a minimum view-space gap,
 * never a normalization gain. Foreground taps cannot cancel other directions. */
float npr_v2_depth_rim(float width_px,
                       float depth_threshold,
                       float softness,
                       float angle_degrees,
                       float arc_length,
                       float arc_softness,
                       float samples)
{
#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_HIZ_DATA) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER)) && \
    !defined(MAT_PROBE_CAPTURE) && !defined(MAT_CAPTURE) && \
    !defined(MAT_BAKE_COLOR)
  /* Hardware depth is affine across a projected triangle, including perspective.
   * Use the receiver's slope, not depth derivatives from neighboring objects. */
  /* EEVEE raster depth is reverse-Z, while HiZ is already converted back. */
  float receiver_depth = 1.0f - gl_FragCoord.z;
  float2 depth_gradient = float2(dFdx(receiver_depth), dFdy(receiver_depth));
  if (!(width_px > 0.0f) || !(arc_length > 0.0f)) {
    return 0.0f;
  }
  int2 depth_extent = textureSize(hiz_tx, 0);
  int2 view_extent = int2(uniform_buf.film.render_extent);
  int2 center_pixel = int2(gl_FragCoord.xy);
  if (any(lessThan(center_pixel, int2(0))) ||
      any(greaterThanEqual(center_pixel, view_extent)) ||
      any(greaterThan(view_extent, depth_extent)))
  {
    return 0.0f;
  }
  float center_depth = texelFetch(hiz_tx, center_pixel, 0).r;
  if (!(center_depth >= 0.0f && center_depth < 1.0f)) {
    return 0.0f;
  }

  int n_samples = clamp(int(samples), 1, 64);
  float half_arc = saturate(arc_length) * 0.5f;
  float arc_transition = min(saturate(arc_softness) * 0.5f, half_arc);
  float angle_turns = angle_degrees / 360.0f;
  float rotation = sampling_rng_1D_get(SAMPLING_TRANSPARENCY);
  float rim = 0.0f;
  for (int axis = 0; axis < 8; axis++) {
    float turns = (float(axis) + rotation) * 0.125f;
    float arc_weight = 1.0f;
    if (arc_length < 1.0f) {
      float distance = abs(fract(turns - angle_turns + 0.5f) - 0.5f);
      arc_weight = arc_transition > 0.0f ?
                       1.0f - smoothstep(half_arc - arc_transition, half_arc, distance) :
                       1.0f - step(half_arc, distance);
    }
    if (arc_weight <= 0.0f) {
      continue;
    }
    float radians = turns * (2.0f * M_PI);
    float2 direction = float2(cos(radians), sin(radians));
    int2 tap_step = int2(round(direction * max(1.0f, width_px / float(n_samples))));
    for (int i = 1; i <= n_samples; i++) {
      float reach = float(i) / float(n_samples);
      int2 pixel = int2(floor(gl_FragCoord.xy + direction * (width_px * reach)));
      /* Off-screen and padded HiZ texels are not background geometry. */
      if (any(lessThan(pixel, int2(0))) || any(greaterThanEqual(pixel, view_extent))) {
        continue;
      }
      float sample_depth = texelFetch(hiz_tx, pixel, 0).r;
      bool background = sample_depth >= 1.0f;
      float predicted_depth = center_depth + dot(depth_gradient, float2(pixel - center_pixel));
      bool edge = background;
      if (!background && predicted_depth >= 0.0f && predicted_depth < 1.0f) {
        float predicted_z = drw_depth_screen_to_view(predicted_depth);
        float sample_z = drw_depth_screen_to_view(sample_depth);
        float tolerance = max(1e-5f, abs(predicted_z) * 1e-5f);
        float gap = predicted_z - sample_z;
        /* A tangent prediction over the whole radius also responds to smooth
         * curvature. Require a local break in the depth slope at the tap,
         * using two equally spaced preceding texels along this direction. */
        int2 prior = pixel - tap_step;
        int2 prior2 = prior - tap_step;
        if (all(greaterThanEqual(prior2, int2(0))) &&
            all(lessThan(prior2, view_extent)) &&
            all(greaterThanEqual(prior, int2(0))) && all(lessThan(prior, view_extent)))
        {
          float d1 = texelFetch(hiz_tx, prior, 0).r;
          float d2 = texelFetch(hiz_tx, prior2, 0).r;
          float local_prediction = 2.0f * d1 - d2;
          if (d1 < 1.0f && d2 < 1.0f && local_prediction >= 0.0f &&
              local_prediction < 1.0f)
          {
            gap = min(gap, drw_depth_screen_to_view(local_prediction) - sample_z);
            /* A smooth curved surface changes slope gradually. A finite
             * discontinuity must exceed that local depth trend as well. */
            float trend = abs(drw_depth_screen_to_view(d1) - drw_depth_screen_to_view(d2));
            if (gap <= 2.0f * trend) {
              gap = 0.0f;
            }
          }
          else {
            gap = 0.0f;
          }
        }
        else {
          gap = 0.0f;
        }
        edge = gap > max(depth_threshold, 0.0f) + tolerance;
      }
      if (edge) {
        /* Softness tapers the spatial band. Raising the depth threshold can
         * only remove candidates, never introduce a different bright edge. */
        /* Estimate the crossing halfway through the tap interval, so Samples=1
         * still has a nonzero soft response instead of only sampling its zero. */
        float crossing = max(0.0f, reach - 0.5f / float(n_samples));
        float band = softness > 0.0f ?
                         1.0f - smoothstep(1.0f - saturate(softness), 1.0f, crossing) :
                         1.0f;
        rim = max(rim, arc_weight * band);
      }
    }
  }
  return rim;
#else
  return 0.0f;
#endif
}

#endif
