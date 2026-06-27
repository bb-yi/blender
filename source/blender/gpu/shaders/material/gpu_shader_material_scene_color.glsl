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

float4 scene_position_resolve(float2 uv)
{
  if (uniform_buf.render_pass.position_id < 0) {
    return float4(0.0f);
  }
  return texture(rp_color_tx, float3(uv, float(uniform_buf.render_pass.position_id)));
}

[[node]]
void node_scene_color_handle_only(TextureHandle &color_image,
                                  TextureHandle &depth_image,
                                  TextureHandle &normal_image,
                                  TextureHandle &position_image)
{
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
