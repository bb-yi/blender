/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_attributes_world_lib.bsl.hh"
#include "eevee_nodetree_frag_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"

void nodetree_filter_outputs(KernelGlobals &kg, ShadingData &sd);

namespace eevee {

struct FilterPassResources {
  [[legacy_info]] ShaderCreateInfo eevee_filter_material;
};

[[fragment]]
void filter_material([[resource_table]] KernelGlobals &kg,
                      [[resource_table]] FilterPassResources & /*srt*/,
                      [[in]] const VertOutCommon &interp,
                      [[frag_coord]] const float4 frag_co,
                      [[front_facing]] const bool front_face)
{
  const ViewMatrices view = kg.view.get(0);
  ShadingData sd = init_globals(kg.pipe, kg.uniforms, interp, view, front_face, frag_co);
  sd.N = view.normal_view_to_world(view.view_incident_vector(interp.P));
  sd.Ni = sd.N;
  sd.Ng = sd.N;
  sd.P = -sd.N;
  attrib_load(WorldPoint{sd.P});

  nodetree_filter_outputs(kg, sd);
}

}  // namespace eevee
