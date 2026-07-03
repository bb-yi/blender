/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#  include "draw_view_infos.hh"
#  include "eevee_common_infos.hh"
#  include "eevee_defines.hh"
#  include "eevee_fullscreen_infos.hh"
#  include "eevee_filter_material_shared.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_filter_object_info_data)
TYPEDEF_SOURCE("eevee_filter_material_shared.hh")
UNIFORM_BUF(FILTER_OBJECT_INFO_BUF_SLOT, FilterObjectInfoData, filter_object_buf[FILTER_OBJECT_INFO_MAX])
UNIFORM_BUF(FILTER_GRAPH_INPUT_BUF_SLOT, FilterGraphInputHandleData, filter_graph_input_buf[FILTER_GRAPH_INPUT_MAX])
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_filter_material)
DEFINE("MAT_FILTER")
SAMPLER(FILTER_SCENE_COLOR_TEX_SLOT, sampler2D, scene_color_tx)
SAMPLER(FILTER_AOV_COLOR_TEX_SLOT, sampler2DArray, rp_color_tx)
SAMPLER(FILTER_AOV_VALUE_TEX_SLOT, sampler2DArray, rp_value_tx)
SAMPLER(FILTER_DEPTH_TEX_SLOT, sampler2DDepth, depth_tx)
SAMPLER(FILTER_CRYPTOMATTE_TEX_SLOT, sampler2D, cryptomatte_tx)
SAMPLER(FILTER_GRAPH_INPUT_TEX_SLOT, sampler2DArray, filter_graph_input_tx)
IMAGE(FILTER_GRAPH_OUTPUT_IMG_SLOT, SFLOAT_16_16_16_16, write, image2DArray, filter_graph_output_img)
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_SOURCE("eevee_filter_material_frag.glsl")
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_render_pass_out)
ADDITIONAL_INFO(eevee_filter_object_info_data)
ADDITIONAL_INFO(eevee_render_texture_data)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_utility_texture)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_filter_graph_input_copy)
DO_STATIC_COMPILATION()
SAMPLER(0, sampler2DArray, input_tx)
PUSH_CONSTANT(int, input_layer)
PUSH_CONSTANT(int2, target_extent)
PUSH_CONSTANT(int, resample_mode)
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_SOURCE("eevee_filter_graph_input_copy_frag.glsl")
ADDITIONAL_INFO(eevee_fullscreen)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_filter_graph_resolve)
DO_STATIC_COMPILATION()
SAMPLER(0, sampler2D, scene_color_tx)
SAMPLER(1, sampler2DArray, rp_color_tx)
SAMPLER(2, sampler2DArray, rp_value_tx)
SAMPLER(3, sampler2DDepth, depth_tx)
SAMPLER(4, sampler2DArray, filter_graph_input_tx)
PUSH_CONSTANT(int2, target_extent)
PUSH_CONSTANT(int, resolve_mode)
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_SOURCE("eevee_filter_graph_resolve_frag.glsl")
ADDITIONAL_INFO(eevee_fullscreen)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_filter_object_info_data)
GPU_SHADER_CREATE_END()
