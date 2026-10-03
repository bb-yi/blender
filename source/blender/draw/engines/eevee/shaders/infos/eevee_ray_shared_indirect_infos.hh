/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#  include "eevee_uniform_infos.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_ray_shared_indirect_accum)
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
LOCAL_GROUP_SIZE(RAYTRACE_GROUP_SIZE, RAYTRACE_GROUP_SIZE)
ADDITIONAL_INFO(eevee_Uniform)
ADDITIONAL_INFO(gbuffer_Reader)
PUSH_CONSTANT(int, closure_index)
STORAGE_BUF(4, read, uint, tiles_coord_buf[])
IMAGE(0, RAYTRACE_RADIANCE_FORMAT, read, image2D, ray_radiance_img)
IMAGE(1, RAYTRACE_RADIANCE_FORMAT, read_write, image2D, shared_radiance_img)
IMAGE(2, SFLOAT_16_16_16_16, read_write, image2D, shared_albedo_img)
IMAGE(3, SFLOAT_16_16_16_16, write, image2D, shared_normal_img)
COMPUTE_SOURCE("eevee_ray_shared_indirect_accum_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_ray_merge_specular)
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
LOCAL_GROUP_SIZE(RAYTRACE_GROUP_SIZE, RAYTRACE_GROUP_SIZE)
ADDITIONAL_INFO(gbuffer_Reader)
PUSH_CONSTANT(int, feature_mask)
PUSH_CONSTANT(bool, use_shared_indirect)
SAMPLER(3, sampler2D, specular_0_tx)
SAMPLER(4, sampler2D, specular_1_tx)
SAMPLER(5, sampler2D, specular_2_tx)
SAMPLER(6, sampler2D, shared_specular_tx)
IMAGE(0, RAYTRACE_RADIANCE_FORMAT, write, image2D, indirect_0_img)
IMAGE(1, RAYTRACE_RADIANCE_FORMAT, write, image2D, indirect_1_img)
IMAGE(2, RAYTRACE_RADIANCE_FORMAT, write, image2D, indirect_2_img)
IMAGE(3, RAYTRACE_RADIANCE_FORMAT, read_write, image2D, shared_indirect_img)
COMPUTE_SOURCE("eevee_ray_merge_specular_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_ray_scene_final_specular)
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
LOCAL_GROUP_SIZE(RAYTRACE_GROUP_SIZE, RAYTRACE_GROUP_SIZE)
ADDITIONAL_INFO(gbuffer_Reader)
ADDITIONAL_INFO(eevee_Uniform)
ADDITIONAL_INFO(draw_Infos)
PUSH_CONSTANT(int, feature_mask)
PUSH_CONSTANT(bool, use_shared_indirect)
SAMPLER(3, sampler2D, specular_0_tx)
SAMPLER(4, sampler2D, specular_1_tx)
SAMPLER(5, sampler2D, specular_2_tx)
SAMPLER(6, sampler2D, shared_specular_tx)
IMAGE(0, SFLOAT_16_16_16_16, read_write, image2D, combined_img)
IMAGE(1, SFLOAT_16_16_16_16, read_write, image2DArray, rp_color_img)
COMPUTE_SOURCE("eevee_ray_scene_final_specular_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_ray_shared_indirect_reconstruct)
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
LOCAL_GROUP_SIZE(RAYTRACE_GROUP_SIZE, RAYTRACE_GROUP_SIZE)
ADDITIONAL_INFO(eevee_Uniform)
ADDITIONAL_INFO(gbuffer_Reader)
PUSH_CONSTANT(int, active_closure_count)
PUSH_CONSTANT(bool, source_is_oidn)
SAMPLER(3, sampler2D, depth_tx)
IMAGE(0, RAYTRACE_RADIANCE_FORMAT, read, image2D, shared_radiance_img)
IMAGE(1, SFLOAT_16_16_16_16, read, image2D, shared_albedo_img)
IMAGE(2, RAYTRACE_RADIANCE_FORMAT, write, image2D, out_radiance_img)
COMPUTE_SOURCE("eevee_ray_shared_indirect_reconstruct_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
