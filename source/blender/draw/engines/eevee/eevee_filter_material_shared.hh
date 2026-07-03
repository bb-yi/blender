/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared filter material object info structures between C++ and GLSL.
 */

#pragma once

#include "eevee_defines.hh"

#define TEX_HANDLE_NULL 0u
#define TEX_HANDLE_RP_COLOR 1u
#define TEX_HANDLE_RP_VALUE 2u
#define TEX_HANDLE_SCENE 30u
#define TEX_HANDLE_FILTER_GRAPH_INPUT 31u
#define TEX_HANDLE_FILTER_GRAPH_TEXTURE 32u

#define FILTER_GRAPH_ALPHA_MODE_OPACITY 0
#define FILTER_GRAPH_ALPHA_MODE_TRANSMITTANCE 1
#define FILTER_GRAPH_ALPHA_MODE_DEPTH 2

#define FILTER_GRAPH_SOURCE_COLOR 0
#define FILTER_GRAPH_SOURCE_DEPTH 1
#define FILTER_GRAPH_SOURCE_VALUE 2
#define FILTER_GRAPH_SOURCE_DATA 3
#define FILTER_GRAPH_SOURCE_INTERMEDIATE 4

#define FILTER_GRAPH_RESAMPLE_NEAREST 0
#define FILTER_GRAPH_RESAMPLE_LINEAR 1

#define FILTER_GRAPH_RESOLVE_RAW 0
#define FILTER_GRAPH_RESOLVE_STAGE_OUTPUT 1

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
  int source_kind;
};

#ifndef GPU_SHADER
BLI_STATIC_ASSERT_ALIGN(FilterObjectInfoData, 16)
BLI_STATIC_ASSERT_ALIGN(FilterGraphInputHandleData, 16)
}  // namespace blender::eevee
#endif
