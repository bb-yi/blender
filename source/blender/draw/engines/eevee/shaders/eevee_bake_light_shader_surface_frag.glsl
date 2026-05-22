/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Store the UV-space bake surface context needed to evaluate point-dependent light shaders.
 */

#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_surf_bake_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_geom_bake_mesh_basic)
FRAGMENT_SHADER_CREATE_INFO(eevee_bake_light_shader_surface)

void main()
{
  out_position = float4(interp.P, 1.0f);
  out_normal = float4(normalize(bake_interp.Ng), 1.0f);
}
