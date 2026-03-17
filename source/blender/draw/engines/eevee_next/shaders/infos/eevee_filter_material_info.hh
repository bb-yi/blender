/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_filter_material)
DEFINE("MAT_FILTER")
SAMPLER(FILTER_SCENE_COLOR_TEX_SLOT, FLOAT_2D, scene_color_tx)
SAMPLER(FILTER_AOV_COLOR_TEX_SLOT, FLOAT_2D_ARRAY, rp_color_tx)
SAMPLER(FILTER_AOV_VALUE_TEX_SLOT, FLOAT_2D_ARRAY, rp_value_tx)
FRAGMENT_OUT(0, VEC4, out_color)
FRAGMENT_SOURCE("eevee_filter_material_frag.glsl")
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_render_texture_data)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_utility_texture)
GPU_SHADER_CREATE_END()
