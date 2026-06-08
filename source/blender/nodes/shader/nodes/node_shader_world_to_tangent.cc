/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "BKE_context.hh"

#include "DEG_depsgraph_query.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_world_to_tangent_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr)
      .default_value({0.0f, 0.0f, 1.0f})
      .min(-10000.0f)
      .max(10000.0f)
      .description("World-space vector to be transformed into tangent space");
  b.add_output<decl::Vector>("Vector"_ustr)
      .description("Input vector expressed in the local tangent-space basis");
}

static void node_shader_buts_world_to_tangent(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  PointerRNA obptr = CTX_data_pointer_get(C, "active_object");
  Object *object = static_cast<Object *>(obptr.data);

  if (object && object->type == OB_MESH) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);

    if (depsgraph) {
      Object *object_eval = DEG_get_evaluated(depsgraph, object);
      PointerRNA dataptr = RNA_id_pointer_create(object_eval->data);
      layout.prop_search(ptr, "uv_map", &dataptr, "uv_layers", "", ICON_GROUP_UVS);
      return;
    }
  }

  layout.prop(ptr, "uv_map", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_GROUP_UVS);
}

static void node_shader_init_world_to_tangent(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderWorldToTangent *data = MEM_new<NodeShaderWorldToTangent>("NodeShaderWorldToTangent");
  node->storage = data;
}

static int node_shader_gpu_world_to_tangent(GPUMaterial *mat,
                                            bNode *node,
                                            bNodeExecData * /*execdata*/,
                                            GPUNodeStack *in,
                                            GPUNodeStack *out)
{
  NodeShaderWorldToTangent *data = static_cast<NodeShaderWorldToTangent *>(node->storage);

  GPU_material_flag_set(mat, GPU_MATFLAG_OBJECT_INFO);
  return GPU_stack_link(
      mat, node, "node_world_to_tangent", in, out, GPU_attribute(mat, CD_TANGENT, data->uv_map));
}

}  // namespace nodes::node_shader_world_to_tangent_cc

void register_node_type_sh_world_to_tangent()
{
  namespace file_ns = nodes::node_shader_world_to_tangent_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeWorldToTangent"_ustr, SH_NODE_WORLD_TO_TANGENT);
  ntype.ui_name = "World To Tangent";
  ntype.ui_description = "Transform a world-space vector into the local tangent-space basis";
  ntype.enum_name_legacy = "WORLD_TO_TANGENT";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_world_to_tangent;
  ntype.initfunc = file_ns::node_shader_init_world_to_tangent;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.default_width = 150;
  ntype.minwidth = 120;
  bke::node_type_storage(ntype,
                         "NodeShaderWorldToTangent",
                         node_free_standard_storage,
                         node_copy_standard_storage);
  ntype.gpu_fn = file_ns::node_shader_gpu_world_to_tangent;

  bke::node_register_type(ntype);
}

}  // namespace blender
