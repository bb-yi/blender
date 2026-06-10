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

#define EEVEE_BAKE_PRIMITIVE_TEX_SLOT FILTER_AOV_VALUE_TEX_SLOT

#ifdef GLSL_CPP_STUBS
#  define MAT_TRANSPARENT
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_surf_bake_color)
DEFINE("MAT_SURFACE_CULL")
DEFINE("MAT_BAKE_COLOR")
DEFINE("EEVEE_SAMPLING_DATA")
/* This pass still uses the legacy bake mesh create-info and GLSL entrypoint, but it calls 5.2 BSL
 * lighting helpers. Expose the equivalent resource-table gates without redeclaring the resources. */
DEFINE("CREATE_INFO_draw_View")
DEFINE("CREATE_INFO_draw_Infos")
DEFINE("CREATE_INFO_eevee_Uniform")
DEFINE("CREATE_INFO_UtilityTexture")
DEFINE("CREATE_INFO_eevee_LightRenderData")
DEFINE("CREATE_INFO_eevee_LightShaderEvalData")
DEFINE("CREATE_INFO_eevee_ShadowRenderData")
DEFINE("CREATE_INFO_eevee_LightEvalData")
DEFINE("CREATE_INFO_eevee_LightEvalIterator")
DEFINE("LIGHT_SHADER_TEXTURE_EVAL")
DEFINE("LIGHT_ITER_FORCE_NO_CULLING")
COMPILATION_CONSTANT(bool, light_iter_force_no_culling, true)
BUILTINS(BuiltinBits::NO_PREPROCESSOR)
PUSH_CONSTANT(int, surface_cull_mode)
FRAGMENT_OUT(0, float4, out_color)
SAMPLER(EEVEE_BAKE_PRIMITIVE_TEX_SLOT, isampler2D, bake_primitive_tx)
SAMPLER(LIGHT_SHADER_TEX_SLOT, sampler2DArray, light_shader_tx)
STORAGE_BUF(LIGHT_SHADER_INDEX_BUF_SLOT, read, int, light_shader_index_buf[])
STORAGE_BUF(LIGHT_SHADER_UNIFORM_BUF_SLOT, read, float4, light_shader_uniform_buf[])
UNIFORM_BUF(PIPELINE_BUF_SLOT, PipelineInfoData, pipeline_buf)
UNIFORM_BUF(RAYTRACE_BUF_SLOT, RayTraceData, raytrace_buf)
ADDITIONAL_INFO(eevee_global_ubo)
ADDITIONAL_INFO(eevee_utility_texture)
ADDITIONAL_INFO(eevee_light_data)
ADDITIONAL_INFO(eevee_shadow_data)
FRAGMENT_SOURCE("eevee_surf_bake_color_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_bake_light_shader_surface)
FRAGMENT_OUT(0, float4, out_position)
FRAGMENT_OUT(1, float4, out_normal)
SAMPLER(EEVEE_BAKE_PRIMITIVE_TEX_SLOT, isampler2D, bake_primitive_tx)
FRAGMENT_SOURCE("eevee_bake_light_shader_surface_frag.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_bake_light_shader_surface_mesh)
ADDITIONAL_INFO(eevee_geom_bake_mesh_basic)
ADDITIONAL_INFO(eevee_bake_light_shader_surface)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
