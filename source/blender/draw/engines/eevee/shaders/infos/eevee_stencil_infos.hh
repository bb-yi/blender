/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "eevee_fullscreen_infos.hh"
#  include "gpu_shader_compat.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_stencil_snapshot)
DO_STATIC_COMPILATION()
SAMPLER(0, usampler2D, stencil_source_tx)
FRAGMENT_OUT(0, uint, out_value)
ADDITIONAL_INFO(eevee_fullscreen)
FRAGMENT_SOURCE("eevee_stencil_snapshot_frag.glsl")
GPU_SHADER_CREATE_END()
