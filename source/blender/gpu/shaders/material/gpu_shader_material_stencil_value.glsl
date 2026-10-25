/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_stencil_value(float3 uv, float use_uv, out float value)
{
  value = 0.0f;
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
