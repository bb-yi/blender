/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_geom_infos.hh"

VERTEX_SHADER_CREATE_INFO(eevee_geom_bake_mesh_basic)

#include "draw_model_lib.glsl"

void main()
{
  DRW_VIEW_FROM_RESOURCE_ID;

  interp.P = drw_point_object_to_world(pos);
  interp.N = normalize(drw_normal_object_to_world(nor));

  gl_Position = float4(bake_uv * 2.0f - 1.0f, 0.0f, 1.0f);
}
