/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Combine light passes to the combined color target and apply surface colors.
 * This also fills the different render passes.
 */

#include "infos/eevee_deferred_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_deferred_combine)

#include "draw_view_lib.glsl"
#include "eevee_deferred_combine_lib.glsl"
#include "eevee_renderpass_lib.glsl"

void main()
{
  int2 texel = int2(gl_FragCoord.xy);

  DeferredCombine dc = deferred_combine(texel);

  if (use_radiance_feedback) {
    /* Output unmodified radiance for indirect lighting. */
    float3 out_radiance = imageLoad(radiance_feedback_img, texel).rgb;
    out_radiance += dc.out_direct + dc.out_indirect;
    imageStore(radiance_feedback_img, texel, float4(out_radiance, 0.0f));
  }

  deferred_combine_clamp(dc);

  /* Light passes. */
  if (render_pass_diffuse_light_enabled) {
    float3 diffuse_light = dc.diffuse_direct + dc.diffuse_indirect;
    output_renderpass_color(
        uniform_buf.render_pass.diffuse_color_id, float4(dc.diffuse_color, 1.0f));
    output_renderpass_color(uniform_buf.render_pass.diffuse_light_id, float4(diffuse_light, 1.0f));
  }
  if (render_pass_specular_light_enabled) {
    float3 specular_light = dc.specular_direct + dc.specular_indirect;
    output_renderpass_color(uniform_buf.render_pass.specular_color_id,
                            float4(dc.specular_color, 1.0f));
    output_renderpass_color(uniform_buf.render_pass.specular_light_id,
                            float4(specular_light, 1.0f));
  }
  if (render_pass_normal_enabled) {
    output_renderpass_color(uniform_buf.render_pass.normal_id, float4(dc.average_normal, 1.0f));
  }
  if (render_pass_position_enabled) {
    float depth = texelFetch(hiz_tx, texel, 0).r;
    float3 P = drw_point_screen_to_world(float3(screen_uv, depth));
    output_renderpass_color(uniform_buf.render_pass.position_id, float4(P, 1.0f));
  }

  out_combined = deferred_combine_final_output(dc);
}
