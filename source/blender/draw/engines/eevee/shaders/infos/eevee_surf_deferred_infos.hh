/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_view_infos.hh"

#  include "eevee_common_infos.hh"
#  include "eevee_sampling_infos.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_surf_npr)
DEFINE("NPR_SHADER")
DEFINE("MAT_SURFACE_CULL")
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
TYPEDEF_SOURCE("eevee_render_texture_shared.hh")
EARLY_FRAGMENT_TEST(true)
PUSH_CONSTANT(int, surface_cull_mode)
FRAGMENT_OUT(0, float4, out_radiance)
SAMPLER(NPR_RADIANCE_TEX_SLOT, sampler2D, radiance_tx)
SAMPLER(LIGHT_SHADER_NPR_TEX_SLOT, sampler2DArray, light_shader_tx)
SAMPLER(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 0, usampler2D, direct_radiance_1_tx)
SAMPLER(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 1, usampler2D, direct_radiance_2_tx)
SAMPLER(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 2, usampler2D, direct_radiance_3_tx)
SAMPLER(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 0, sampler2D, indirect_radiance_1_tx)
SAMPLER(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 1, sampler2D, indirect_radiance_2_tx)
SAMPLER(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 2, sampler2D, indirect_radiance_3_tx)
STORAGE_BUF(LIGHT_SHADER_INDEX_BUF_SLOT, read, int, light_shader_index_buf[])
STORAGE_BUF(LIGHT_SHADER_UNIFORM_BUF_SLOT, read, float4, light_shader_uniform_buf[])
PUSH_CONSTANT(bool, use_split_radiance)
PUSH_CONSTANT(bool, use_radiance_input_for_combined)
ADDITIONAL_INFO(eevee_gbuffer_data)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_hiz_data)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_view_culling)
ADDITIONAL_INFO(eevee_render_pass_inout)
FRAGMENT_SOURCE("eevee_surf_deferred_npr_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_surf_npr_depth_offset)
DEFINE("NPR_SHADER")
DEFINE("MAT_SURFACE_CULL")
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
TYPEDEF_SOURCE("eevee_render_texture_shared.hh")
DEPTH_WRITE(DepthWrite::ANY)
PUSH_CONSTANT(int, surface_cull_mode)
FRAGMENT_OUT(0, float4, out_radiance)
SAMPLER(NPR_RADIANCE_TEX_SLOT, sampler2D, radiance_tx)
SAMPLER(LIGHT_SHADER_NPR_TEX_SLOT, sampler2DArray, light_shader_tx)
SAMPLER(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 0, usampler2D, direct_radiance_1_tx)
SAMPLER(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 1, usampler2D, direct_radiance_2_tx)
SAMPLER(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 2, usampler2D, direct_radiance_3_tx)
SAMPLER(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 0, sampler2D, indirect_radiance_1_tx)
SAMPLER(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 1, sampler2D, indirect_radiance_2_tx)
SAMPLER(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 2, sampler2D, indirect_radiance_3_tx)
STORAGE_BUF(LIGHT_SHADER_INDEX_BUF_SLOT, read, int, light_shader_index_buf[])
STORAGE_BUF(LIGHT_SHADER_UNIFORM_BUF_SLOT, read, float4, light_shader_uniform_buf[])
PUSH_CONSTANT(bool, use_split_radiance)
PUSH_CONSTANT(bool, use_radiance_input_for_combined)
ADDITIONAL_INFO(eevee_gbuffer_data)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_hiz_data)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_view_culling)
ADDITIONAL_INFO(eevee_render_pass_inout)
FRAGMENT_SOURCE("eevee_surf_deferred_npr_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_surf_npr_refraction_data)
SAMPLER(BACK_HIZ_TX_SLOT, sampler2D, hiz_back_tx)
SAMPLER(BACK_RADIANCE_TX_SLOT, sampler2D, radiance_back_tx)
GPU_SHADER_CREATE_END()
