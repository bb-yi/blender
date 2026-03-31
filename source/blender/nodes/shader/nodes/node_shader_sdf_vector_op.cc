/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2021 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup shdnodes
 */

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "BLI_string_utf8.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "../node_shader_util.hh"
#include "node_util.hh"

namespace blender {

namespace nodes::node_shader_sdf_vector_op_cc {

NODE_STORAGE_FUNCS(NodeSdfVectorOp)

static void sh_node_sdf_vector_op_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector1").hide_value();
  b.add_input<decl::Vector>("Vector2").no_muted_links();
  b.add_input<decl::Vector>("Vector3").no_muted_links();
  b.add_input<decl::Float>("Scale").min(-100000.0f).max(100000.0f).default_value(1.0f);
  b.add_input<decl::Float>("Value1").min(-100000.0f).max(100000.0f).default_value(0.0f);
  b.add_input<decl::Float>("Value2").min(-100000.0f).max(100000.0f).default_value(0.0f);
  b.add_input<decl::Float>("Angle").subtype(PROP_ANGLE);
  b.add_input<decl::Int>("Count1").min(0).max(127).default_value(4);
  b.add_input<decl::Int>("Count2").min(0).max(127).default_value(4);

  b.add_output<decl::Vector>("Vector");
  b.add_output<decl::Vector>("Position");
  b.add_output<decl::Float>("Value");
}

}  // namespace nodes::node_shader_sdf_vector_op_cc

static const char *node_shader_sdf_vector_op_get_name(int mode)
{
  switch (mode) {
    case SHD_SDF_VEC_OP_SPIN:
      return "node_sdf_vector_op_spin";
    case SHD_SDF_VEC_OP_SWIZZLE:
      return "node_sdf_vector_op_swizzle";
    case SHD_SDF_VEC_OP_EXTRUDE:
      return "node_sdf_vector_op_extrude";
    case SHD_SDF_VEC_OP_TWIST:
      return "node_sdf_vector_op_twist";
    case SHD_SDF_VEC_OP_SWIRL:
      return "node_sdf_vector_op_swirl";
    case SHD_SDF_VEC_OP_RADIAL_SHEAR:
      return "node_sdf_vector_op_radial_shear";
    case SHD_SDF_VEC_OP_PINCH_INFLATE:
      return "node_sdf_vector_op_pinch_inflate";
    case SHD_SDF_VEC_OP_BEND:
      return "node_sdf_vector_op_bend";
    case SHD_SDF_VEC_OP_REPEAT_FINITE:
      return "node_sdf_vector_op_repeat";
    case SHD_SDF_VEC_OP_REPEAT_INF:
      return "node_sdf_vector_op_repeat_inf";
    case SHD_SDF_VEC_OP_REPEAT_INF_MIRROR:
      return "node_sdf_vector_op_repeat_inf_mirror";
    case SHD_SDF_VEC_OP_ROTATE:
      return "node_sdf_vector_op_rotate";
    case SHD_SDF_VEC_OP_REFLECT:
      return "node_sdf_vector_op_reflect";
    case SHD_SDF_VEC_OP_MIRROR:
      return "node_sdf_vector_op_mirror";
    case SHD_SDF_VEC_OP_POLAR:
      return "node_sdf_vector_op_polar";
    case SHD_SDF_VEC_OP_MAP_UV:
      return "node_sdf_vector_op_map_uv";
    case SHD_SDF_VEC_OP_MAP_11:
      return "node_sdf_vector_op_map_11";
    case SHD_SDF_VEC_OP_MAP_05:
      return "node_sdf_vector_op_map_05";
    case SHD_SDF_VEC_OP_ROTATE_UV:
      return "node_sdf_vector_op_uv_rotate";
    case SHD_SDF_VEC_OP_SCALE_UV:
      return "node_sdf_vector_op_uv_scale";
    case SHD_SDF_VEC_OP_RND_UV:
      return "node_sdf_vector_op_random_uv_rotate";
    case SHD_SDF_VEC_OP_RND_UV_FLIP:
      return "node_sdf_vector_op_random_uv_flip";
    case SHD_SDF_VEC_OP_OCTANT:
      return "node_sdf_vector_op_octant";
    case SHD_SDF_VEC_OP_TILESET:
      return "node_sdf_vector_op_tileset";
    case SHD_SDF_VEC_OP_GRID:
      return "node_sdf_vector_op_grid";
  }

  return nullptr;
}

