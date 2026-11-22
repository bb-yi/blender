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
                           float3 &hit_normal,
                           float4 &hit_attributes)
{
  bool hit = false;
  bool self_hit = false;
  hit_distance = length;
  hit_position = float3(0.0f);
  hit_normal = float3(0.0f);
  hit_attributes = float4(-1.0f, 0.0f, 0.0f, 0.0f);
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
                        hit_normal,
                        hit_attributes);
  is_hit = hit ? 1.0f : 0.0f;
  is_self_hit = self_hit ? 1.0f : 0.0f;
}

[[node]] void node_raycast_attribute_float(float4 hit,
                                           int attribute_id,
                                           [[resource_table]] KernelGlobals &kg,
                                           float &value)
{
  int components;
  float4 sampled = hardware_raycast_attribute(kg, hit, attribute_id, components);
  value = components < 3 ? sampled.x : (sampled.x + sampled.y + sampled.z) / 3.0f;
}

[[node]] void node_raycast_attribute_vector(float4 hit,
                                            int attribute_id,
                                            [[resource_table]] KernelGlobals &kg,
                                            float3 &value)
{
  int components;
  value = hardware_raycast_attribute(kg, hit, attribute_id, components).xyz;
}

[[node]] void node_raycast_attribute_color(float4 hit,
                                           int attribute_id,
                                           [[resource_table]] KernelGlobals &kg,
                                           float4 &color,
                                           float &alpha)
{
  int components;
  color = hardware_raycast_attribute(kg, hit, attribute_id, components);
  alpha = color.a;
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
