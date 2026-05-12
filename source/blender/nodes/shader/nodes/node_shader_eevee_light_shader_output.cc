/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_eevee_light_shader_output_cc {

constexpr float default_range_scale = 1.0f;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_input<decl::Color>("Color").default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Float>("Intensity").default_value(1.0f).min(0.0f);
  b.add_input<decl::Float>("Attenuation").default_value(1.0f).min(0.0f);
  b.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
    layout.prop(ptr, "range_scale", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  });
}

static void node_shader_init_eevee_light_shader_output(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom3 = default_range_scale;
  node->flag |= NODE_OPTIONS;
}

static void node_shader_buts_eevee_light_shader_output(ui::Layout &layout,
                                                       bContext * /*C*/,
                                                       PointerRNA *ptr)
{
  layout.prop(ptr, "range_scale", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static int node_shader_gpu_eevee_light_shader_output(GPUMaterial *mat,
                                                     bNode *node,
                                                     bNodeExecData * /*execdata*/,
  GPUNodeStack *in,
  GPUNodeStack *out)
{
  GPUNodeLink *outlink_light_shader = nullptr;
  GPU_stack_link(mat, node, "node_output_eevee_light_shader", in, out, &outlink_light_shader);
  GPU_material_output_light_shader(mat, outlink_light_shader);
  return true;
}

}  // namespace nodes::node_shader_eevee_light_shader_output_cc

void register_node_type_sh_eevee_light_shader_output()
{
  namespace file_ns = nodes::node_shader_eevee_light_shader_output_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(
      &ntype, "ShaderNodeEeveeLightShaderOutput", SH_NODE_EEVEE_LIGHT_SHADER_OUTPUT);
  ntype.enum_name_legacy = "EEVEE_LIGHT_SHADER_OUTPUT";
  ntype.ui_name = "Light Shader Output";
  ntype.ui_description = "Output custom Eevee direct-light color and attenuation for a light";
  ntype.nclass = NODE_CLASS_OUTPUT;
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Large);
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_eevee_light_shader_output;
  ntype.draw_buttons_ex = file_ns::node_shader_buts_eevee_light_shader_output;
  ntype.initfunc = file_ns::node_shader_init_eevee_light_shader_output;
  ntype.add_ui_poll = light_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_eevee_light_shader_output;
  ntype.no_muting = true;

  bke::node_register_type(ntype);
}

}  // namespace blender
