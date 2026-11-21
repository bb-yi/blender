/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"

[[node]]
void node_light_info(const float light_index,
                     [[resource_table]] KernelGlobals &kg,
                     const ShadingData &sd,
                     float4 &color,
                     float &power,
                     float3 &position)
{
  node_light_info_impl(kg, sd, int(light_index), color, power, position);
}
