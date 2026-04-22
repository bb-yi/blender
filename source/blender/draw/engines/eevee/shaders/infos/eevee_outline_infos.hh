/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_view_infos.hh"
#  include "eevee_common_infos.hh"
#  include "eevee_fullscreen_infos.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_outline_detect)
DO_STATIC_COMPILATION()
FRAGMENT_OUT(0, float4, out_outline_seed)
SAMPLER(OUTLINE_DEPTH_TEX_SLOT, sampler2D, depth_tx)
SAMPLER(OUTLINE_COLOR_TEX_SLOT, sampler2D, outline_color_tx)
SAMPLER(OUTLINE_INFO_TEX_SLOT, sampler2D, outline_info_tx)
ADDITIONAL_INFO(eevee_gbuffer_data)
ADDITIONAL_INFO(eevee_fullscreen)
ADDITIONAL_INFO(draw_view)
TYPEDEF_SOURCE("eevee_defines.hh")
FRAGMENT_SOURCE("eevee_outline_detect_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_outline_expand)
DO_STATIC_COMPILATION()
FRAGMENT_OUT(0, float4, out_outline_color)
SAMPLER(OUTLINE_DEPTH_TEX_SLOT, sampler2D, depth_tx)
SAMPLER(OUTLINE_SEED_TEX_SLOT, sampler2D, outline_seed_tx)
SAMPLER(OUTLINE_COLOR_TEX_SLOT, sampler2D, outline_color_tx)
SAMPLER(OUTLINE_INFO_TEX_SLOT, sampler2D, outline_info_tx)
ADDITIONAL_INFO(eevee_fullscreen)
ADDITIONAL_INFO(draw_view)
TYPEDEF_SOURCE("eevee_defines.hh")
FRAGMENT_SOURCE("eevee_outline_expand_frag.glsl")
GPU_SHADER_CREATE_END()
