/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

VERTEX_SHADER_CREATE_INFO(eevee_nodetree)
VERTEX_SHADER_CREATE_INFO(eevee_geom_bake_mesh)

#include "draw_model_lib.glsl"
#include "eevee_attributes_mesh_lib.glsl"
#include "eevee_nodetree_vert_lib.glsl"
#include "eevee_surf_lib.glsl"

void main()
{
  DRW_VIEW_FROM_RESOURCE_ID;

  init_interface();

  interp.P = drw_point_object_to_world(pos);
  interp.N = normalize(drw_normal_object_to_world(nor));
  bake_interp.Ng = normalize(drw_normal_object_to_world(geom_nor));

  init_globals();
  attrib_load(MeshVertex{0});

  interp.P += nodetree_displacement();

  gl_Position = float4(bake_uv * 2.0f - 1.0f, 0.0f, 1.0f);
}
