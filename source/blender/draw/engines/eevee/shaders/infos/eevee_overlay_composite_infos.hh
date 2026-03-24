/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "eevee_fullscreen_infos.hh"
#  include "eevee_uniform_infos.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_overlay_composite)
DO_STATIC_COMPILATION()
SAMPLER(0, sampler2D, scene_color_tx)
SAMPLER(1, sampler2D, overlay_color_tx)
PUSH_CONSTANT(int, overlay_alpha_mode)
PUSH_CONSTANT(int, overlay_blend_mode)
PUSH_CONSTANT(float, overlay_opacity)
PUSH_CONSTANT(float2, overlay_offset)
PUSH_CONSTANT(float2, overlay_scale)
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_SOURCE("eevee_overlay_composite_frag.glsl")
ADDITIONAL_INFO(eevee_fullscreen)
ADDITIONAL_INFO(eevee_global_ubo)
GPU_SHADER_CREATE_END()
