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

/** Settings of a Tessellation Control node. */
struct ShaderTessellationSettings {
  /** #NodeShaderTessellationMode. */
  int mode;
  /** #NodeShaderTessellationNormal. */
  int normal_mode;
  /** Subdivision level range. Every edge is split into a number of segments in this range. */
  int min_level;
  int max_level;
  /** Camera distance range over which the level decreases from `max_level` to `min_level`. */
  float near_distance;
  float far_distance;
  /** Target edge length in pixels for #SHD_TESSELLATION_SCREEN_EDGE. */
  float edge_length;
  /** Phong tessellation factor. */
  float phong;
  /** Bound on the vertex displacement (#Material::inflate_bounds), used for culling.
   * Negative when the material displaces vertices without a declared bound. */
  float max_displacement;
  /** Whether the material renders front faces only. */
  bool backface_cull;
};

/** Maximum subdivision level. Limited by the primitive expansion of the draw manager. */
#define SHD_TESSELLATION_LEVEL_MAX 15

ShaderTessellationSettings node_shader_tessellation_settings_get(const bNode &node);
/**
 * Return the Tessellation Control node linked to the tessellation input of the active EEVEE
 * material output, or nullptr if the material is not tessellated.
 */
const bNode *ntreeShaderTessellationNode(bNodeTree *ntree);
/**
 * Primitive expansion level of a fully subdivided triangle: the resolution of its lattice.
 * Equal to `max_level` rounded up to an odd number for the view dependent modes, which use
 * fractional-odd partitioning.
 */
int node_shader_tessellation_expansion(const ShaderTessellationSettings &settings);
/**
 * Pack the settings for the shader. Must match `eevee_geom_mesh_tess_lib.glsl`.
 * Unpacked by the shader, the levels and mode being exact integers.
 */
void node_shader_tessellation_settings_pack(const ShaderTessellationSettings &settings,
                                            float r_packed[4]);

struct bNodeTree *npr_tree_get(struct bNodeTree *ntree);
struct bNodeTree *npr_tree_get_from_mat(struct Material *material);
struct bNodeTree *ntreeGPUNPRNodes(struct bNodeTree *material_tree, struct GPUMaterial *mat);
void ntreeGPULightShaderNodes(struct bNodeTree *localtree, struct GPUMaterial *mat);

/**
 * This one needs to work on a local tree.
 */
void ntreeGPUMaterialNodesPreprocess(struct bNodeTree *localtree, struct GPUMaterial *mat);
void ntreeGPUMaterialNodes(struct bNodeTree *localtree,
                          struct GPUMaterial *mat,
                          bool preprocessed = false);

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
void node_shader_glsl_function_tag_text_users_dirty(Main &bmain, const bNode &node);
bool node_shader_glsl_function_refresh_text_users(Main &bmain,
                                                  bNodeTree &ntree,
                                                  bNode &node,
                                                  std::string &r_error);
bool node_shader_glsl_function_reset_defaults(bNode &node, std::string &r_error);

}  // namespace blender
