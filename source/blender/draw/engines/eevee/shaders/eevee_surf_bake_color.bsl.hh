/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_nodetree_frag_lib.bsl.hh"
#include "eevee_forward_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"

void bake_evaluate(KernelGlobals &kg, ShadingData &sd, float3 geom_normal, int primitive_id,
                   int thickness_mode);

float4 bake_lighting_eval(KernelGlobals &kg, ShadingData &sd)
{
  float3 radiance, transmittance;
  eevee::forward_lighting_eval(kg, sd, kg.view_matrices_get(sd), kg.resource_id_get(sd),
                               sd.thickness, sd.frag_co.xy, radiance, transmittance);
  return float4(radiance, saturate(1.0f - average(transmittance)));
}

namespace eevee {
struct BakePassResources {
  [[legacy_info]] ShaderCreateInfo eevee_surf_bake_color;
};

[[fragment]]
void surf_bake_color([[resource_table]] KernelGlobals &kg,
                     [[resource_table]] BakePassResources & /*srt*/,
                     [[in]] const VertOutCommon &interp,
                     [[in]] const VertOutBake &bake_interp,
                     [[frag_coord]] const float4 frag_co,
                     [[front_facing]] const bool front_face)
{
  material_surface_cull_discard(front_face);
  const ViewMatrices view = kg.view.get(0);
  ShadingData sd = init_globals(kg.pipe, kg.uniforms, interp, view, true, frag_co);
  init_globals_mesh(interp, sd, float3(0.0f));
  bake_evaluate(kg, sd, bake_interp.Ng, bake_interp.primitive_id, int(kg.nt.node_tree.thickness_mode));
}

}  // namespace eevee
