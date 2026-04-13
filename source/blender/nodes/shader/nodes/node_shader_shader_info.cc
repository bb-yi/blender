/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "BLI_hash.h"
#include "BLI_string.h"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_shader_info_cc {

static constexpr int stable_shadow_sample_default = 8;
static constexpr int stable_shadow_sample_fallback = 8;
static constexpr int stable_shadow_sample_max = 32;

static uint shader_info_lightgroup_hash_from_id(const int lightgroup_id)
{
  char lightgroup_name[16];
  BLI_snprintf(lightgroup_name, sizeof(lightgroup_name), "%d", max_ii(lightgroup_id, 0));
  return BLI_hash_string(lightgroup_name);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("World Position").hide_value();
  b.add_input<decl::Vector>("Normal").hide_value();
  b.add_input<decl::Float>("Exponent").default_value(16.0f).min(1.0f).max(512.0f);

  b.add_output<decl::Color>("Diffuse Shading");
  b.add_output<decl::Float>("Shadow");
  b.add_output<decl::Color>("Ambient Lighting");
  b.add_output<decl::Float>("Half-Lambert Factor");
  b.add_output<decl::Float>("Blinn-Phong Factor");
}

static void node_shader_init_shader_info(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = MEM_new<NodeShaderShaderInfo>("NodeShaderShaderInfo");
  node->custom1 = SHD_SHADER_INFO_SHADOW_TEMPORAL;
  node->custom2 = stable_shadow_sample_default;
}

static void node_shader_buts(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "shadow_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  const int shadow_mode = RNA_enum_get(ptr, "shadow_mode");
  if (shadow_mode == SHD_SHADER_INFO_SHADOW_SOFT_FILTERED) {
    layout.prop(ptr, "stable_shadow_samples", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "lightgroup_id", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static int node_shader_gpu_shader_info(GPUMaterial *mat,
                                       bNode *node,
                                       bNodeExecData * /*execdata*/,
                                       GPUNodeStack *in,
                                       GPUNodeStack *out)
{
  if (!in[0].link) {
    GPU_link(mat, "world_position_get", &in[0].link);
  }
  if (!in[1].link) {
    GPU_link(mat, "world_normals_get", &in[1].link);
  }

  const float shadow_mode = float(node->custom1);
  const int shadow_sample_count = (node->custom2 > 0) ?
                                      min_ii(node->custom2, stable_shadow_sample_max) :
                                      stable_shadow_sample_fallback;
  const float stable_shadow_samples = float(shadow_sample_count);
  NodeShaderShaderInfo *storage = static_cast<NodeShaderShaderInfo *>(node->storage);
  static int lightgroup_hash_fallback = 0;
  uint lightgroup_hash = shader_info_lightgroup_hash_from_id(0);
  if (storage != nullptr) {
    if (storage->lightgroup[0] != '\0') {
      lightgroup_hash = BLI_hash_string(storage->lightgroup);
    }
    else {
      lightgroup_hash = shader_info_lightgroup_hash_from_id(storage->lightgroup_id);
    }
    storage->_pad = int(lightgroup_hash);
  }
  else {
    lightgroup_hash_fallback = int(lightgroup_hash);
  }

  GPU_material_flag_set(mat, GPU_MATFLAG_DIFFUSE | GPU_MATFLAG_SHADER_INFO);
  if (node->custom1 == SHD_SHADER_INFO_SHADOW_SOFT_FILTERED) {
    GPU_material_flag_set(mat, GPU_MATFLAG_RAYCAST);
  }
  BLI_STATIC_ASSERT(sizeof(float) == sizeof(uint),
                    "GPUCodegen: Shader Info lightgroup hash needs float and uint to be the same size.");
  GPUNodeLink *lightgroup_hash_link = GPU_uniform(reinterpret_cast<float *>(
      storage != nullptr ? &storage->_pad : &lightgroup_hash_fallback));
  return GPU_stack_link(mat,
                        node,
                        "node_shader_info",
                        in,
                        out,
                        GPU_constant(&shadow_mode),
                        GPU_constant(&stable_shadow_samples),
                        lightgroup_hash_link);
}

}  // namespace nodes::node_shader_shader_info_cc

void register_node_type_sh_shader_info()
{
  namespace file_ns = nodes::node_shader_shader_info_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeShaderInfo", SH_NODE_SHADER_INFO);
  ntype.ui_name = "Shader Info";
  ntype.ui_description =
      "Expose Eevee direct light, shadow mask, ambient probe, half-lambert, and Blinn-Phong "
      "information for the surface";
  ntype.enum_name_legacy = "SHADERINFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.initfunc = file_ns::node_shader_init_shader_info;
  ntype.draw_buttons = file_ns::node_shader_buts;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  bke::node_type_storage(
      ntype, "NodeShaderShaderInfo", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::node_shader_gpu_shader_info;

  bke::node_register_type(ntype);
}

}  // namespace blender
