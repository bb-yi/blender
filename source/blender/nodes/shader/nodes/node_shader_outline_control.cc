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

static void node_declare(NodeDeclarationBuilder &b)
{
  /* Enable custom socket order so that collapsible panels work. */
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  /* Top-level: line style. Always visible. */
  b.add_input<decl::Color>("Line Color").default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_input<decl::Float>("Line Alpha")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Controls the opacity of the outline");
  b.add_input<decl::Float>("Line Width").default_value(2.0f).min(0.0f);

  /* Depth / silhouette edge settings. */
  PanelDeclarationBuilder &depth = b.add_panel("Depth").default_closed(true);
  depth.add_input<decl::Float>("Depth Threshold").default_value(0.1f).min(0.0f).max(1.0f);
  depth.add_input<decl::Float>("Depth Threshold Range")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Strength range above the depth threshold over which the line tapers from zero to "
          "Depth Edge Width. Zero = hard on/off");
  depth.add_input<decl::Float>("Depth Edge Width")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Width multiplier applied to depth/silhouette edges");

  /* Normal / crease edge settings. */
  PanelDeclarationBuilder &normal = b.add_panel("Normal").default_closed(true);
  normal.add_input<decl::Float>("Normal Threshold").default_value(0.5f).min(0.0f).max(1.0f);
  normal.add_input<decl::Float>("Normal Threshold Range")
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Strength range above the normal threshold over which the line tapers from zero to "
          "Normal Edge Width. Zero = hard on/off");
  normal.add_input<decl::Float>("Normal Edge Width")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Width multiplier applied to normal/crease edges");

  /* ID-boundary edge settings. */
  PanelDeclarationBuilder &id = b.add_panel("ID").default_closed(true);
  id.add_input<decl::Int>("Outline ID").default_value(0).min(0).max(32767);
  id.add_input<decl::Bool>("ID Edge")
      .default_value(true)
      .description("Draw outlines where the outline ID changes");
  id.add_input<decl::Float>("ID Edge Width")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Width multiplier applied to ID-boundary edges. Independent from Depth/Normal");

  /* Freestyle sits at the top level (single socket, not worth a panel). */
  b.add_input<decl::Bool>("Freestyle Edge")
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
  GPU_stack_link(mat, node, "node_output_outline", in, out, &outlink);
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

  sh_node_type_base(&ntype, "ShaderNodeOutlineControl", SH_NODE_OUTLINE_CONTROL);
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
