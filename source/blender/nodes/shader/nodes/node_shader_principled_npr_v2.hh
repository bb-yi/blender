/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender {
struct GPUMaterial;
struct GPUNodeStack;
struct bNode;
namespace nodes::node_shader_principled_npr_cc {
int node_gpu_v2(GPUMaterial *material, bNode *node, GPUNodeStack *inputs, GPUNodeStack *outputs);
}
}  // namespace blender
