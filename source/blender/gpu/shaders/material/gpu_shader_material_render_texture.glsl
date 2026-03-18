/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_render_texture_none(float3 vector,
                              float use_explicit_vector,
                              float render_texture_uid,
                              float4 &color,
                              float &alpha)
{
  color = float4(0.0);
  alpha = 0.0;
}

[[node]]
void node_render_texture(float3 vector,
                         float use_explicit_vector,
                         float render_texture_uid,
                         float4 &color,
                         float &alpha)
{
  color = float4(0.0);
  alpha = 0.0;
}
