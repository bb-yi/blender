/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma BLENDER_REQUIRE(gpu_shader_material_implicit_defaults.glsl)

[[node]]
void node_output_eevee_light_shader(float4 color, float attenuation, out float4 result)
{
  result = float4(color.rgb, max(attenuation, 0.0f));
}
