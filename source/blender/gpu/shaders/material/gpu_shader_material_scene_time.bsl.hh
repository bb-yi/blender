/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"

[[node]]
void node_scene_time(float scale,
                     [[resource_table]] KernelGlobals &kg,
                     float &frame,
                     float &seconds,
                     float &timeline,
                     float &scaled_frame)
{
  scene_time_uniforms(kg, seconds, frame, timeline);
  const float scale_safe = (abs(scale) > 1e-8f) ? scale : 1.0f;
  scaled_frame = frame / scale_safe;
}
