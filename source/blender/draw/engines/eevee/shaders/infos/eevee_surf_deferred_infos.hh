/* SPDX-FileCopyrightText: 2023 Blender Authors
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
#  include "eevee_sampling_infos.hh"
#  include "eevee_shadow_infos.hh"
#  include "eevee_shadow_shared.hh"
#  include "eevee_uniform_infos.hh"
#  include "eevee_volume_infos.hh"
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

/* -------------------------------------------------------------------- */
/** \name Surface
 * \{ */

GPU_SHADER_CREATE_INFO(eevee_surf_deferred_base)
DEFINE("MAT_DEFERRED")
DEFINE("GBUFFER_WRITE")
TYPEDEF_SOURCE("eevee_render_texture_shared.hh")
/* NOTE: This removes the possibility of using gl_FragDepth. */
EARLY_FRAGMENT_TEST(true)
/* Direct output. (Emissive, Holdout) */
FRAGMENT_OUT(0, float4, out_radiance)
FRAGMENT_OUT_ROG(1, uint, out_gbuf_header, DEFERRED_GBUFFER_ROG_ID)
FRAGMENT_OUT(2, float2, out_gbuf_normal)
FRAGMENT_OUT(3, float4, out_gbuf_closure1)
FRAGMENT_OUT(4, float4, out_gbuf_closure2)
/* Everything is stored inside a two layered target, one for each format. This is to fit the
 * limitation of the number of images we can bind on a single shader. */
IMAGE_FREQ(GBUF_CLOSURE_SLOT, UNORM_10_10_10_2, write, image2DArray, out_gbuf_closure_img, PASS)
IMAGE_FREQ(GBUF_NORMAL_SLOT, UNORM_16_16, write, image2DArray, out_gbuf_normal_img, PASS)
/* Storage for additional infos that are shared across closures. */
IMAGE_FREQ(GBUF_HEADER_SLOT, UINT_32, write, uimage2DArray, out_gbuf_header_img, PASS)
/* Added at runtime because of test shaders not having `node_tree`. */
// ADDITIONAL_INFO(eevee_render_pass_out)
// ADDITIONAL_INFO(eevee_cryptomatte_out)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_hiz_data)
ADDITIONAL_INFO(eevee_render_texture_data)
ADDITIONAL_INFO(draw_view_culling)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_surf_deferred)
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
FRAGMENT_SOURCE("eevee_surf_deferred_frag.glsl")
ADDITIONAL_INFO(eevee_surf_deferred_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_surf_npr)
DEFINE("NPR_SHADER")
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
TYPEDEF_SOURCE("eevee_render_texture_shared.hh")
EARLY_FRAGMENT_TEST(true)
FRAGMENT_OUT(0, float4, out_radiance)
SAMPLER(NPR_RADIANCE_TEX_SLOT, sampler2D, radiance_tx)
SAMPLER(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 0, usampler2D, direct_radiance_1_tx)
SAMPLER(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 1, usampler2D, direct_radiance_2_tx)
SAMPLER(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 2, usampler2D, direct_radiance_3_tx)
SAMPLER(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 0, sampler2D, indirect_radiance_1_tx)
SAMPLER(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 1, sampler2D, indirect_radiance_2_tx)
SAMPLER(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 2, sampler2D, indirect_radiance_3_tx)
SAMPLER(BACK_HIZ_TX_SLOT, sampler2D, hiz_back_tx)
SAMPLER(BACK_RADIANCE_TX_SLOT, sampler2D, radiance_back_tx)
PUSH_CONSTANT(bool, use_split_radiance)
ADDITIONAL_INFO(eevee_gbuffer_data)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(eevee_sampling_data)
ADDITIONAL_INFO(eevee_hiz_data)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_shadow_data)
ADDITIONAL_INFO(eevee_render_texture_data)
ADDITIONAL_INFO(draw_view_culling)
ADDITIONAL_INFO(eevee_render_pass_inout)
FRAGMENT_SOURCE("eevee_surf_deferred_npr_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_renderpass_clear)
FRAGMENT_OUT(0, float4, out_background)
FRAGMENT_SOURCE("eevee_renderpass_clear_frag.glsl")
ADDITIONAL_INFO(eevee_fullscreen)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_render_pass_out)
ADDITIONAL_INFO(eevee_cryptomatte_out)
TYPEDEF_SOURCE("eevee_defines.hh")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
