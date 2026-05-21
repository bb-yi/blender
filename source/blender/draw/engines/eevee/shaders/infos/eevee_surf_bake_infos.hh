/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "BLI_utildefines_variadic.h"

#  include "gpu_shader_compat.hh"

#  include "draw_object_infos_infos.hh"
#  include "draw_view_infos.hh"

#  include "eevee_common_infos.hh"
#  include "eevee_light_infos.hh"
#  include "eevee_lightprobe_infos.hh"
#  include "eevee_sampling_infos.hh"
#  include "eevee_shadow_infos.hh"
#  include "eevee_shadow_shared.hh"
#  include "eevee_uniform_infos.hh"
#endif

#ifdef GLSL_CPP_STUBS
#  define MAT_TRANSPARENT
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_surf_bake_color)
DEFINE("MAT_SURFACE_CULL")
DEFINE("MAT_BAKE_COLOR")
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
PUSH_CONSTANT(int, surface_cull_mode)
FRAGMENT_OUT(0, float4, out_color)
SAMPLER(LIGHT_SHADER_TEX_SLOT, sampler2DArray, light_shader_tx)
STORAGE_BUF(LIGHT_SHADER_INDEX_BUF_SLOT, read, int, light_shader_index_buf[])
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_shadow_data)
ADDITIONAL_INFO(eevee_lightprobe_data)
FRAGMENT_SOURCE("eevee_surf_bake_color_frag.glsl")
GPU_SHADER_CREATE_END()
