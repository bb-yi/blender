/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

vec2 scene_color_resolve_uv(vec3 vector, float use_explicit_vector)
{
  vec2 uv = vec2(0.0);
#ifdef GPU_FRAGMENT_SHADER
  uv = (use_explicit_vector > 0.5) ? vector.xy :
                                     (gl_FragCoord.xy / vec2(textureSize(scene_color_tx, 0)));
#else
  if (use_explicit_vector > 0.5) {
    uv = vector.xy;
  }
#endif
  return clamp(uv, vec2(0.0), vec2(1.0));
}

float scene_depth_resolve_linear(vec2 uv)
{
  float depth = texture(depth_tx, uv).r;
  return -drw_depth_screen_to_view(depth);
}

vec4 scene_normal_resolve(vec2 uv)
{
  if (uniform_buf.render_pass.normal_id < 0) {
    return vec4(0.0);
  }
  return texture(rp_color_tx, vec3(uv, float(uniform_buf.render_pass.normal_id)));
}

void node_scene_color(vec3 vector,
                      float use_explicit_vector,
                      float source,
                      out vec4 color,
                      out float alpha)
{
  vec2 uv = scene_color_resolve_uv(vector, use_explicit_vector);
  if (source < 0.5) {
    color = texture(scene_color_tx, uv);
    /* Scene color is stored with Eevee transmittance in alpha until film accumulation. */
    alpha = saturate(1.0 - color.a);
  }
  else if (source < 1.5) {
    float depth = scene_depth_resolve_linear(uv);
    color = vec4(depth, depth, depth, 1.0);
    alpha = depth;
  }
  else {
    color = scene_normal_resolve(uv);
    alpha = (uniform_buf.render_pass.normal_id >= 0) ? 1.0 : 0.0;
  }
}

void node_output_filter(vec4 color, float alpha, out vec4 filter_result)
{
  filter_result = vec4(color.rgb, clamp(alpha, 0.0, 1.0));
}
