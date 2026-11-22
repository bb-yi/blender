/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "gpu_shader_material_interface.bsl.hh"

[[node]]
void node_raycast(float3 position,
                  float3 direction,
                  float length,
                  [[resource_table]] KernelGlobals &kg,
                  const ShadingData &sd,
                  float &is_hit,
                  float &is_self_hit,
                  float &hit_distance,
                  float3 &hit_position,
                  float3 &hit_normal)
{
  bool hit = false;
  bool self_hit = false;
  raycast_eval(kg,
               sd,
               position,
               direction,
               length,
               false,
               hit,
               self_hit,
               hit_distance,
               hit_position,
               hit_normal);
  is_hit = hit ? 1.0f : 0.0f;
  is_self_hit = self_hit ? 1.0f : 0.0f;
}

[[node]]
void node_raycast_hardware(float3 position,
                           float3 direction,
                           float length,
                           bool only_local,
                           [[resource_table]] KernelGlobals &kg,
                           const ShadingData &sd,
                           float &is_hit,
                           float &is_self_hit,
                           float &hit_distance,
                           float3 &hit_position,
                           float3 &hit_normal)
{
  bool hit = false;
  bool self_hit = false;
  hit_distance = length;
  hit_position = float3(0.0f);
  hit_normal = float3(0.0f);
  hardware_raycast_eval(kg,
                        sd,
                        position,
                        direction,
                        length,
                        only_local,
                        hit,
                        self_hit,
                        hit_distance,
                        hit_position,
                        hit_normal);
  is_hit = hit ? 1.0f : 0.0f;
  is_self_hit = self_hit ? 1.0f : 0.0f;
}

[[node]]
void node_raycast_only_local(float3 position,
                             float3 direction,
                             float length,
                             [[resource_table]] KernelGlobals &kg,
                             const ShadingData &sd,
                             float &is_hit,
                             float &is_self_hit,
                             float &hit_distance,
                             float3 &hit_position,
                             float3 &hit_normal)
{
  bool hit = false;
  bool self_hit = false;
  raycast_eval(kg,
               sd,
               position,
               direction,
               length,
               true,
               hit,
               self_hit,
               hit_distance,
               hit_position,
               hit_normal);
  is_hit = hit ? 1.0f : 0.0f;
  is_self_hit = self_hit ? 1.0f : 0.0f;
}
