/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#  include "eevee_defines.hh"
#  include "eevee_filter_material_shared.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_filter_object_info_data)
TYPEDEF_SOURCE("eevee_filter_material_shared.hh")
UNIFORM_BUF(FILTER_OBJECT_INFO_BUF_SLOT, FilterObjectInfoData, filter_object_buf[FILTER_OBJECT_INFO_MAX])
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_filter_material)
DEFINE("MAT_FILTER")
SAMPLER(FILTER_SCENE_COLOR_TEX_SLOT, sampler2D, scene_color_tx)
SAMPLER(FILTER_AOV_COLOR_TEX_SLOT, sampler2DArray, rp_color_tx)
SAMPLER(FILTER_AOV_VALUE_TEX_SLOT, sampler2DArray, rp_value_tx)
SAMPLER(FILTER_DEPTH_TEX_SLOT, sampler2DDepth, depth_tx)
SAMPLER(FILTER_CRYPTOMATTE_TEX_SLOT, sampler2D, cryptomatte_tx)
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_SOURCE("eevee_filter_material_frag.glsl")
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_filter_object_info_data)
ADDITIONAL_INFO(eevee_render_texture_data)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_utility_texture)
GPU_SHADER_CREATE_END()
