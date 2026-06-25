/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_view_infos.hh"
#  include "eevee_common_infos.hh"
#  include "eevee_fullscreen_infos.hh"
#  include "eevee_uniform_infos.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_outline_detect)
DO_STATIC_COMPILATION()
FRAGMENT_OUT(0, float4, out_outline_seed)
SAMPLER(OUTLINE_DEPTH_TEX_SLOT, sampler2DDepth, depth_tx)
SAMPLER(PREPASS_NORMAL_TEX_SLOT, sampler2D, prepass_normal_tx)
SAMPLER(OUTLINE_COLOR_TEX_SLOT, sampler2D, outline_color_tx)
SAMPLER(OUTLINE_INFO_TEX_SLOT, usampler2D, outline_info_tx)
ADDITIONAL_INFO(eevee_gbuffer_data)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_fullscreen)
ADDITIONAL_INFO(draw_view)
TYPEDEF_SOURCE("eevee_defines.hh")
FRAGMENT_SOURCE("eevee_outline_detect_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_outline_jfa_init)
DO_STATIC_COMPILATION()
FRAGMENT_OUT(0, float2, out_jfa_coord)
SAMPLER(OUTLINE_SEED_TEX_SLOT, sampler2D, outline_seed_tx)
ADDITIONAL_INFO(eevee_fullscreen)
TYPEDEF_SOURCE("eevee_defines.hh")
FRAGMENT_SOURCE("eevee_outline_jfa_init_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_outline_factor_blur)
DO_STATIC_COMPILATION()
FRAGMENT_OUT(0, float4, out_outline_seed)
SAMPLER(OUTLINE_SEED_TEX_SLOT, sampler2D, outline_seed_tx)
SAMPLER(OUTLINE_INFO_TEX_SLOT, usampler2D, outline_info_tx)
ADDITIONAL_INFO(eevee_fullscreen)
TYPEDEF_SOURCE("eevee_defines.hh")
FRAGMENT_SOURCE("eevee_outline_factor_blur_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_outline_jfa_step)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(OUTLINE_JFA_STEP_GROUP_SIZE, OUTLINE_JFA_STEP_GROUP_SIZE, 1)
IMAGE(OUTLINE_JFA_IN_IMG_SLOT, SFLOAT_32_32, read, image2D, jfa_in_img)
IMAGE(OUTLINE_JFA_OUT_IMG_SLOT, SFLOAT_32_32, write, image2D, jfa_out_img)
PUSH_CONSTANT(int, jfa_step_size)
TYPEDEF_SOURCE("eevee_defines.hh")
COMPUTE_SOURCE("eevee_outline_jfa_step_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_outline_resolve)
DO_STATIC_COMPILATION()
FRAGMENT_OUT(0, float4, out_outline_color)
FRAGMENT_OUT(1, float, out_outline_depth)
FRAGMENT_OUT(2, float4, out_outline_velocity)
SAMPLER(OUTLINE_DEPTH_TEX_SLOT, sampler2DDepth, depth_tx)
SAMPLER(OUTLINE_VECTOR_TEX_SLOT, sampler2D, vector_tx)
SAMPLER(OUTLINE_OCCLUSION_DEPTH_TEX_SLOT, sampler2DDepth, outline_occlusion_depth_tx)
SAMPLER(OUTLINE_SEED_TEX_SLOT, sampler2D, outline_seed_tx)
SAMPLER(OUTLINE_COLOR_TEX_SLOT, sampler2D, outline_color_tx)
SAMPLER(OUTLINE_INFO_TEX_SLOT, usampler2D, outline_info_tx)
SAMPLER(OUTLINE_JFA_TEX_SLOT, sampler2D, jfa_tx)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_fullscreen)
ADDITIONAL_INFO(draw_view)
TYPEDEF_SOURCE("eevee_defines.hh")
FRAGMENT_SOURCE("eevee_outline_resolve_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_outline_freestyle)
DO_STATIC_COMPILATION()
BUILTINS(BuiltinBits::CLIP_CONTROL)
VERTEX_IN(0, float3, pos)
FRAGMENT_OUT(0, float4, out_outline_seed)
SAMPLER(OUTLINE_DEPTH_TEX_SLOT, sampler2DDepth, depth_tx)
SAMPLER(OUTLINE_COLOR_TEX_SLOT, sampler2D, outline_color_tx)
SAMPLER(OUTLINE_INFO_TEX_SLOT, usampler2D, outline_info_tx)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_resource_id_varying)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(draw_view)
TYPEDEF_SOURCE("eevee_defines.hh")
VERTEX_SOURCE("eevee_outline_freestyle_vert.glsl")
FRAGMENT_SOURCE("eevee_outline_freestyle_frag.glsl")
GPU_SHADER_CREATE_END()
