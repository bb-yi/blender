/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "eevee_nodetree_type_lib.glsl"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_outline_shell_self_occlusion)
SAMPLER(OUTLINE_SHELL_DEPTH_TEX_SLOT, sampler2DArray, outline_shell_depth_tx)
STORAGE_BUF(OUTLINE_SHELL_LAYER_BUF_SLOT, read, int, outline_shell_layer_buf[])
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_nodetree)
UNIFORM_BUF(0 /*GPU_NODE_TREE_UBO_SLOT*/, NodeTree, node_tree)
GPU_SHADER_CREATE_END()
