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

GPU_SHADER_CREATE_INFO(eevee_geom_hit_mesh)
DEFINE("MAT_GEOM_MESH")
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
TYPEDEF_SOURCE("eevee_raytrace_shared.hh")
IMAGE(0, SFLOAT_16_16_16_16, read_write, image2D, hit_albedo_img)
IMAGE(1, SFLOAT_16_16_16_16, read_write, image2D, hit_material_img)
IMAGE(2, SFLOAT_16_16_16_16, read_write, image2D, hit_normal_img)
IMAGE(3, SFLOAT_16_16_16_16, read_write, image2D, hit_position_img)
IMAGE(4, RAYTRACE_RADIANCE_FORMAT, read_write, image2D, ray_radiance_img)
IMAGE(5, SFLOAT_16_16_16_16, read_write, image2D, hit_throughput_img)
SAMPLER(5, sampler2D, ray_time_tx)
SAMPLER(6, usampler2D, hit_identity_tx)
SAMPLER(7, sampler2D, hit_barycentric_tx)
SAMPLER(8, sampler2D, hit_world_position_tx)
SAMPLER(9, sampler2D, hit_emission_replay_tx)
PUSH_CONSTANT(float3, hit_proxy_emission)
SAMPLER(HIZ_TEX_SLOT, sampler2D, hiz_tx)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_surf_shadow_tint)
DEFINE("MAT_HIT_EVAL")
DEFINE("MAT_SHADOW_TINT")
TYPEDEF_SOURCE("eevee_raytrace_shared.hh")
STORAGE_BUF(13, read_write, HardwareShadowTintHit, shadow_tint_hits[])
STORAGE_BUF(14, read, uint2, shadow_tint_entries[])
STORAGE_BUF(15, read, HardwareShadowTintRay, shadow_tint_rays[])
GPU_SHADER_CREATE_END()
