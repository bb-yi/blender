/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "BLI_math_bits.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_light_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Float>("Power"_ustr);
  b.add_output<decl::Int>("Type"_ustr)
      .description("Light type index: 0 Point, 1 Sun, 2 Spot, 3 Area. Outputs -1 when no light is assigned");
  b.add_output<decl::Vector>("Position"_ustr)
      .description("World-space location of the selected light object");
  b.add_output<decl::Vector>("Direction"_ustr)
      .description("World-space emission direction");
  b.add_output<decl::Float>("Radius"_ustr)
      .description("Primary size parameter of the selected light");
  b.add_output<decl::Float>("Spot Size"_ustr)
      .description("Spot cone angle in radians");
  b.add_output<decl::Float>("Sun Angle"_ustr)
      .description("Sun angular diameter in radians");
  b.add_output<decl::Float>("Visible"_ustr)
      .description("Outputs 1 when the selected light object is not hidden, otherwise 0");
}

static void node_shader_buts_light_info(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "light_object", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static int node_shader_gpu_light_info(GPUMaterial *mat,
                                      bNode *node,
                                      bNodeExecData * /*execdata*/,
                                      GPUNodeStack *in,
                                      GPUNodeStack *out)
{
  static const float zero_uid = uint_as_float(0u);
  float uid_value = zero_uid;
  Object *light_object = reinterpret_cast<Object *>(node->id);
  const eGPUReferencedObjectDataFlag flags = eGPUReferencedObjectDataFlag(
      GPU_REFERENCED_OBJECT_DATA_TRANSFORM | GPU_REFERENCED_OBJECT_DATA_COLOR |
      GPU_REFERENCED_OBJECT_DATA_VISIBILITY | GPU_REFERENCED_OBJECT_DATA_TYPE |
      GPU_REFERENCED_OBJECT_DATA_LIGHT);
  uid_value = uint_as_float(GPU_material_referenced_object_ensure(mat, light_object, flags));

  return GPU_stack_link(mat,
                        node,
                        "node_light_info",
                        in,
                        out,
                        GPU_constant(&uid_value));
}

}  // namespace nodes::node_shader_light_info_cc

void register_node_type_sh_light_info()
{
  namespace file_ns = nodes::node_shader_light_info_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeLightInfo"_ustr, SH_NODE_LIGHT_INFO);
  ntype.ui_name = "Light Info";
  ntype.ui_description =
      "Read flat color, power, transform, visibility, and size values from a chosen light";
  ntype.enum_name_legacy = "LIGHTINFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_light_info;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_light_info;
  ntype.gpu_uses_referenced_object_data = true;

  bke::node_register_type(ntype);
}

}  // namespace blender
