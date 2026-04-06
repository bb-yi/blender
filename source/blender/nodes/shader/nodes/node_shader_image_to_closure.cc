/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"

namespace blender {

namespace nodes::node_shader_image_to_closure_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Image>("Image").hide_value();
  b.add_output<decl::Closure>("Closure");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  bNode &node = *static_cast<bNode *>(ptr->data);
  bNodeSocket &socket = node.input_socket(0);
  PointerRNA socket_ptr = RNA_pointer_create_discrete(ptr->owner_id, RNA_NodeSocket, &socket);
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(&socket_ptr, "default_value", ui::ITEM_R_SPLIT_EMPTY_NAME, socket.name, ICON_NONE);
  layout.prop(ptr, "interpolation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "extension", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = SHD_INTERP_LINEAR;
  node->custom2 = SHD_IMAGE_EXTENSION_REPEAT;
}

}  // namespace nodes::node_shader_image_to_closure_cc

void register_node_type_sh_image_to_closure()
{
  namespace file_ns = nodes::node_shader_image_to_closure_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeImageToClosure", SH_NODE_IMAGE_TO_CLOSURE);
  ntype.enum_name_legacy = "IMAGE_TO_CLOSURE";
  ntype.ui_name = "Image to Closure";
  ntype.ui_description = "Adapt an image into a closure-backed sample source";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.initfunc = file_ns::node_init;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;

  bke::node_register_type(ntype);
}

}  // namespace blender
