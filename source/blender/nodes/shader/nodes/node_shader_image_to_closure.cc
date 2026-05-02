/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "RNA_access.hh"

#include "DNA_node_types.h"

#include "UI_interface_layout.hh"

namespace blender {

namespace nodes::node_shader_image_to_closure_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Closure>("Closure");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "image", ui::ITEM_R_SPLIT_EMPTY_NAME, "Image", ICON_NONE);

  bNode *node = ptr->data_as<bNode>();
  if (node->id == nullptr) {
    ui::Layout &settings = layout.column(false);
    settings.enabled_set(false);
    settings.prop(ptr, "texture_type", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    settings.prop(ptr, "interpolation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    settings.prop(ptr, "extension", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    return;
  }

  layout.prop(ptr, "texture_type", ui::ITEM_R_SPLIT_EMPTY_NAME, "Texture Type", ICON_NONE);
  if (RNA_enum_get(ptr, "texture_type") ==
      IMA_IMAGE_TO_CLOSURE_TEXTURE_3D_LUT_STRIP)
  {
    layout.prop(ptr, "texture_size_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, "Size Mode", ICON_NONE);
    if (RNA_enum_get(ptr, "texture_size_mode") == IMA_IMAGE_TO_CLOSURE_3D_LUT_SIZE_MANUAL) {
      layout.prop(ptr, "texture_width", ui::ITEM_R_SPLIT_EMPTY_NAME, "Width", ICON_NONE);
      layout.prop(ptr, "texture_height", ui::ITEM_R_SPLIT_EMPTY_NAME, "Height", ICON_NONE);
      layout.prop(ptr, "texture_depth", ui::ITEM_R_SPLIT_EMPTY_NAME, "Depth", ICON_NONE);
    }
  }
  layout.prop(ptr, "colorspace", ui::ITEM_R_SPLIT_EMPTY_NAME, "Color Space", ICON_NONE);
  layout.prop(ptr, "interpolation", ui::ITEM_R_SPLIT_EMPTY_NAME, "Interpolation", ICON_NONE);
  layout.prop(ptr, "extension", ui::ITEM_R_SPLIT_EMPTY_NAME, "Extension", ICON_NONE);
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
  ntype.draw_buttons = file_ns::node_layout;
  ntype.initfunc = file_ns::node_init;
  bke::node_type_storage(
      ntype, "NodeShaderImageToClosure", node_free_standard_storage, node_copy_standard_storage);
  ntype.add_ui_poll = object_filter_or_npr_eevee_shader_nodes_poll;

  bke::node_register_type(ntype);
}

}  // namespace blender
