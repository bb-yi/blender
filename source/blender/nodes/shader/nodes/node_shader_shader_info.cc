/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_shader_info_cc {

static constexpr int stable_shadow_sample_default = 8;
static constexpr int stable_shadow_sample_fallback = 8;
static constexpr int stable_shadow_sample_max = 32;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("World Position"_ustr).hide_value();
  b.add_input<decl::Vector>("Normal"_ustr).hide_value();
  b.add_input<decl::Float>("Exponent"_ustr).default_value(16.0f).min(1.0f).max(512.0f);
  b.add_input<decl::Float>("PCF Radius"_ustr)
      .default_value(-1.0f)
      .min(-1.0f)
      .max(64.0f)
      .description("Filter radius in shadow-map texels. For PCSS and VSSM this limits the "
                   "automatic penumbra radius. Negative uses the light filter or automatic "
                   "contact hardening; zero disables the material filter");
  b.add_input<decl::Float>("Max Penumbra"_ustr)
      .default_value(0.25f).min(0.0f).max(10000.0f)
      .description("Maximum contact-hardening filter radius in world units");

  b.add_output<decl::Color>("Diffuse Shading"_ustr);
  b.add_output<decl::Float>("Shadow"_ustr);
  b.add_output<decl::Color>("Ambient Lighting"_ustr);
  b.add_output<decl::Float>("Half-Lambert Factor"_ustr);
  b.add_output<decl::Float>("Blinn-Phong Factor"_ustr);
  b.add_output<decl::Float>("Self Shadow"_ustr);
  b.add_output<decl::Float>("Cast Shadow"_ustr);
}

static void node_shader_init_shader_info(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = MEM_new<NodeShaderShaderInfo>("NodeShaderShaderInfo");
  node->custom1 = SHD_SHADER_INFO_SHADOW_TEMPORAL;
  node->custom2 = stable_shadow_sample_default;
}

static void draw_shadow_mode_menu(bContext *, ui::Layout *layout, void *)
{
  const PointerRNA *menu_node = layout->context_ptr_get("shader_info_shadow_node", nullptr);
  if (!menu_node) {
    return;
  }
  PointerRNA node = *menu_node;
  for (const char *mode : {"TEMPORAL", "SOFT_FILTERED", "PCSS"}) {
    layout->prop_enum(&node, "shadow_mode", mode, std::nullopt, ICON_NONE);
  }
  layout->separator();
  layout->prop_enum(&node, "shadow_mode", "PCF", std::nullopt, ICON_NONE);
}

