/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "eevee_light_shared.hh"
#  include "eevee_lightprobe_shared.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_light_data)
TYPEDEF_SOURCE("eevee_light_shared.hh")
STORAGE_BUF(LIGHT_CULL_BUF_SLOT, read, LightCullingData, light_cull_buf)
STORAGE_BUF(LIGHT_BUF_SLOT, read, LightData, light_buf[])
STORAGE_BUF(LIGHT_ZBIN_BUF_SLOT, read, uint, light_zbin_buf[])
STORAGE_BUF(LIGHT_TILE_BUF_SLOT, read, uint, light_tile_buf[])
/* Metal codegen currently needs a literal size here; scoped enum constants from host_shared
 * types are emitted into stage namespaces and are not visible unqualified in this declaration. */
UNIFORM_BUF(WORLD_SUNLIGHT_BUF_SLOT, LightData, sunlight_buf[2])
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_light_shader)
DEFINE("MAT_LIGHT_SHADER")
PUSH_CONSTANT(int, light_index)
FRAGMENT_OUT(0, float4, out_light_shader)
FRAGMENT_SOURCE("eevee_light_shader_frag.glsl")
ADDITIONAL_INFO(eevee_fullscreen)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_gbuffer_data)
ADDITIONAL_INFO(eevee_hiz_data)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_object_infos)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_light_shader_front)
DEFINE("MAT_LIGHT_SHADER")
DEFINE("MAT_LIGHT_SHADER_FRONT_LAYER")
PUSH_CONSTANT(int, light_index)
FRAGMENT_OUT(0, float4, out_light_shader)
FRAGMENT_SOURCE("eevee_light_shader_front_frag.glsl")
ADDITIONAL_INFO(eevee_fullscreen)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_hiz_data)
SAMPLER(PREPASS_NORMAL_TEX_SLOT, sampler2D, prepass_normal_tx)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_object_infos)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_light_shader_volume)
DEFINE("MAT_LIGHT_SHADER")
DEFINE("MAT_LIGHT_SHADER_VOLUME")
LOCAL_GROUP_SIZE(VOLUME_GROUP_SIZE, VOLUME_GROUP_SIZE, VOLUME_GROUP_SIZE)
PUSH_CONSTANT(int, light_index)
IMAGE(0, SFLOAT_16_16_16_16, write, image2DArray, out_light_shader_img)
COMPUTE_SOURCE("eevee_light_shader_volume_comp.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_object_infos)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_light_shader_uniform)
DEFINE("MAT_LIGHT_SHADER")
DEFINE("MAT_LIGHT_SHADER_UNIFORM")
LOCAL_GROUP_SIZE(1)
PUSH_CONSTANT(int, light_index)
STORAGE_BUF(LIGHT_SHADER_UNIFORM_BUF_SLOT, write, float4, out_light_shader_buf[])
COMPUTE_SOURCE("eevee_light_shader_uniform_comp.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_object_infos)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_light_shader_surfel)
DEFINE("MAT_LIGHT_SHADER")
DEFINE("MAT_LIGHT_SHADER_SURFEL")
LOCAL_GROUP_SIZE(SURFEL_GROUP_SIZE)
PUSH_CONSTANT(int, light_index)
STORAGE_BUF(SURFEL_BUF_SLOT, read, Surfel, surfel_buf[])
STORAGE_BUF(CAPTURE_BUF_SLOT, read, CaptureInfoData, capture_info_buf)
STORAGE_BUF(LIGHT_SHADER_SURFEL_BUF_SLOT, write, float4, out_light_shader_buf[])
COMPUTE_SOURCE("eevee_light_shader_surfel_comp.glsl")
TYPEDEF_SOURCE("eevee_lightprobe_shared.hh")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_object_infos)
GPU_SHADER_CREATE_END()
