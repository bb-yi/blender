/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float2 scene_color_resolve_uv(KernelGlobals &kg, ShadingData &sd, float3 vector, float use_explicit_vector)
{
  float2 uv = float2(0.0f);
#ifdef GPU_FRAGMENT_SHADER
  if (use_explicit_vector > 0.5f) {
    uv = (vector.xy - eevee_uniform_data_get().camera.uv_bias) / eevee_uniform_data_get().camera.uv_scale;
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

float scene_depth_resolve_linear(KernelGlobals &kg, ShadingData &sd, float2 uv)
{
  /* Eevee stores the hardware depth buffer in reverse-Z space. */
  float depth = 1.0f - texture(depth_tx, uv).r;
  return -kg.view_matrices_get(sd).depth_screen_to_view(depth);
}

float4 scene_normal_resolve(KernelGlobals &kg, ShadingData &sd, float2 uv)
{
  if (eevee_uniform_data_get().render_pass.normal_id < 0) {
    return float4(0.0f);
  }
  return texture(rp_color_tx, float3(uv, float(eevee_uniform_data_get().render_pass.normal_id)));
}

float4 scene_position_resolve(KernelGlobals &kg, ShadingData &sd, float2 uv)
{
  if (eevee_uniform_data_get().render_pass.position_id < 0) {
    return float4(0.0f);
  }
  return texture(rp_color_tx, float3(uv, float(eevee_uniform_data_get().render_pass.position_id)));
}

[[node]]
void node_scene_color(float3 vector,
                      float use_explicit_vector,
                      float source,[[resource_table]] KernelGlobals &kg, ShadingData &sd,
                      out float4 color,
                      out float alpha,
                      TextureHandle &color_image,
                      TextureHandle &depth_image,
                      TextureHandle &normal_image,
                      TextureHandle &position_image)
{
  float2 uv = scene_color_resolve_uv(kg, sd, vector, use_explicit_vector);
  if (source < 0.5f) {
    color = texture(scene_color_tx, uv);
    /* Scene color is stored with Eevee transmittance in alpha until film accumulation. */
    alpha = saturate(1.0f - color.a);
  }
  else if (source < 1.5f) {
    float depth = scene_depth_resolve_linear(kg, sd, uv);
    color = float4(depth, depth, depth, 1.0f);
    alpha = depth;
  }
  else if (source < 2.5f) {
    color = scene_normal_resolve(kg, sd, uv);
    alpha = (eevee_uniform_data_get().render_pass.normal_id >= 0) ? 1.0f : 0.0f;
  }
  else if (source > 3.5f) {
    color = scene_position_resolve(kg, sd, uv);
    alpha = (eevee_uniform_data_get().render_pass.position_id >= 0) ? 1.0f : 0.0f;
  }
  else {
    color = float4(0.0f);
    alpha = 0.0f;
  }

  color_image = TextureHandle(TEX_HANDLE_SCENE, 0);
  depth_image = TextureHandle(TEX_HANDLE_SCENE, 1);
  normal_image = TextureHandle(TEX_HANDLE_SCENE, 2);
  position_image = TextureHandle(TEX_HANDLE_SCENE, 4);
}

[[node]]
void node_output_filter(float4 color, float alpha, out float4 filter_result)
{
  filter_result = float4(color.rgb, clamp(alpha, 0.0f, 1.0f));
}
