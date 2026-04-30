/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

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

  const bNode &node = *ptr->data_as<bNode>();
  if (node.id == nullptr) {
    ui::Layout &settings = layout.column(false);
    settings.enabled_set(false);
    settings.prop(ptr, "texture_type", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    settings.prop(ptr, "interpolation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    settings.prop(ptr, "extension", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    return;
  }

  PointerRNA image_ptr = RNA_pointer_create_discrete(node.id, RNA_Image, node.id);
  layout.prop(&image_ptr, "glsl_closure_texture_type", ui::ITEM_R_SPLIT_EMPTY_NAME, "Texture Type", ICON_NONE);
  if (RNA_enum_get(&image_ptr, "glsl_closure_texture_type") ==
      IMA_IMAGE_TO_CLOSURE_TEXTURE_3D_LUT_STRIP)
  {
    layout.prop(
        &image_ptr, "glsl_closure_size_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, "Size Mode", ICON_NONE);
    if (RNA_enum_get(&image_ptr, "glsl_closure_size_mode") ==
        IMA_IMAGE_TO_CLOSURE_3D_LUT_SIZE_MANUAL)
    {
      layout.prop(
          &image_ptr, "glsl_closure_width", ui::ITEM_R_SPLIT_EMPTY_NAME, "Width", ICON_NONE);
      layout.prop(
          &image_ptr, "glsl_closure_height", ui::ITEM_R_SPLIT_EMPTY_NAME, "Height", ICON_NONE);
      layout.prop(
          &image_ptr, "glsl_closure_depth", ui::ITEM_R_SPLIT_EMPTY_NAME, "Depth", ICON_NONE);
    }
  }
  layout.prop(
      &image_ptr, "glsl_closure_interpolation", ui::ITEM_R_SPLIT_EMPTY_NAME, "Interpolation", ICON_NONE);
  layout.prop(&image_ptr, "glsl_closure_extension", ui::ITEM_R_SPLIT_EMPTY_NAME, "Extension", ICON_NONE);
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
  ntype.add_ui_poll = object_filter_or_npr_eevee_shader_nodes_poll;

  bke::node_register_type(ntype);
}

}  // namespace blender
