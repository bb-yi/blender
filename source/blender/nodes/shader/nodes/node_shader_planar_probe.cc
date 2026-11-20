/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "BLI_math_bits.hh"

#include "BKE_lib_id.hh"

#include "DNA_material_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_planar_probe_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Position"_ustr)
      .hide_value()
      .description("World-space sampling position; defaults to the current surface position");
  b.add_input<decl::Vector>("Normal"_ustr)
      .hide_value()
      .description("World-space reflection normal; defaults to the current shading normal");
  b.add_input<decl::Vector>("UV"_ustr).hide_value().description(
      "Public image coordinates with a bottom-left origin; unconnected uses planar projection");
  b.add_output<decl::Color>("Color"_ustr)
      .description(
          "Captured scene-linear HDR color, without the receiving material's lighting response");
  b.add_output<decl::Float>("Depth"_ustr)
      .description("Positive linear Z depth in the reflected camera, in scene units; "
                   "zero without geometry depth");
  b.add_output<decl::Vector>("UV"_ustr).description(
      "Projected or explicit public UV; can be transformed and connected to another Planar Probe");
  b.add_output<decl::Float>("Is Valid"_ustr)
      .description(
          "One when this view has a completed capture and the sampling coordinates are valid");
}

static void node_draw(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "selection", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  if (static_cast<bNode *>(ptr->data)->custom1 == 1) {
    layout.prop(ptr, "probe_object", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
}

static int node_gpu(GPUMaterial *mat,
                    bNode *node,
                    bNodeExecData * /*execdata*/,
                    GPUNodeStack *in,
                    GPUNodeStack *out)
{
  const Material *material = GPU_material_get_material(mat);
  const float supported = material && material->eevee_domain == MA_EEVEE_DOMAIN_SURFACE;
  if (supported != 0.0f) {
    GPU_material_planar_probe_used_set(mat);
  }
  /* The live capture table validates object type, visibility and view membership. */
  ID *original = node->id ? (node->id->orig_id ? node->id->orig_id : node->id) : nullptr;
  uint32_t uid = 0;
  if (supported != 0.0f && node->custom1 == 1 && original != nullptr) {
    if (!(original->tag & ID_TAG_TEMP_MAIN)) {
      if (original->session_uid == 0) {
        BKE_lib_libblock_session_uid_ensure(original);
      }
      uid = original->session_uid;
    }
  }
  const float object_uid = uint_as_float(uid);
  const float selection = float(node->custom1);
  const float explicit_position = in[0].link != nullptr;
  const float explicit_normal = in[1].link != nullptr;
  const float explicit_uv = in[2].link != nullptr;
  return GPU_stack_link(mat,
                        node,
                        "node_planar_probe",
                        in,
                        out,
                        GPU_constant(&selection),
                        GPU_constant(&object_uid),
                        GPU_constant(&explicit_position),
                        GPU_constant(&explicit_normal),
                        GPU_constant(&explicit_uv),
                        GPU_constant(&supported),
                        GPU_kernel_globals(),
                        GPU_shading_data());
}

}  // namespace nodes::node_shader_planar_probe_cc

void register_node_type_sh_planar_probe()
{
  namespace file_ns = blender::nodes::node_shader_planar_probe_cc;
  static blender::bke::bNodeType ntype;
  sh_node_type_base(&ntype, "ShaderNodePlanarProbe"_ustr, SH_NODE_PLANAR_PROBE);
  ntype.ui_name = "Planar Probe";
  ntype.ui_description =
      "Sample an Eevee planar reflection capture's color, depth and image coordinates";
  ntype.enum_name_legacy = "PLANAR_PROBE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_draw;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_gpu;
  ntype.gpu_uses_referenced_object_data = true;
  blender::bke::node_register_type(ntype);
}

}  // namespace blender
