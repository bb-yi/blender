/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "DNA_node_types.h"

namespace blender {

namespace nodes::node_shader_image_to_closure_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Closure>("Closure");
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = MEM_new<NodeShaderImageToClosure>(__func__);
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
  ntype.initfunc = file_ns::node_init;
  bke::node_type_storage(
      ntype, "NodeShaderImageToClosure", node_free_standard_storage, node_copy_standard_storage);
  ntype.add_ui_poll = object_filter_or_npr_eevee_shader_nodes_poll;

  bke::node_register_type(ntype);
}

}  // namespace blender
