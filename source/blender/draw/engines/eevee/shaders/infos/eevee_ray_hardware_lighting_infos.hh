/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#  include "draw_view_infos.hh"
#  include "draw_object_infos_infos.hh"
#  include "eevee_common_infos.hh"
#  include "eevee_uniform_infos.hh"
#  include "eevee_sampling_infos.hh"
#  include "eevee_lightprobe_infos.hh"
#  include "eevee_shadow_infos.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_ray_trace_hardware_lighting)
BUILTINS(BuiltinBits::NO_PREPROCESSOR | BuiltinBits::TEXTURE_ATOMIC)
LOCAL_GROUP_SIZE(RAYTRACE_GROUP_SIZE, RAYTRACE_GROUP_SIZE)
ADDITIONAL_INFO(draw_View)
ADDITIONAL_INFO(draw_Infos)
ADDITIONAL_INFO(eevee_Uniform)
ADDITIONAL_INFO(gbuffer_Reader)
ADDITIONAL_INFO(eevee_LightprobeRenderData)
ADDITIONAL_INFO(eevee_ShadowRenderData)
ADDITIONAL_INFO(UtilityTexture)
COMPILATION_CONSTANT(bool, shadow_random, true)
SPECIALIZATION_CONSTANT(int, closure_index, 0)
SPECIALIZATION_CONSTANT(bool, use_hardware_environment, true)
SPECIALIZATION_CONSTANT(bool, use_hardware_rt_shadows, false)
SPECIALIZATION_CONSTANT(bool, use_hardware_rt_environment_visibility, false)
PUSH_CONSTANT(int, hardware_trace_phase)
PUSH_CONSTANT(int, hardware_reflection_mode)
PUSH_CONSTANT(int, hardware_refraction_mode)
PUSH_CONSTANT(int, hardware_local_lights_len)
PUSH_CONSTANT(int, hardware_lights_len)
IMAGE(0, SFLOAT_16_16_16_16, read, image2D, ray_data_img)
IMAGE(1, RAYTRACE_RAYTIME_FORMAT, read_write, image2D, ray_time_img)
IMAGE(2, SFLOAT_16_16_16_16, read, image2D, hit_albedo_img)
IMAGE(3, SFLOAT_16_16_16_16, read, image2D, hit_material_img)
IMAGE(4, SFLOAT_16_16_16_16, read, image2D, hit_normal_img)
IMAGE(5, SFLOAT_16_16_16_16, read, image2D, hit_position_img)
IMAGE(6, RAYTRACE_RADIANCE_FORMAT, read_write, image2D, ray_radiance_img)
IMAGE(7, UINT_32_32_32_32, read, uimage2D, hit_identity_img)
SAMPLER(1, sampler2D, depth_tx)
SAMPLER(8, sampler2D, hardware_rt_hit_environment_visibility_tx)
SAMPLER(9, sampler2DArray, hardware_rt_shadow_visibility_tx)
SAMPLER(12, sampler2D, radiance_front_tx)
SAMPLER(13, sampler2D, radiance_back_tx)
SAMPLER(14, sampler2D, hit_world_position_tx)
SAMPLER(15, sampler2D, hardware_rt_environment_visibility_tx)
SAMPLER(22, sampler2D, hit_barycentric_tx)
SAMPLER(23, sampler2D, layered_receiver_hit_barycentric_tx)
SAMPLER(24, sampler2D, layered_receiver_throughput_tx)
SAMPLER(25, sampler2D, layered_receiver_ray_time_tx)
SAMPLER(26, sampler2D, layered_receiver_ray_radiance_tx)
SAMPLER(27, sampler2D, layered_receiver_hit_albedo_tx)
SAMPLER(28, sampler2D, layered_receiver_hit_material_tx)
SAMPLER(29, sampler2D, layered_receiver_hit_normal_tx)
SAMPLER(30, sampler2D, layered_receiver_hit_position_tx)
SAMPLER(31, usampler2D, layered_receiver_hit_identity_tx)
SAMPLER(32, sampler2D, layered_receiver_world_position_tx)
SAMPLER(33, sampler2D, hit_transmission_layer_tx)
SAMPLER(34, sampler2D, transmission_receiver_throughput_tx)
SAMPLER(35, sampler2D, transmission_receiver_ray_time_tx)
SAMPLER(36, sampler2D, transmission_receiver_ray_radiance_tx)
SAMPLER(37, sampler2D, transmission_receiver_hit_albedo_tx)
SAMPLER(38, sampler2D, transmission_receiver_hit_material_tx)
SAMPLER(39, sampler2D, transmission_receiver_hit_normal_tx)
SAMPLER(40, sampler2D, transmission_receiver_hit_position_tx)
SAMPLER(41, usampler2D, transmission_receiver_hit_identity_tx)
SAMPLER(42, sampler2D, transmission_receiver_world_position_tx)
SAMPLER(47, sampler2D, hardware_reflected_receiver_gi_tx)
SAMPLER(48, sampler2D, hardware_layered_receiver_gi_tx)
SAMPLER(49, sampler2DArray, hardware_layered_receiver_rt_shadow_visibility_tx)
SAMPLER(50, sampler2DArray, hardware_transmission_receiver_rt_shadow_visibility_tx)
SAMPLER(51, sampler2D, hardware_transmission_receiver_gi_tx)
SAMPLER(52, sampler2D, hardware_direct_light_tx)
SAMPLER(53, sampler2D, hardware_secondary_photon_gi_tx)
SAMPLER(54, sampler2D, hardware_layered_secondary_photon_gi_tx)
SAMPLER(55, sampler2D, hardware_transmission_secondary_photon_gi_tx)
STORAGE_BUF(5, read, uint, tiles_coord_buf[])
STORAGE_BUF(14, read, LightData, hardware_light_buf[])
UNIFORM_BUF(WORLD_SUNLIGHT_BUF_SLOT, LightData, hardware_sunlight_buf[2])
COMPUTE_SOURCE("eevee_ray_hardware_lighting_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
