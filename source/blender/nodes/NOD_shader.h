/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include <string>

#include "BKE_node.hh"

namespace blender {

extern struct bke::bNodeTreeType *ntreeType_Shader;

void register_node_type_sh_custom_group(bke::bNodeType *ntype);

struct bNodeTreeExec *ntreeShaderBeginExecTree(struct bNodeTree *ntree);
void ntreeShaderEndExecTree(struct bNodeTreeExec *exec);

/**
 * Find an output node of the shader tree.
 *
 * \note it will only return output which is NOT in the group, which isn't how
 * render engines works but it's how the GPU shader compilation works. This we
 * can change in the future and make it a generic function, but for now it stays
 * private here.
 */
struct bNode *ntreeShaderOutputNode(struct bNodeTree *ntree, int target);

struct bNodeTree *npr_tree_get(struct bNodeTree *ntree);
struct bNodeTree *npr_tree_get_from_mat(struct Material *material);
struct bNodeTree *ntreeGPUNPRNodes(struct bNodeTree *material_tree, struct GPUMaterial *mat);
void ntreeGPULightShaderNodes(struct bNodeTree *localtree, struct GPUMaterial *mat);

/**
 * This one needs to work on a local tree.
 */
void ntreeGPUMaterialNodes(struct bNodeTree *localtree, struct GPUMaterial *mat);

bool node_shader_glsl_function_source_get(const bNode &node,
                                          std::string &r_source,
                                          std::string &r_error);
bool node_shader_glsl_function_code_source_ensure(Main &bmain,
                                                  bNode &node,
                                                  bool &r_changed,
                                                  std::string &r_error);
bool node_shader_glsl_function_edit_source_get(const bNode &node,
                                               std::string &r_source,
                                               std::string &r_error);
void node_shader_glsl_function_edit_source_set(bNode &node, const char *source);
void node_shader_glsl_function_edit_function_set(bNode &node, const char *function_name);
bool node_shader_glsl_function_source_dirty(const bNode &node);
void node_shader_glsl_function_discard_draft(bNode &node);
bool node_shader_glsl_function_apply_draft(Main &bmain,
                                           bNodeTree &ntree,
                                           bNode &node,
                                           std::string &r_error);
bool node_shader_glsl_function_reset_defaults(bNode &node, std::string &r_error);

}  // namespace blender
