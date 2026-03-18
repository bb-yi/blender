/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_npr_image_sample_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Image>("Image").hide_value();
  b.add_input<decl::Vector>("Offset").hide_value();
  b.add_output<decl::Color>("Color");
}

static void node_shader_buts(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr,
              "offset_type",
              ui::ITEM_R_SPLIT_EMPTY_NAME | ui::ITEM_R_EXPAND,
              std::nullopt,
              ICON_NONE);
}

static int node_shader_gpu_npr_image_sample(GPUMaterial *mat,
                                            bNode *node,
                                            bNodeExecData * /*execdata*/,
                                            GPUNodeStack *in,
                                            GPUNodeStack *out)
{
  return GPU_stack_link(
      mat, node, node->custom1 ? "npr_image_sample_texel" : "npr_image_sample_view", in, out);
}

}  // namespace nodes::node_shader_npr_image_sample_cc

void register_node_type_sh_npr_image_sample()
{
  namespace file_ns = nodes::node_shader_npr_image_sample_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeNPR_ImageSample", SH_NODE_NPR_IMAGE_SAMPLE);
  ntype.enum_name_legacy = "NPR_IMAGE_SAMPLE";
  ntype.ui_name = "Image Sample";
  ntype.ui_description = "Sample a TextureHandle within the NPR shader tree";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts;
  ntype.add_ui_poll = npr_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_npr_image_sample;

  bke::node_register_type(ntype);
}

}  // namespace blender
