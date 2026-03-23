/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float2 scene_color_resolve_uv(float3 vector, float use_explicit_vector)
{
  float2 uv = float2(0.0f);
#ifdef GPU_FRAGMENT_SHADER
  if (use_explicit_vector > 0.5f) {
    uv = (vector.xy - uniform_buf.camera.uv_bias) / uniform_buf.camera.uv_scale;
  }
  else {
    uv = gl_FragCoord.xy / float2(textureSize(scene_color_tx, 0));
  }
#else
  if (use_explicit_vector > 0.5f) {
    uv = vector.xy;
  }
#endif
  return clamp(uv, float2(0.0f), float2(1.0f));
}

float scene_depth_resolve_linear(float2 uv)
{
  /* Eevee stores the hardware depth buffer in reverse-Z space. */
  float depth = 1.0f - texture(depth_tx, uv).r;
  return -drw_depth_screen_to_view(depth);
}

float4 scene_normal_resolve(float2 uv)
{
  if (uniform_buf.render_pass.normal_id < 0) {
    return float4(0.0f);
  }
  return texture(rp_color_tx, float3(uv, float(uniform_buf.render_pass.normal_id)));
}

float4 scene_shadow_resolve(float2 uv)
{
  if (uniform_buf.render_pass.shadow_id < 0) {
    return float4(1.0f);
  }
#ifdef MAT_FILTER
  float visibility = texture(scene_shadow_tx, uv).r;
#else
  float visibility = texture(rp_value_tx, float3(uv, float(uniform_buf.render_pass.shadow_id))).r;
#endif
  return float4(visibility, visibility, visibility, 1.0f);
}

[[node]]
void node_scene_color(float3 vector,
                      float use_explicit_vector,
                      float source,
                      out float4 color,
                      out float alpha)
{
  float2 uv = scene_color_resolve_uv(vector, use_explicit_vector);
  if (source < 0.5f) {
    color = texture(scene_color_tx, uv);
    /* Scene color is stored with Eevee transmittance in alpha until film accumulation. */
    alpha = saturate(1.0f - color.a);
  }
  else if (source < 1.5f) {
    float depth = scene_depth_resolve_linear(uv);
    color = float4(depth, depth, depth, 1.0f);
    alpha = depth;
  }
  else if (source < 2.5f) {
    color = scene_normal_resolve(uv);
    alpha = (uniform_buf.render_pass.normal_id >= 0) ? 1.0f : 0.0f;
  }
  else {
    color = scene_shadow_resolve(uv);
    alpha = 1.0f;
  }
}

[[node]]
void node_output_filter(float4 color, float alpha, out float4 filter_result)
{
  filter_result = float4(color.rgb, clamp(alpha, 0.0f, 1.0f));
}
