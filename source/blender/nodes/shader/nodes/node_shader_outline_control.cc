/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_material_types.h"

#include "BKE_context.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

namespace blender {

namespace nodes::node_shader_outline_control_cc {

static GPUNodeStack outline_control_default_input(const GPUType gpu_type,
                                                  const short socket_type,
                                                  const float value)
{
  GPUNodeStack stack = {};
  stack.type = gpu_type;
  stack.vec[0] = value;
  stack.vec[1] = value;
  stack.vec[2] = value;
  stack.vec[3] = 1.0f;
  stack.sockettype = socket_type;
  return stack;
}

static GPUNodeStack outline_control_input_or_default(const bNode &node,
                                                     GPUNodeStack *in,
                                                     const UString identifier,
                                                     const GPUNodeStack &default_input)
{
  const bNodeSocket *socket = node.input_by_identifier(identifier);
  if (socket == nullptr) {
    return default_input;
  }
  return in[socket->index()];
}

static void node_declare(NodeDeclarationBuilder &b)
{
  /* Enable custom socket order so collapsible panels and resource bindings stay stable. */
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  /* Line style. */
  b.add_input<decl::Color>("Line Color"_ustr).default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_input<decl::Float>("Line Alpha"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Controls the opacity of the outline");
  b.add_input<decl::Float>("Line Width"_ustr).default_value(2.0f).min(0.0f);

  /* Depth / silhouette edge settings. */
  PanelDeclarationBuilder &depth = b.add_panel("Depth"_ustr).default_closed(true);
  depth.add_input<decl::Float>("Depth Threshold"_ustr).default_value(0.1f).min(0.0f).max(1.0f);
  depth.add_input<decl::Float>("Depth Threshold Range"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Strength range above the depth threshold over which the line tapers from zero to "
          "Depth Edge Width. Zero = hard on/off");
  depth.add_input<decl::Float>("Depth Edge Width"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Width multiplier applied to depth/silhouette edges");

  /* Normal / crease edge settings. */
  PanelDeclarationBuilder &normal = b.add_panel("Normal"_ustr).default_closed(true);
  normal.add_input<decl::Float>("Normal Threshold"_ustr).default_value(0.5f).min(0.0f).max(1.0f);
  normal.add_input<decl::Float>("Normal Threshold Range"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Strength range above the normal threshold over which the line tapers from zero to "
          "Normal Edge Width. Zero = hard on/off");
  normal.add_input<decl::Float>("Normal Edge Width"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Width multiplier applied to normal/crease edges");

  /* ID-boundary edge settings. */
  PanelDeclarationBuilder &id = b.add_panel("ID"_ustr).default_closed(true);
  id.add_input<decl::Int>("Outline ID"_ustr).default_value(0).min(0).max(32767);
  id.add_input<decl::Bool>("ID Edge"_ustr)
      .default_value(true)
      .description("Draw outlines where the outline ID changes");
  id.add_input<decl::Float>("ID Edge Width"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Width multiplier applied to ID-boundary edges. Independent from Depth/Normal");

  b.add_input<decl::Bool>("Freestyle Edge"_ustr)
      .default_value(true)
      .description("Draw outlines on mesh edges marked as Freestyle edges");
}

static int node_shader_gpu_outline_control(GPUMaterial *mat,
                                           bNode *node,
                                           bNodeExecData * /*execdata*/,
                                           GPUNodeStack *in,
                                           GPUNodeStack *out)
{
  GPU_material_flag_set(mat, GPU_MATFLAG_OBJECT_INFO);
  GPUNodeLink *outlink = nullptr;

  GPUNodeStack outline_in[14] = {};
  outline_in[0] = outline_control_input_or_default(
      *node, in, "Line Color"_ustr, outline_control_default_input(GPU_VEC4, SOCK_RGBA, 0.0f));
  outline_in[1] = outline_control_input_or_default(
      *node, in, "Line Alpha"_ustr, outline_control_default_input(GPU_FLOAT, SOCK_FLOAT, 1.0f));
  outline_in[2] = outline_control_input_or_default(
      *node, in, "Line Width"_ustr, outline_control_default_input(GPU_FLOAT, SOCK_FLOAT, 2.0f));
  outline_in[3] = outline_control_input_or_default(
      *node, in, "Depth Threshold"_ustr, outline_control_default_input(GPU_FLOAT, SOCK_FLOAT, 0.1f));
  outline_in[4] = outline_control_input_or_default(*node,
                                                   in,
                                                   "Depth Threshold Range"_ustr,
                                                   outline_control_default_input(
                                                       GPU_FLOAT, SOCK_FLOAT, 0.0f));
  outline_in[5] = outline_control_input_or_default(
      *node, in, "Depth Edge Width"_ustr, outline_control_default_input(GPU_FLOAT, SOCK_FLOAT, 1.0f));
  outline_in[6] = outline_control_input_or_default(
      *node, in, "Normal Threshold"_ustr, outline_control_default_input(GPU_FLOAT, SOCK_FLOAT, 0.5f));
  outline_in[7] = outline_control_input_or_default(*node,
                                                   in,
                                                   "Normal Threshold Range"_ustr,
                                                   outline_control_default_input(
                                                       GPU_FLOAT, SOCK_FLOAT, 0.0f));
  outline_in[8] = outline_control_input_or_default(
      *node, in, "Normal Edge Width"_ustr, outline_control_default_input(GPU_FLOAT, SOCK_FLOAT, 1.0f));
  outline_in[9] = outline_control_input_or_default(
      *node, in, "Outline ID"_ustr, outline_control_default_input(GPU_FLOAT, SOCK_INT, 0.0f));
  outline_in[10] = outline_control_input_or_default(
      *node, in, "ID Edge"_ustr, outline_control_default_input(GPU_FLOAT, SOCK_BOOLEAN, 1.0f));
  outline_in[11] = outline_control_input_or_default(
      *node, in, "ID Edge Width"_ustr, outline_control_default_input(GPU_FLOAT, SOCK_FLOAT, 1.0f));
  outline_in[12] = outline_control_input_or_default(
      *node, in, "Freestyle Edge"_ustr, outline_control_default_input(GPU_FLOAT, SOCK_BOOLEAN, 1.0f));
  outline_in[13].end = true;

  GPU_stack_link(mat, node, "node_output_outline", outline_in, out, &outlink);
  GPU_material_add_output_link_outline(mat, outlink);
  return true;
}

static bool node_add_ui_poll(const bContext *C)
{
  return object_or_npr_eevee_shader_nodes_poll(C);
}

}  // namespace nodes::node_shader_outline_control_cc

void register_node_type_sh_outline_control()
{
  namespace file_ns = nodes::node_shader_outline_control_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeOutlineControl"_ustr, SH_NODE_OUTLINE_CONTROL);
  ntype.enum_name_legacy = "OUTLINE_CONTROL";
  ntype.ui_name = "Outline Control";
  ntype.ui_description = "Write Eevee outline parameters for the built-in screen-space outline pass";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = file_ns::node_add_ui_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_outline_control;
  ntype.no_muting = true;

  bke::node_register_type(ntype);
}

}  // namespace blender
