/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"
#  include "eevee_defines.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_render_texture_extract_base)
LOCAL_GROUP_SIZE(FILM_GROUP_SIZE, FILM_GROUP_SIZE)
SAMPLER(0, DEPTH_2D, depth_tx)
SAMPLER(1, FLOAT_2D, combined_tx)
SAMPLER(2, UINT_2D, gbuf_header_tx)
SAMPLER(3, FLOAT_2D_ARRAY, gbuf_normal_tx)
PUSH_CONSTANT(IVEC2, output_extent)
PUSH_CONSTANT(INT, output_type)
COMPUTE_SOURCE("eevee_render_texture_extract_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_render_texture_extract_rgba16f)
DO_STATIC_COMPILATION()
IMAGE(4, GPU_RGBA16F, WRITE, FLOAT_2D, output_img)
ADDITIONAL_INFO(eevee_render_texture_extract_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_render_texture_extract_rgba32f)
DO_STATIC_COMPILATION()
IMAGE(4, GPU_RGBA32F, WRITE, FLOAT_2D, output_img)
ADDITIONAL_INFO(eevee_render_texture_extract_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_render_texture_extract_r16f)
DO_STATIC_COMPILATION()
IMAGE(4, GPU_R16F, WRITE, FLOAT_2D, output_img)
ADDITIONAL_INFO(eevee_render_texture_extract_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_render_texture_extract_r32f)
DO_STATIC_COMPILATION()
IMAGE(4, GPU_R32F, WRITE, FLOAT_2D, output_img)
ADDITIONAL_INFO(eevee_render_texture_extract_base)
GPU_SHADER_CREATE_END()
