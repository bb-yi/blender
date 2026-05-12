/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma BLENDER_REQUIRE(gpu_shader_material_implicit_defaults.glsl)

[[node]]
void node_output_eevee_light_shader(float4 color,
                                    float intensity,
                                    float attenuation,
                                    out float4 result)
{
  result = float4(color.rgb * max(intensity, 0.0f), max(attenuation, 0.0f));
}
