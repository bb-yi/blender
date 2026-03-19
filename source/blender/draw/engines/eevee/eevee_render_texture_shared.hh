/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared render texture structures between C++ and GLSL.
 */

#pragma once

#include "eevee_defines.hh"

#ifndef GPU_SHADER
#  include "BLI_memory_utils.hh"
#  include "GPU_shader_shared.hh"

namespace blender::eevee {
#endif

enum [[host_shared]] eRenderTextureFlags : uint32_t {
  RENDER_TEXTURE_SLOT_VALID = 1u << 0u,
  RENDER_TEXTURE_SLOT_CAPTURING = 1u << 1u,
  RENDER_TEXTURE_SLOT_SOURCE_SHIFT = 8u,
  RENDER_TEXTURE_SLOT_SOURCE_MASK = 0xFu << RENDER_TEXTURE_SLOT_SOURCE_SHIFT,
  RENDER_TEXTURE_SLOT_FORMAT_SHIFT = 12u,
  RENDER_TEXTURE_SLOT_FORMAT_MASK = 0xFu << RENDER_TEXTURE_SLOT_FORMAT_SHIFT,
};

struct [[host_shared]] RenderTextureData {
  float4x4 viewproj;
  float4x4 prev_viewproj;
  int4 info;
};

#ifndef GPU_SHADER
BLI_STATIC_ASSERT_ALIGN(RenderTextureData, 16)
}  // namespace blender::eevee
#endif
