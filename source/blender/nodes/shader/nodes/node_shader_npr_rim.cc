/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"
#include "DNA_material_types.h"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {
namespace nodes::node_shader_npr_rim_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  const bool depth = node && node->custom1 == 1;
  b.add_input<decl::Color>("Color"_ustr).default_value({1, 1, 1, 1});
  b.add_input<decl::Float>("Strength"_ustr).default_value(1).min(0).max(1000);
  b.add_input<decl::Vector>("Normal"_ustr).hide_value().available(!depth);
  b.add_input<decl::Float>("Angle"_ustr).default_value(0).min(-360).max(360)
      .description("Screen-space rotation in degrees");
  b.add_input<decl::Float>("Length"_ustr).default_value(1).min(0).max(1);
  b.add_input<decl::Float>("Length Falloff"_ustr).default_value(.1f).min(0).max(1);
  b.add_input<decl::Float>("Thickness"_ustr).default_value(.1f).min(0).max(1).available(!depth);
  b.add_input<decl::Float>("Thickness Falloff"_ustr).default_value(.05f).min(0).max(1).available(!depth);
  b.add_input<decl::Float>("Width (Pixels)"_ustr).default_value(4).min(0).max(256).available(depth);
  b.add_input<decl::Float>("Depth Threshold"_ustr).default_value(.05f).min(0).max(1000).available(depth);
  b.add_input<decl::Float>("Softness"_ustr).default_value(.5f).min(0).max(1).available(depth);
  b.add_input<decl::Float>("Samples"_ustr).default_value(8).min(1).max(64).available(depth);
  b.add_input<decl::Float>("Light Bias"_ustr).default_value(0).min(-1).max(1);
  b.add_input<decl::Float>("Light Factor"_ustr).default_value(1).min(0).max(1)
      .description("Lighting coordinate used by Light Bias; connect a lighting factor to modulate the rim");
  b.add_input<decl::Float>("Mask"_ustr).default_value(1).min(0).max(1);
  b.add_input<decl::Float>("Alpha"_ustr).default_value(1).min(0).max(1).available(depth)
      .description("Match the surface opacity; fractional alpha disables depth rim");
  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Float>("Factor"_ustr);
}

static void node_layout(ui::Layout &layout, bContext *, PointerRNA *ptr)
{
  layout.prop(ptr, "rim_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static int node_gpu(GPUMaterial *mat, bNode *node, bNodeExecData *, GPUNodeStack *in, GPUNodeStack *out)
{
  if (!in[2].link) {
    GPU_link(mat, "world_normals_get", &in[2].link);
  }
  const Material *material = GPU_material_get_material(mat);
  float supported = !material || material->surface_render_method != MA_SURFACE_METHOD_FORWARD;
  if (node->custom1 == 1 && supported) {
    GPU_material_hiz_data_set(mat);
  }
  float mode = float(node->custom1);
  return GPU_stack_link(mat, node, "node_npr_rim", in, out,
                        GPU_constant(&mode), GPU_constant(&supported));
}
}  // namespace nodes::node_shader_npr_rim_cc

void register_node_type_sh_npr_rim()
{
  namespace file_ns = nodes::node_shader_npr_rim_cc;
  static bke::bNodeType ntype;
  sh_node_type_base(&ntype, "ShaderNodeNPRRim"_ustr, SH_NODE_NPR_RIM);
  ntype.ui_name = "NPR Rim Light";
  ntype.ui_description = "Standalone Fresnel or screen-depth rim color. Depth requires an opaque Eevee deferred surface";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_gpu;
  bke::node_register_type(ntype);
}
}  // namespace blender
