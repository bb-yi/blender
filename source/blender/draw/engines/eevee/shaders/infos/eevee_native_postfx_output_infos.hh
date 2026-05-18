/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#  include "eevee_defines.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_native_postfx_output_extract)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(FILM_GROUP_SIZE, FILM_GROUP_SIZE)
SAMPLER(0, sampler2D, depth_tx)
SAMPLER(1, sampler2D, vector_tx)
SAMPLER(2, sampler2DArray, rp_color_tx)
SAMPLER(3, sampler2DArray, rp_value_tx)
SAMPLER(4, sampler2D, outline_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, out_color_img)
PUSH_CONSTANT(int, source_kind)
PUSH_CONSTANT(int, source_layer)
COMPUTE_SOURCE("eevee_native_postfx_output_extract_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_native_postfx_output_pack_color)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(FILM_GROUP_SIZE, FILM_GROUP_SIZE)
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2DArray, output_img)
PUSH_CONSTANT(int, output_layer)
COMPUTE_SOURCE("eevee_native_postfx_output_pack_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_native_postfx_output_pack_value)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(FILM_GROUP_SIZE, FILM_GROUP_SIZE)
DEFINE("PACK_VALUE")
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, SFLOAT_16, write, image2DArray, output_img)
PUSH_CONSTANT(int, output_layer)
COMPUTE_SOURCE("eevee_native_postfx_output_pack_comp.glsl")
GPU_SHADER_CREATE_END()
