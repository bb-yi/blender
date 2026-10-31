/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_stencil_value(float3 uv, float use_uv, out float value, out float depth, out float valid)
{
  value = 0.0f;
  depth = 0.0f;
  valid = 0.0f;
#if defined(MAT_STENCIL_VALUE) && defined(GPU_FRAGMENT_SHADER)
  float2 pixel = gl_FragCoord.xy;
  if (use_uv != 0.0f) {
    /* Inverse of Render Info, including camera crop/overscan. Film and texture extents may differ. */
    pixel = (uv.xy - uniform_buf.camera.uv_bias) / uniform_buf.camera.uv_scale *
            max(float2(uniform_buf.film.extent), float2(1.0f));
  }
  int2 extent = textureSize(stencil_value_tx, 0);
  /* Also rejects NaN/Inf before conversion to integer coordinates. */
  if (all(greaterThanEqual(pixel, float2(0.0f))) && all(lessThan(pixel, float2(extent)))) {
    value = float(texelFetch(stencil_value_tx, int2(pixel), 0).r & 15u);
  }
#endif
}

[[node]]
void node_stencil_surface(
    float3 uv, float use_uv, out float value, out float depth, out float valid)
{
  value = 0.0f;
  depth = 0.0f;
  valid = 0.0f;
#if defined(MAT_STENCIL_SURFACE) && defined(GPU_FRAGMENT_SHADER)
  /* Convert the same full-resolution pixel coordinates as Stencil Value, then scale to the
   * auxiliary buffer. Film extent, render-buffer extent and the capture extent can differ. */
  float2 pixel = gl_FragCoord.xy;
  if (use_uv != 0.0f) {
    pixel = (uv.xy - uniform_buf.camera.uv_bias) / uniform_buf.camera.uv_scale *
            max(float2(uniform_buf.film.extent), float2(1.0f));
  }
  int2 extent = textureSize(stencil_surface_depth_tx, 0);
  float2 source_extent = max(float2(stencil_surface_extent), float2(1.0f));
  pixel *= float2(extent) / source_extent;
  if (all(greaterThanEqual(pixel, float2(0.0f))) && all(lessThan(pixel, float2(extent)))) {
    int2 texel = int2(pixel);
    float hardware_depth = texelFetch(stencil_surface_depth_tx, texel, 0).r;
    /* Own depth is cleared to reverse-Z far. Reference zero is a valid hit. */
    if (hardware_depth > 0.0f) {
      depth = -drw_depth_screen_to_view(1.0f - hardware_depth);
      value = float(texelFetch(stencil_surface_value_tx, texel, 0).r & 15u);
      valid = float(depth > 0.0f);
    }
  }
#endif
}
