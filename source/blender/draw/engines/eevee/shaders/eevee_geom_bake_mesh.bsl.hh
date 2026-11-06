/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_model.bsl.hh"
#include "eevee_attributes_mesh_lib.bsl.hh"
#include "eevee_nodetree_vert_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"

namespace eevee {

struct GeomBakeVertIn {
  [[attribute(0)]] float3 pos;
  [[attribute(1)]] float3 nor;
  [[attribute(2)]] float2 bake_uv;
  [[attribute(3)]] float3 geom_nor;
  [[attribute(4)]] int primitive_id;
};

[[vertex]]
void geom_bake_mesh([[resource_table]] KernelGlobals &kg,
                    [[resource_table]] const draw::Resource &res_id,
                    [[instance_index]] const int inst_index,
                    [[in]] const GeomBakeVertIn &vert_in,
                    [[out]] VertOutCommon &interp,
                    [[out]] VertOutBake &bake_interp,
                    [[position]] float4 &out_position)
{
  draw::ID id = res_id.get(inst_index);
  uint resource_id = id.resource_id<1>();
  const ViewMatrices view = kg.view.get(0);
  const ObjectMatrices obj = kg.model.get(resource_id);
  ObjectInfos ob_infos = kg.infos.get(resource_id);

  init_interface(interp, id.raw_id);
  interp.P = obj.point_object_to_world(vert_in.pos);
  interp.P_relative = to_float3x3(obj.model) * vert_in.pos;
  interp.N = normalize(obj.normal_object_to_world(vert_in.nor));
  interp.P_base = interp.P;
  bake_interp.Ng = normalize(obj.normal_object_to_world(vert_in.geom_nor));
  bake_interp.primitive_id = vert_in.primitive_id;

  ShadingData sd = init_globals(kg.pipe, kg.uniforms, interp, view, true, float4(0.0f));
  init_globals_mesh(interp, sd, float3(0.0f));
  float3 lP_orco = vert_in.pos * ob_infos.orco_mul + ob_infos.orco_add;
  attrib_load(MeshVertex{vert_in.pos, to_float3x3(obj.model_inverse), lP_orco});
  const float3 displacement = nodetree_displacement(kg, sd);
  interp.P += displacement;
  interp.P_relative += displacement;
  out_position = float4(vert_in.bake_uv * 2.0f - 1.0f, 0.0f, 1.0f);
}

}  // namespace eevee
