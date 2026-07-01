/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared filter material object info structures between C++ and GLSL.
 */

#pragma once

#include "eevee_defines.hh"

#define FILTER_GRAPH_ALPHA_MODE_OPACITY 0
#define FILTER_GRAPH_ALPHA_MODE_TRANSMITTANCE 1
#define FILTER_GRAPH_ALPHA_MODE_DEPTH 2

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

struct [[host_shared]] FilterGraphInputHandleData {
  uint type;
  int index;
  int alpha_mode;
  int _pad;
};

#ifndef GPU_SHADER
BLI_STATIC_ASSERT_ALIGN(FilterObjectInfoData, 16)
BLI_STATIC_ASSERT_ALIGN(FilterGraphInputHandleData, 16)
}  // namespace blender::eevee
#endif
