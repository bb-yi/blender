/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"
#include "gpu_shader_material_interface.bsl.hh"

[[node]]
void node_uvmap(float4 attr_uv, float3 &outvec)
{
  outvec = attr_uv.xyz;
}

[[node]]
void world_view_direction_get([[resource_table]] KernelGlobals &kg, ShadingData &sd, float3 &V)
{
  V = -coordinate_impl(kg, sd, sd.P, sd.N).incoming;
}
