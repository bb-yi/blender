/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_overlay_composite_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_overlay_composite)

float2 overlay_base_uv_get()
{
  /* Match the texture coordinate mapping used by Eevee screen/window coordinates so camera view
   * matches the final render framing. */
  return screen_uv * uniform_buf.camera.uv_scale + uniform_buf.camera.uv_bias;
}

float2 overlay_sample_uv_get(float2 base_uv)
{
  float2 safe_scale = max(overlay_scale, float2(1e-8f));
  return ((base_uv - 0.5f) / safe_scale) + 0.5f - overlay_offset;
}

bool overlay_uv_is_valid(float2 uv)
{
  return all(greaterThanEqual(uv, float2(0.0f))) && all(lessThanEqual(uv, float2(1.0f)));
}

float3 overlay_blend_rgb(float3 scene_rgb, float3 overlay_rgb)
{
  switch (overlay_blend_mode) {
    case 1:
      return scene_rgb + overlay_rgb;
    case 2:
      return scene_rgb * overlay_rgb;
    case 3:
      return 1.0f - (1.0f - scene_rgb) * (1.0f - overlay_rgb);
    case 4: {
      float3 result;
      result.x = (scene_rgb.x < 0.5f) ? (2.0f * scene_rgb.x * overlay_rgb.x) :
                                        (1.0f - 2.0f * (1.0f - scene_rgb.x) *
                                                     (1.0f - overlay_rgb.x));
      result.y = (scene_rgb.y < 0.5f) ? (2.0f * scene_rgb.y * overlay_rgb.y) :
                                        (1.0f - 2.0f * (1.0f - scene_rgb.y) *
                                                     (1.0f - overlay_rgb.y));
      result.z = (scene_rgb.z < 0.5f) ? (2.0f * scene_rgb.z * overlay_rgb.z) :
                                        (1.0f - 2.0f * (1.0f - scene_rgb.z) *
                                                     (1.0f - overlay_rgb.z));
      return result;
    }
    default:
      return overlay_rgb;
  }
}

void main()
{
  float2 overlay_uv = overlay_sample_uv_get(overlay_base_uv_get());
  float4 scene_color = texture(scene_color_tx, screen_uv);
  float4 overlay_color = overlay_uv_is_valid(overlay_uv) ? texture(overlay_color_tx, overlay_uv) :
                                                           float4(0.0f);

  float overlay_alpha = clamp(overlay_color.a * overlay_opacity, 0.0f, 1.0f);
  float3 overlay_rgb = overlay_color.rgb;
  float3 overlay_premul = overlay_color.rgb * overlay_opacity;

  if (overlay_alpha_mode == 0) {
    overlay_premul = overlay_rgb * overlay_alpha;
  }
  else {
    overlay_rgb = (overlay_alpha > 1e-8f) ? (overlay_premul / overlay_alpha) : float3(0.0f);
  }

  if (overlay_blend_mode == 0) {
    out_color = float4(overlay_premul, overlay_alpha) + scene_color * (1.0f - overlay_alpha);
    return;
  }

  float scene_alpha = clamp(scene_color.a, 0.0f, 1.0f);
  float3 scene_rgb = (scene_alpha > 1e-8f) ? (scene_color.rgb / scene_alpha) : scene_color.rgb;
  float3 blended_rgb = overlay_blend_rgb(scene_rgb, overlay_rgb);
  float3 mixed_rgb = mix(scene_rgb, blended_rgb, overlay_alpha);
  float out_alpha = overlay_alpha + scene_alpha * (1.0f - overlay_alpha);
  out_color = float4(mixed_rgb * out_alpha, out_alpha);
}