static int node_shader_gpu_sdf_vector_op(GPUMaterial *mat,
                                         bNode *node,
                                         bNodeExecData * /*execdata*/,
                                         GPUNodeStack *in,
                                         GPUNodeStack *out)
{
  NodeSdfVectorOp *sdf = static_cast<NodeSdfVectorOp *>(node->storage);
  const char *name = node_shader_sdf_vector_op_get_name(sdf->operation);

  if (name != nullptr) {
    float axis = float(sdf->axis);
    return GPU_stack_link(mat, node, name, in, out, GPU_constant(&axis));
  }

  return 0;
}

static void node_shader_label_sdf_vector_op(const bNodeTree * /*ntree*/,
                                            const bNode *node,
                                            char *label,
                                            int maxlen)
{
  NodeSdfVectorOp &node_storage = *static_cast<NodeSdfVectorOp *>(node->storage);
  const char *name;
  const bool enum_label = RNA_enum_name(
      rna_enum_node_sdf_vector_op_items, node_storage.operation, &name);
  if (!enum_label) {
    name = "Unknown SDF Vector Op";
  }
  BLI_strncpy_utf8(label, IFACE_(name), maxlen);
}

static void node_shader_update_sdf_vector_op(bNodeTree *ntree, bNode *node)
{
  NodeSdfVectorOp *sdf = static_cast<NodeSdfVectorOp *>(node->storage);

  bNodeSocket *sock_vector_1 = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 0));
  bNodeSocket *sock_vector_2 = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 1));
  bNodeSocket *sock_vector_3 = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 2));
  bNodeSocket *sock_scale = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 3));
  bNodeSocket *sock_value = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 4));
  bNodeSocket *sock_value_2 = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 5));
  bNodeSocket *sock_angle = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 6));
  bNodeSocket *sock_count = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 7));
  bNodeSocket *sock_count_2 = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 8));

  bNodeSocket *sock_vector_out = static_cast<bNodeSocket *>(BLI_findlink(&node->outputs, 0));
  bNodeSocket *sock_position_out = static_cast<bNodeSocket *>(BLI_findlink(&node->outputs, 1));
  bNodeSocket *sock_value_out = static_cast<bNodeSocket *>(BLI_findlink(&node->outputs, 2));

  bke::node_set_socket_availability(*ntree,
                                    *sock_value_out,
                                    ELEM(sdf->operation,
                                         SHD_SDF_VEC_OP_MIRROR,
                                         SHD_SDF_VEC_OP_REFLECT,
                                         SHD_SDF_VEC_OP_POLAR,
                                         SHD_SDF_VEC_OP_EXTRUDE));

  bke::node_set_socket_availability(
      *ntree, *sock_position_out, ELEM(sdf->operation, SHD_SDF_VEC_OP_MIRROR, SHD_SDF_VEC_OP_GRID));

  bke::node_set_socket_availability(*ntree,
                                    *sock_vector_2,
                                    ELEM(sdf->operation,
                                         SHD_SDF_VEC_OP_SWIRL,
                                         SHD_SDF_VEC_OP_RADIAL_SHEAR,
                                         SHD_SDF_VEC_OP_PINCH_INFLATE,
                                         SHD_SDF_VEC_OP_ROTATE_UV,
                                         SHD_SDF_VEC_OP_GRID,
                                         SHD_SDF_VEC_OP_RND_UV,
                                         SHD_SDF_VEC_OP_RND_UV_FLIP,
                                         SHD_SDF_VEC_OP_MIRROR,
                                         SHD_SDF_VEC_OP_EXTRUDE,
                                         SHD_SDF_VEC_OP_REFLECT,
                                         SHD_SDF_VEC_OP_REPEAT_FINITE,
                                         SHD_SDF_VEC_OP_REPEAT_INF,
                                         SHD_SDF_VEC_OP_REPEAT_INF_MIRROR));

  bke::node_set_socket_availability(*ntree,
                                    *sock_vector_3,
                                    ELEM(sdf->operation,
                                         SHD_SDF_VEC_OP_SWIRL,
                                         SHD_SDF_VEC_OP_RADIAL_SHEAR,
                                         SHD_SDF_VEC_OP_REPEAT_FINITE));

  bke::node_set_socket_availability(
      *ntree, *sock_scale, ELEM(sdf->operation, SHD_SDF_VEC_OP_SCALE_UV, SHD_SDF_VEC_OP_TILESET));

  bke::node_set_socket_availability(*ntree,
                                    *sock_value,
                                    !ELEM(sdf->operation,
                                          SHD_SDF_VEC_OP_MAP_11,
                                          SHD_SDF_VEC_OP_MAP_05,
                                          SHD_SDF_VEC_OP_MAP_UV,
                                          SHD_SDF_VEC_OP_RND_UV_FLIP,
                                          SHD_SDF_VEC_OP_ROTATE_UV,
                                          SHD_SDF_VEC_OP_RND_UV,
                                          SHD_SDF_VEC_OP_SCALE_UV,
                                          SHD_SDF_VEC_OP_EXTRUDE,
                                          SHD_SDF_VEC_OP_GRID,
                                          SHD_SDF_VEC_OP_MIRROR,
                                          SHD_SDF_VEC_OP_ROTATE,
                                          SHD_SDF_VEC_OP_SWIZZLE,
                                          SHD_SDF_VEC_OP_BEND,
                                          SHD_SDF_VEC_OP_REPEAT_FINITE,
                                          SHD_SDF_VEC_OP_REPEAT_INF,
                                          SHD_SDF_VEC_OP_REPEAT_INF_MIRROR));

  bke::node_set_socket_availability(
      *ntree,
      *sock_value_2,
      ELEM(sdf->operation, SHD_SDF_VEC_OP_TILESET, SHD_SDF_VEC_OP_PINCH_INFLATE));

  bke::node_set_socket_availability(*ntree,
                                    *sock_angle,
                                    ELEM(sdf->operation,
                                         SHD_SDF_VEC_OP_BEND,
                                         SHD_SDF_VEC_OP_TWIST,
                                         SHD_SDF_VEC_OP_ROTATE,
                                         SHD_SDF_VEC_OP_ROTATE_UV));
  bke::node_set_socket_availability(*ntree, *sock_count, ELEM(sdf->operation, SHD_SDF_VEC_OP_TILESET));
  bke::node_set_socket_availability(*ntree,
                                    *sock_count_2,
                                    ELEM(sdf->operation, SHD_SDF_VEC_OP_TILESET));

  node_sock_label_clear(sock_value);
  node_sock_label_clear(sock_value_2);
  node_sock_label_clear(sock_value_out);
  node_sock_label_clear(sock_vector_1);
  node_sock_label_clear(sock_vector_2);
  node_sock_label_clear(sock_vector_out);
  node_sock_label(sock_vector_1, "Vector");
  node_sock_label(sock_vector_2, "Vector");
  sock_vector_2->flag &= ~SOCK_HIDE_VALUE;

  switch (sdf->operation) {
    case SHD_SDF_VEC_OP_GRID:
      node_sock_label(sock_vector_2, "Scale");
      node_sock_label(sock_vector_out, "UVW");
      break;
    case SHD_SDF_VEC_OP_ROTATE_UV:
      node_sock_label(sock_vector_2, "Center");
      node_sock_label(sock_vector_1, "UV");
      node_sock_label(sock_vector_out, "UV");
      break;
    case SHD_SDF_VEC_OP_MAP_UV:
      node_sock_label(sock_vector_out, "UVW");
      break;
    case SHD_SDF_VEC_OP_MAP_11:
    case SHD_SDF_VEC_OP_MAP_05:
      node_sock_label(sock_vector_1, "UV");
      node_sock_label(sock_vector_out, "Vector");
      break;
    case SHD_SDF_VEC_OP_TILESET:
      node_sock_label(sock_value, "Index");
      node_sock_label(sock_value_2, "Padding");
      node_sock_label(sock_vector_1, "UV");
      node_sock_label(sock_vector_out, "UV");
      sock_vector_2->flag |= SOCK_HIDE_VALUE;
      break;
    case SHD_SDF_VEC_OP_RND_UV:
    case SHD_SDF_VEC_OP_RND_UV_FLIP:
      node_sock_label(sock_value, "Padding");
      node_sock_label(sock_vector_1, "UV");
      node_sock_label(sock_vector_2, "Position");
      node_sock_label(sock_vector_out, "UV");
      sock_vector_2->flag |= SOCK_HIDE_VALUE;
      break;
    case SHD_SDF_VEC_OP_REPEAT_FINITE:
      node_sock_label(sock_vector_2, "Spacing");
      node_sock_label(sock_vector_3, "Count");
      break;
    case SHD_SDF_VEC_OP_REPEAT_INF_MIRROR:
    case SHD_SDF_VEC_OP_REPEAT_INF:
    case SHD_SDF_VEC_OP_MIRROR:
      node_sock_label(sock_vector_2, "Spacing");
      break;
    case SHD_SDF_VEC_OP_REFLECT:
      node_sock_label(sock_value, "Offset");
      node_sock_label(sock_vector_2, "Normal");
      node_sock_label(sock_value_out, "Mask");
      break;
    case SHD_SDF_VEC_OP_POLAR:
      node_sock_label(sock_value, "Count");
      node_sock_label(sock_value_out, "Mask");
      break;
    case SHD_SDF_VEC_OP_EXTRUDE:
      node_sock_label(sock_value_out, "Internal Distance");
      break;
    case SHD_SDF_VEC_OP_TWIST:
      node_sock_label(sock_value, "Twist");
      break;
    case SHD_SDF_VEC_OP_SWIRL:
    case SHD_SDF_VEC_OP_RADIAL_SHEAR:
    case SHD_SDF_VEC_OP_PINCH_INFLATE:
      node_sock_label(sock_value, "Strength");
      node_sock_label(sock_value_2, "Radius");
      node_sock_label(sock_vector_2, "Center");
      node_sock_label(sock_vector_3, "Offset");
      break;
    case SHD_SDF_VEC_OP_SPIN:
      node_sock_label(sock_value, "Offset");
      break;
  }
}

