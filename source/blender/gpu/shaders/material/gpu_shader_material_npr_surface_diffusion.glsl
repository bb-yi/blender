/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void npr_surface_diffusion_empty(Closure &result)
{
  result = Closure(0);
}

[[node]]
void npr_surface_diffusion_capture(Closure input_shader, float &result)
{
  result = 0.0f;
}

[[node]]
void node_npr_surface_color(float4 color, float strength, float weight, Closure &result)
{
#if defined(MAT_CAPTURE) || defined(MAT_BAKE_COLOR)
  if (g_npr_diffusion_scope) {
    result = Closure(0);
    return;
  }
#endif
  ClosureEmission unlit;
  unlit.emission = max(color.rgb, float3(0.0f)) * max(strength, 0.0f);
  unlit.weight = weight;
  result = closure_eval(unlit);
}
