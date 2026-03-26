/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_view_infos.hh"
#  include "eevee_fullscreen_infos.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_shadow_mask_filter_common)
SAMPLER(1, sampler2DDepth, depth_tx)
PUSH_CONSTANT(int2, filter_direction)
FRAGMENT_OUT(0, float, out_visibility)
FRAGMENT_SOURCE("eevee_shadow_filter_frag.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(eevee_fullscreen)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_mask_filter)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_shadow_mask_filter_common)
SAMPLER(0, sampler2D, shadow_tx)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_shadow_mask_filter_layered)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_shadow_mask_filter_common)
DEFINE("SHADOW_FILTER_LAYERED_INPUT")
SAMPLER(0, sampler2DArray, shadow_tx)
PUSH_CONSTANT(int, shadow_layer)
GPU_SHADER_CREATE_END()
