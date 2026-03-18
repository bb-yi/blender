/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_filter_material_info.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_geom_world)
FRAGMENT_SHADER_CREATE_INFO(eevee_filter_material)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.glsl"
#include "eevee_nodetree_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

#define TEX_HANDLE_NULL 0u
#define TEX_HANDLE_RP_COLOR 1u
#define TEX_HANDLE_RP_VALUE 2u

vec4 closure_to_rgba(Closure cl)
{
  UNUSED_VARS(cl);
  return vec4(0.0);
}

vec4 nodetree_filter();

void input_aov_impl(uint hash, out TextureHandle color, out TextureHandle value)
{
  int color_index = aov_color_index(hash);
  int value_index = aov_value_index(hash);

  color = (color_index >= 0) ? TextureHandle(TEX_HANDLE_RP_COLOR,
                                             uint(uniform_buf.render_pass.color_len + color_index)) :
                               TEXTURE_HANDLE_DEFAULT;
  value = (value_index >= 0) ? TextureHandle(TEX_HANDLE_RP_VALUE,
                                             uint(uniform_buf.render_pass.value_len + value_index)) :
                               TEXTURE_HANDLE_DEFAULT;
}

vec4 TextureHandle_eval(TextureHandle tex, vec2 offset, bool texel_offset)
{
  if (tex.type == TEX_HANDLE_NULL) {
    return vec4(0.0);
  }

  ivec2 extent = textureSize(scene_color_tx, 0);
  ivec2 texel = ivec2(gl_FragCoord.xy);
  if (texel_offset) {
    texel += ivec2(offset);
  }
  else {
    vec2 uv = clamp((gl_FragCoord.xy / vec2(extent)) + offset, vec2(0.0), vec2(1.0));
    texel = ivec2(uv * vec2(extent));
  }

  texel = clamp(texel, ivec2(0), extent - ivec2(1));

  switch (tex.type) {
    case TEX_HANDLE_RP_COLOR:
      return texelFetch(rp_color_tx, ivec3(texel, int(tex.index)), 0);
    case TEX_HANDLE_RP_VALUE:
      return vec4(texelFetch(rp_value_tx, ivec3(texel, int(tex.index)), 0).rrr, 1.0);
    default:
      return vec4(0.0);
  }
}

vec4 TextureHandle_eval(TextureHandle tex)
{
  return TextureHandle_eval(tex, vec2(0.0), true);
}

void main()
{
  init_globals();
  /* Reuse the world-style fullscreen setup so regular shader node helpers still work. */
  g_data.N = drw_normal_view_to_world(drw_view_incident_vector(interp.P));
  g_data.Ng = g_data.N;
  g_data.P = -g_data.N;
  attrib_load();

  vec4 filter_result = nodetree_filter();
  out_color = vec4(filter_result.rgb, saturate(1.0 - filter_result.a));
}