static void node_shader_buts(ui::Layout &layout, bContext *context, PointerRNA *ptr)
{
  PropertyRNA *mode_prop = RNA_struct_find_property(ptr, "shadow_mode");
  const int shadow_mode = RNA_property_enum_get(ptr, mode_prop);
  const char *mode_label = nullptr;
  RNA_property_enum_name_gettexted(context, ptr, mode_prop, shadow_mode, &mode_label);

  ui::Layout &mode_row = layout.split(0.32f, true);
  mode_row.label(RNA_property_ui_name(mode_prop, ptr), ICON_NONE);
  mode_row.context_ptr_set("shader_info_shadow_node", ptr);
  mode_row.menu_fn(mode_label ? mode_label : "", ICON_DOWNARROW_HLT, draw_shadow_mode_menu, nullptr);

  if (shadow_mode == SHD_SHADER_INFO_SHADOW_SOFT_FILTERED ||
      shadow_mode >= SHD_SHADER_INFO_SHADOW_PCF)
  {
    layout.prop(ptr, "stable_shadow_samples", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "lightgroup_id", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  bke::node_set_socket_availability(
      *ntree,
      *bke::node_find_socket(*node, SOCK_IN, "PCF Radius"_ustr),
      node->custom1 == SHD_SHADER_INFO_SHADOW_TEMPORAL ||
          (node->custom1 >= SHD_SHADER_INFO_SHADOW_PCF &&
           node->custom1 <= SHD_SHADER_INFO_SHADOW_MSM));
  bke::node_set_socket_availability(
      *ntree,
      *bke::node_find_socket(*node, SOCK_IN, "Max Penumbra"_ustr),
      node->custom1 == SHD_SHADER_INFO_SHADOW_CHS);
}

static int node_shader_gpu_shader_info(GPUMaterial *mat,
                                       bNode *node,
                                       bNodeExecData * /*execdata*/,
                                       GPUNodeStack *in,
                                       GPUNodeStack *out)
{
  const bool default_surface = !in[0].link && !in[1].link;
  if (!in[0].link) {
    GPU_link(mat, "world_position_get", GPU_shading_data(), &in[0].link);
  }
  if (!in[1].link) {
    GPU_link(mat, "world_normals_get", GPU_shading_data(), &in[1].link);
  }

  const float shadow_mode = float(node->custom1);
  const int shadow_sample_count = (node->custom2 > 0) ?
                                      min_ii(node->custom2, stable_shadow_sample_max) :
                                      stable_shadow_sample_fallback;
  const float stable_shadow_samples = float(shadow_sample_count);
  NodeShaderShaderInfo *storage = static_cast<NodeShaderShaderInfo *>(node->storage);
  const float lightgroup_id_value = float(storage != nullptr ? max_ii(storage->lightgroup_id, 0) : 0);
  const bool shadow_outputs = out[1].hasoutput || out[5].hasoutput || out[6].hasoutput;
  const bool cache_eligible = default_surface && shadow_outputs &&
                              node->custom1 == SHD_SHADER_INFO_SHADOW_SOFT_FILTERED;
  const float cache_eligible_value = cache_eligible ? 1.0f : 0.0f;

  /* Shader Info evaluates the same per-light custom color/attenuation cache used by GLSL
   * Function's GLSLLight helpers. Keep the existing light-access create-info path enabled so
   * ordinary Deferred materials receive LIGHT_SHADER_TEXTURE_EVAL and its front-layer resources. */
  GPU_material_flag_set(mat,
                        GPU_MATFLAG_DIFFUSE | GPU_MATFLAG_SHADER_INFO |
                            GPU_MATFLAG_GLSL_LIGHT_ACCESS);
  if (out[5].hasoutput || out[6].hasoutput) {
    GPU_material_shader_info_shadow_classification_set(mat);
  }
  if (shadow_outputs && node->custom1 >= SHD_SHADER_INFO_SHADOW_HARD) {
    GPU_material_flag_set(mat, GPU_MATFLAG_SHADOW_FILTER_METHODS);
  }
  if (shadow_outputs && node->custom1 >= SHD_SHADER_INFO_SHADOW_VSM &&
      node->custom1 <= SHD_SHADER_INFO_SHADOW_MSM)
  {
    GPU_material_flag_set(mat, GPU_MATFLAG_SHADOW_MOMENTS);
  }
  if (cache_eligible) {
    /* The cache reconstructs receiver position from the opaque prepass depth/ID textures. Keep
     * those resources limited to the one case that actually uses the cache. */
    GPU_material_flag_set(mat, GPU_MATFLAG_RAYCAST);
    GPU_material_hiz_data_set(mat);
    GPU_material_node_shadow_cache_set(mat, shadow_sample_count);
  }
  return GPU_stack_link(mat,
                        node,
                        "node_shader_info",
                        in,
                        out,
                        GPU_constant(&shadow_mode),
                        GPU_constant(&stable_shadow_samples),
                        GPU_constant(&lightgroup_id_value),
                        GPU_constant(&cache_eligible_value),
                        GPU_kernel_globals(),
                        GPU_shading_data());
}

}  // namespace nodes::node_shader_shader_info_cc

void register_node_type_sh_shader_info()
{
  namespace file_ns = nodes::node_shader_shader_info_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeShaderInfo"_ustr, SH_NODE_SHADER_INFO);
  ntype.ui_name = "Shader Info";
  ntype.ui_description =
      "Expose Eevee direct light, shadow mask, ambient probe, half-lambert, and Blinn-Phong "
      "information for the surface";
  ntype.enum_name_legacy = "SHADERINFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.initfunc = file_ns::node_shader_init_shader_info;
  ntype.draw_buttons = file_ns::node_shader_buts;
  ntype.declare = file_ns::node_declare;
  ntype.updatefunc = file_ns::node_update;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  bke::node_type_storage(
      ntype, "NodeShaderShaderInfo", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::node_shader_gpu_shader_info;

  bke::node_register_type(ntype);
}

}  // namespace blender
