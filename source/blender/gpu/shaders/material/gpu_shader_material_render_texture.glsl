/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

int render_texture_slot_find(const int uid)
{
  if (uid < 0) {
    return -1;
  }

  for (int i = 0; i < RENDER_TEXTURE_SLOT_MAX; i++) {
    if ((render_texture_buf[i].info.w & RENDER_TEXTURE_SLOT_VALID) != 0 &&
        render_texture_buf[i].info.x == uid)
    {
      return i;
    }
  }

  return -1;
}

vec2 render_texture_project_uv(const mat4x4 viewproj, const vec3 world_position)
{
  vec4 projected = viewproj * vec4(world_position, 1.0);
  if (projected.w <= 0.0) {
    return vec2(-1.0);
  }
  return (projected.xy / projected.w) * 0.5 + 0.5;
}

vec4 render_texture_sample_current(const int slot, vec2 uv)
{
  switch (slot) {
    case 0:
      return texture(render_texture_color_tx_0, uv);
    case 1:
      return texture(render_texture_color_tx_1, uv);
    case 2:
      return texture(render_texture_color_tx_2, uv);
    case 3:
      return texture(render_texture_color_tx_3, uv);
    default:
      return vec4(0.0);
  }
}

vec4 render_texture_sample_history(const int slot, vec2 uv)
{
  switch (slot) {
    case 0:
      return texture(render_texture_color_history_tx_0, uv);
    case 1:
      return texture(render_texture_color_history_tx_1, uv);
    case 2:
      return texture(render_texture_color_history_tx_2, uv);
    case 3:
      return texture(render_texture_color_history_tx_3, uv);
    default:
      return vec4(0.0);
  }
}

vec4 render_texture_as_opacity(vec4 color)
{
  /* Eevee stores transmittance in the combined alpha channel. Convert it to
   * the conventional opacity that shader nodes expect. */
  color.a = saturate(1.0 - color.a);
  return color;
}

void node_render_texture_none(vec3 vector,
                              float use_explicit_vector,
                              float render_texture_uid,
                              out vec4 color,
                              out float alpha)
{
  color = vec4(0.0);
  alpha = 0.0;
}

void node_render_texture(vec3 vector,
                         float use_explicit_vector,
                         float render_texture_uid,
                         out vec4 color,
                         out float alpha)
{
  int slot = render_texture_slot_find(int(render_texture_uid));
  if (slot == -1) {
    color = vec4(0.0);
    alpha = 0.0;
    return;
  }

  vec2 uv = (use_explicit_vector > 0.5) ? vector.xy :
                                         render_texture_project_uv(render_texture_buf[slot].viewproj,
                                                                   g_data.P);

  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
    color = vec4(0.0);
    alpha = 0.0;
    return;
  }

  color = ((render_texture_buf[slot].info.w & RENDER_TEXTURE_SLOT_CAPTURING) != 0) ?
              render_texture_sample_history(slot, uv) :
              render_texture_sample_current(slot, uv);
  color = render_texture_as_opacity(color);
  alpha = color.a;
}
