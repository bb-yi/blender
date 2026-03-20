/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#  include "draw_view_infos.hh"
#  include "eevee_defines.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_render_texture_extract_base)
LOCAL_GROUP_SIZE(FILM_GROUP_SIZE, FILM_GROUP_SIZE)
ADDITIONAL_INFO(draw_view)
SAMPLER(0, sampler2D, depth_tx)
SAMPLER(1, sampler2D, combined_tx)
SAMPLER(2, usampler2DArray, gbuf_header_tx)
SAMPLER(3, sampler2DArray, gbuf_normal_tx)
PUSH_CONSTANT(int2, output_extent)
PUSH_CONSTANT(int, output_type)
COMPUTE_SOURCE("eevee_render_texture_extract_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_render_texture_extract_rgba16f)
DO_STATIC_COMPILATION()
IMAGE(4, SFLOAT_16_16_16_16, write, image2D, output_img)
ADDITIONAL_INFO(eevee_render_texture_extract_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_render_texture_extract_rgba32f)
DO_STATIC_COMPILATION()
IMAGE(4, SFLOAT_32_32_32_32, write, image2D, output_img)
ADDITIONAL_INFO(eevee_render_texture_extract_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_render_texture_extract_r16f)
DO_STATIC_COMPILATION()
IMAGE(4, SFLOAT_16, write, image2D, output_img)
ADDITIONAL_INFO(eevee_render_texture_extract_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_render_texture_extract_r32f)
DO_STATIC_COMPILATION()
IMAGE(4, SFLOAT_32, write, image2D, output_img)
ADDITIONAL_INFO(eevee_render_texture_extract_base)
GPU_SHADER_CREATE_END()
