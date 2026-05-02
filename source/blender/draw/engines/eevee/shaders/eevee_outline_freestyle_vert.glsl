/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_outline_infos.hh"

VERTEX_SHADER_CREATE_INFO(eevee_outline_freestyle)

#include "draw_model_lib.glsl"
#include "draw_view_lib.glsl"
#include "eevee_outline_lib.glsl"
#include "eevee_reverse_z_lib.glsl"

void main()
{
  DRW_VIEW_FROM_RESOURCE_ID;
  float3 world_pos = drw_point_object_to_world(pos);
  gl_Position = reverse_z::transform(drw_point_world_to_homogenous(world_pos));
}
