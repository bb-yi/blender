/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared filter material object info structures between C++ and GLSL.
 */

#pragma once

#include "eevee_defines.hh"

#ifndef GPU_SHADER
#  include "BLI_memory_utils.hh"
#  include "GPU_shader_shared.hh"

namespace blender::eevee {
#endif

struct [[host_shared]] FilterObjectInfoData {
  float4 location;
  float4 rotation;
  float4 scale;
  float4 color;
  float4 metadata;
};

#ifndef GPU_SHADER
BLI_STATIC_ASSERT_ALIGN(FilterObjectInfoData, 16)
}  // namespace blender::eevee
#endif