static void node_shader_init_sdf_vector_op(bNodeTree * /*ntree*/, bNode *node)
{
  NodeSdfVectorOp *sdf = MEM_new<NodeSdfVectorOp>(__func__);
  sdf->operation = SHD_SDF_VEC_OP_GRID;
  sdf->axis = SHD_SDF_AXIS_XYZ;
  node->storage = sdf;
}

static void node_shader_buts_sdf_vector_op(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "operation", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
  const int type = RNA_enum_get(ptr, "operation");
  if (ELEM(type,
           SHD_SDF_VEC_OP_ROTATE_UV,
           SHD_SDF_VEC_OP_OCTANT,
           SHD_SDF_VEC_OP_GRID,
           SHD_SDF_VEC_OP_TWIST,
           SHD_SDF_VEC_OP_SWIRL,
           SHD_SDF_VEC_OP_RADIAL_SHEAR,
           SHD_SDF_VEC_OP_MIRROR,
           SHD_SDF_VEC_OP_SWIZZLE,
           SHD_SDF_VEC_OP_ROTATE,
           SHD_SDF_VEC_OP_POLAR,
           SHD_SDF_VEC_OP_BEND,
           SHD_SDF_VEC_OP_SPIN,
           SHD_SDF_VEC_OP_EXTRUDE)) {
    layout.prop(ptr, "axis", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
  }
}

void register_node_type_sh_sdf_vector_op()
{
  namespace file_ns = nodes::node_shader_sdf_vector_op_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeSdfVectorOp", SH_NODE_SDF_VECTOR_OP);
  ntype.ui_name = "SDF Vector Operator";
  ntype.ui_description = "Transform or remap vector domains used by SDF workflows";
  ntype.enum_name_legacy = "SDF_VECTOR_OP";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = file_ns::sh_node_sdf_vector_op_declare;
  ntype.draw_buttons = node_shader_buts_sdf_vector_op;
  ntype.initfunc = node_shader_init_sdf_vector_op;
  ntype.labelfunc = node_shader_label_sdf_vector_op;
  ntype.updatefunc = node_shader_update_sdf_vector_op;
  ntype.add_ui_poll = eevee_shader_nodes_poll;
  ntype.gpu_fn = node_shader_gpu_sdf_vector_op;
  bke::node_type_storage(
      ntype, "NodeSdfVectorOp", node_free_standard_storage, node_copy_standard_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
