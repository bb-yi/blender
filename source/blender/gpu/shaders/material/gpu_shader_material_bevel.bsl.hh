/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

#include "gpu_shader_material_interface.bsl.hh"

[[node]]
void node_bevel(float radius, float3 N, float samples,
                [[resource_table]] KernelGlobals &kg, ShadingData &sd, float3 &result)
{
  bevel_eval(kg, sd, radius, N, samples, result);
}
