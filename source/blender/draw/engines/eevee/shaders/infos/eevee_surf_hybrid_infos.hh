/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "eevee_light_infos.hh"
#  include "eevee_lightprobe_infos.hh"
#  include "eevee_sampling_infos.hh"
#  include "eevee_shadow_infos.hh"
#  include "eevee_surf_deferred_infos.hh"
#endif

#ifdef GLSL_CPP_STUBS
#  define CURVES_SHADER
#  define DRW_HAIR_INFO

#  define POINTCLOUD_SHADER
#  define DRW_POINTCLOUD_INFO

#  define SHADOW_UPDATE_ATOMIC_RASTER
#  define MAT_TRANSPARENT
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_surf_deferred_hybrid)
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
FRAGMENT_SOURCE("eevee_surf_hybrid_frag.glsl")
TYPEDEF_SOURCE("eevee_render_texture_shared.hh")
ADDITIONAL_INFO(eevee_surf_deferred_base)
ADDITIONAL_INFO(eevee_light_data)
DEFINE("LIGHT_SHADER_TEXTURE_EVAL")
SAMPLER(LIGHT_SHADER_TEX_SLOT, sampler2DArray, light_shader_tx)
STORAGE_BUF(LIGHT_SHADER_INDEX_BUF_SLOT, read, int, light_shader_index_buf[])
STORAGE_BUF(LIGHT_SHADER_UNIFORM_BUF_SLOT, read, float4, light_shader_uniform_buf[])
ADDITIONAL_INFO(eevee_lightprobe_data)
ADDITIONAL_INFO(eevee_shadow_data)
ADDITIONAL_INFO(eevee_render_texture_data)
/* Optionally added depending on the material. */
// ADDITIONAL_INFO(eevee_hiz_prev_data)
// ADDITIONAL_INFO(eevee_previous_layer_radiance)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_surf_deferred_hybrid_depth_offset)
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
FRAGMENT_SOURCE("eevee_surf_hybrid_frag.glsl")
TYPEDEF_SOURCE("eevee_render_texture_shared.hh")
ADDITIONAL_INFO(eevee_surf_deferred_base_depth_offset)
ADDITIONAL_INFO(eevee_light_data)
DEFINE("LIGHT_SHADER_TEXTURE_EVAL")
SAMPLER(LIGHT_SHADER_TEX_SLOT, sampler2DArray, light_shader_tx)
STORAGE_BUF(LIGHT_SHADER_INDEX_BUF_SLOT, read, int, light_shader_index_buf[])
STORAGE_BUF(LIGHT_SHADER_UNIFORM_BUF_SLOT, read, float4, light_shader_uniform_buf[])
ADDITIONAL_INFO(eevee_lightprobe_data)
ADDITIONAL_INFO(eevee_shadow_data)
ADDITIONAL_INFO(eevee_render_texture_data)
/* Optionally added depending on the material. */
// ADDITIONAL_INFO(eevee_hiz_prev_data)
// ADDITIONAL_INFO(eevee_previous_layer_radiance)
GPU_SHADER_CREATE_END()
