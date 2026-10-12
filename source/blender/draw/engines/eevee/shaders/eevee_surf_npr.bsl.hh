/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_nodetree_frag_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"

/* Implemented by the legacy evaluation source attached to this material's generated node tree. */
void npr_evaluate(KernelGlobals &kg, ShadingData &sd, float &out_depth);

namespace eevee {

struct NPRPassResources {
  [[legacy_info]] ShaderCreateInfo eevee_surf_npr;
};


[[fragment]] [[early_fragment_tests]]
void surf_npr([[resource_table]] KernelGlobals &kg,
              [[resource_table]] NPRPassResources & /*srt*/,
              [[in]] const VertOutCommon &interp,
              [[in, condition(is_curves)]] const VertOutCurves &curves_interp,
              [[in, condition(is_pointcloud)]] const VertOutPointcloud &ptcloud_interp,
              [[frag_coord]] const float4 frag_co,
              [[bary_coord]] [[condition(use_barycentric)]] const float3 bary_co,
              [[front_facing]] const bool front_face)
{
  float out_depth = frag_co.z;
  const ViewMatrices view = kg.view.get(0);
  ShadingData sd = init_globals(kg.pipe, kg.uniforms, interp, view, front_face, frag_co);
  if (kg.pipe.is_mesh) [[static_branch]] {
    float3 barycentric_co = float3(0.0f);
    if (kg.pipe.use_barycentric) [[static_branch]] {
      barycentric_co = bary_co;
    }
    init_globals_mesh(interp, sd, barycentric_co);
  }
  else if (kg.pipe.is_curves) [[static_branch]] {
    init_globals_curves(interp, curves_interp, sd, view);
  }
  else if (kg.pipe.is_pointcloud) [[static_branch]] {
    init_globals_pointcloud(ptcloud_interp, sd);
  }
  npr_evaluate(kg, sd, out_depth);
}

[[fragment]]
void surf_npr_depth_offset([[resource_table]] KernelGlobals &kg,
                           [[resource_table]] NPRPassResources & /*srt*/,
                           [[in]] const VertOutCommon &interp,
                           [[in, condition(is_curves)]] const VertOutCurves &curves_interp,
                           [[in, condition(is_pointcloud)]] const VertOutPointcloud &ptcloud_interp,
                           [[frag_coord]] const float4 frag_co,
                           [[bary_coord]] [[condition(use_barycentric)]] const float3 bary_co,
                           [[front_facing]] const bool front_face,
                           [[frag_depth(any)]] float &out_depth)
{
  out_depth = frag_co.z;
  const ViewMatrices view = kg.view.get(0);
  ShadingData sd = init_globals(kg.pipe, kg.uniforms, interp, view, front_face, frag_co);
  if (kg.pipe.is_mesh) [[static_branch]] {
    float3 barycentric_co = float3(0.0f);
    if (kg.pipe.use_barycentric) [[static_branch]] {
      barycentric_co = bary_co;
    }
    init_globals_mesh(interp, sd, barycentric_co);
  }
  else if (kg.pipe.is_curves) [[static_branch]] {
    init_globals_curves(interp, curves_interp, sd, view);
  }
  else if (kg.pipe.is_pointcloud) [[static_branch]] {
    init_globals_pointcloud(ptcloud_interp, sd);
  }
  npr_evaluate(kg, sd, out_depth);
}

}  // namespace eevee
