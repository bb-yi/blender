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

void node_scene_color(vec3 vector,
                      float use_explicit_vector,
                      out vec4 color,
                      out float alpha)
{
  vec2 uv = scene_color_resolve_uv(vector, use_explicit_vector);
  color = texture(scene_color_tx, uv);
  alpha = color.a;
}

void node_output_filter(vec4 color, float opacity, out vec4 filter_result)
{
  filter_result = vec4(color.rgb, clamp(opacity, 0.0, 1.0));
}
