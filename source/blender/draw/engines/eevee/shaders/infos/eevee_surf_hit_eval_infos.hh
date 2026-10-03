/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#  include "draw_view_infos.hh"
#  include "eevee_common_infos.hh"
#  include "eevee_sampling_infos.hh"
#  include "eevee_uniform_infos.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_surf_hit_eval_flat_iface, hit_flat)
FLAT(uint, packed_texel)
FLAT(uint, resource_id_raw)
FLAT(int, front_facing)
FLAT(float3, view_origin)
GPU_SHADER_NAMED_INTERFACE_END(hit_flat)

GPU_SHADER_CREATE_INFO(eevee_geom_hit_mesh)
DEFINE("MAT_GEOM_MESH")
BUILTINS(BuiltinBits::VERTEX_ID)
VERTEX_SOURCE("eevee_geom_hit_mesh_vert.glsl")
VERTEX_OUT(eevee_surf_hit_eval_flat_iface)
ADDITIONAL_INFO(eevee_geom_iface_info)
ADDITIONAL_INFO(draw_Model)
ADDITIONAL_INFO(draw_Infos)
ADDITIONAL_INFO(draw_View)
ADDITIONAL_INFO(gpu_index_buffer_load)
STORAGE_BUF_FREQ(0, read, uint, pos[], GEOMETRY)
STORAGE_BUF_FREQ(1, read, uint, nor[], GEOMETRY)
PUSH_CONSTANT(int2, gpu_attr_0)
PUSH_CONSTANT(int, gpu_attr_0_meta)
PUSH_CONSTANT(int2, gpu_attr_1)
PUSH_CONSTANT(int, gpu_attr_1_meta)
/* Primitive IDs are relative to the material's BLAS index subrange. */
PUSH_CONSTANT(int, hit_eval_index_start)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_surf_hit_eval)
DEFINE("MAT_HIT_EVAL")
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
TYPEDEF_SOURCE("eevee_raytrace_shared.hh")
IMAGE(0, SFLOAT_16_16_16_16, read_write, image2D, hit_albedo_img)
IMAGE(1, SFLOAT_16_16_16_16, read_write, image2D, hit_material_img)
IMAGE(2, SFLOAT_16_16_16_16, read_write, image2D, hit_normal_img)
IMAGE(3, SFLOAT_16_16_16_16, read_write, image2D, hit_position_img)
IMAGE(4, RAYTRACE_RADIANCE_FORMAT, read_write, image2D, ray_radiance_img)
IMAGE(5, SFLOAT_16_16_16_16, read_write, image2D, hit_throughput_img)
SAMPLER(4, sampler2D, ray_data_tx)
SAMPLER(5, sampler2D, ray_time_tx)
SAMPLER(6, usampler2D, hit_identity_tx)
SAMPLER(7, sampler2D, hit_barycentric_tx)
SAMPLER(8, sampler2D, hit_world_position_tx)
SAMPLER(9, sampler2D, hit_emission_replay_tx)
PUSH_CONSTANT(float3, hit_proxy_emission)
/* 5.2 draw-manager resource IDs occupy slot 11. Do not alias that binding. */
STORAGE_BUF(12, read, HardwareHitEvalRecord, hit_eval_list_buf[])
ADDITIONAL_INFO(eevee_Uniform)
ADDITIONAL_INFO(eevee_PipelineConstants)
SAMPLER(HIZ_TEX_SLOT, sampler2D, hiz_tx)
ADDITIONAL_INFO(eevee_sampling_data)
/* Material BSDF LUT functions use the 5.2 BSL resource table. The legacy sampler-only
 * declaration leaves CREATE_INFO_UtilityTexture undefined and strips their function bodies. */
ADDITIONAL_INFO(UtilityTexture)
FRAGMENT_SOURCE("eevee_surf_hit_eval_frag.glsl")
GPU_SHADER_CREATE_END()
