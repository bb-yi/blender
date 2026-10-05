/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 *
 * Tessellation settings of an EEVEE material. The settings are constants of the material: links
 * to the inputs are ignored. Every pass (including shadows) tessellates the surface the same way,
 * using the main camera to choose the subdivision levels.
 */

#include "BLI_math_base.h"

#include "NOD_shader.h"

#include "node_shader_util.hh"
#include "node_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_tessellation_control_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("Max Level"_ustr)
      .default_value(4)
      .min(1)
      .max(SHD_TESSELLATION_LEVEL_MAX)
      .description("Highest number of segments each triangle edge is split into");
  b.add_input<decl::Int>("Min Level"_ustr)
      .default_value(1)
      .min(1)
      .max(SHD_TESSELLATION_LEVEL_MAX)
      .description("Lowest number of segments each triangle edge is split into");
  b.add_input<decl::Float>("Near Distance"_ustr)
      .default_value(2.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Camera distance under which the maximum level is used");
  b.add_input<decl::Float>("Far Distance"_ustr)
      .default_value(20.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Camera distance over which the minimum level is used");
  b.add_input<decl::Float>("Edge Length"_ustr)
      .default_value(8.0f)
      .min(1.0f)
      .max(1000.0f)
      .subtype(PROP_PIXEL)
      .description("Target length of the tessellated edges on screen");
  b.add_input<decl::Float>("Phong Smoothing"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Smooth the tessellated surface using the vertex normals");
  b.add_output<decl::Color>("Tessellation"_ustr);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "normal_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const int mode = node->custom1;
  bke::node_set_socket_availability(
      *ntree, *node->input_by_identifier("Min Level"_ustr), mode != SHD_TESSELLATION_FIXED);
  bke::node_set_socket_availability(
      *ntree, *node->input_by_identifier("Near Distance"_ustr), mode == SHD_TESSELLATION_DISTANCE);
  bke::node_set_socket_availability(
      *ntree, *node->input_by_identifier("Far Distance"_ustr), mode == SHD_TESSELLATION_DISTANCE);
  bke::node_set_socket_availability(*ntree,
                                    *node->input_by_identifier("Edge Length"_ustr),
                                    mode == SHD_TESSELLATION_SCREEN_EDGE);
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = SHD_TESSELLATION_FIXED;
  node->custom2 = SHD_TESSELLATION_NORMAL_BUMP;
}

static int node_gpu_material(GPUMaterial *mat,
                             bNode *node,
                             bNodeExecData * /*execdata*/,
                             GPUNodeStack * /*in*/,
                             GPUNodeStack *out)
{
  float packed[4];
  node_shader_tessellation_settings_pack(node_shader_tessellation_settings_get(*node), packed);
  /* Uniform to avoid shader recompilation when tweaking the settings. */
  return GPU_link(mat, "set_rgba", GPU_uniform(packed), &out[0].link);
}

}  // namespace nodes::node_shader_tessellation_control_cc

/* Doesn't require the topology cache, which might not be available during render engine sync. */
static const bNodeSocket *input_socket_find(const bNode &node, const StringRef identifier)
{
  for (const bNodeSocket &socket : node.inputs) {
    if (StringRef(socket.identifier) == identifier) {
      return &socket;
    }
  }
  return nullptr;
}

static int socket_int_value(const bNode &node, const StringRef identifier)
{
  const bNodeSocket *socket = input_socket_find(node, identifier);
  return (socket != nullptr) ? socket->default_value_typed<bNodeSocketValueInt>()->value : 1;
}

static float socket_float_value(const bNode &node, const StringRef identifier)
{
  const bNodeSocket *socket = input_socket_find(node, identifier);
  return (socket != nullptr) ? socket->default_value_typed<bNodeSocketValueFloat>()->value :
                               0.0f;
}

ShaderTessellationSettings node_shader_tessellation_settings_get(const bNode &node)
{
  ShaderTessellationSettings settings;
  settings.mode = node.custom1;
  settings.normal_mode = node.custom2;
  settings.max_level = clamp_i(
      socket_int_value(node, "Max Level"), 1, SHD_TESSELLATION_LEVEL_MAX);
  settings.min_level = clamp_i(socket_int_value(node, "Min Level"), 1, settings.max_level);
  if (settings.mode == SHD_TESSELLATION_FIXED) {
    settings.min_level = settings.max_level;
  }
  settings.near_distance = max_ff(socket_float_value(node, "Near Distance"), 0.0f);
  settings.far_distance = max_ff(socket_float_value(node, "Far Distance"),
                                 settings.near_distance + 1e-4f);
  settings.edge_length = max_ff(socket_float_value(node, "Edge Length"), 1.0f);
  settings.phong = clamp_f(socket_float_value(node, "Phong Smoothing"), 0.0f, 1.0f);
  return settings;
}

void node_shader_tessellation_settings_pack(const ShaderTessellationSettings &settings,
                                            float r_packed[4])
{
  r_packed[0] = float(settings.max_level + settings.min_level * 16 + settings.mode * 256);
  r_packed[1] = (settings.mode == SHD_TESSELLATION_SCREEN_EDGE) ? settings.edge_length :
                                                                  settings.near_distance;
  r_packed[2] = settings.far_distance;
  r_packed[3] = settings.phong;
}

static const bNode *tessellation_source_node(const bNodeSocket *socket)
{
  const bNodeLink *link = socket->link;
  while (link != nullptr && link->is_used()) {
    const bNode *from_node = link->fromnode;
    if (from_node->is_muted()) {
      return nullptr;
    }
    if (from_node->is_reroute()) {
      link = static_cast<const bNodeSocket *>(from_node->inputs.first)->link;
      continue;
    }
    if (from_node->type_legacy == SH_NODE_TESSELLATION_CONTROL) {
      return from_node;
    }
    return nullptr;
  }
  return nullptr;
}

const bNode *ntreeShaderTessellationNode(bNodeTree *ntree)
{
  if (ntree == nullptr) {
    return nullptr;
  }
  bNode *output_node = ntreeShaderOutputNode(ntree, SHD_OUTPUT_EEVEE);
  if (output_node == nullptr || output_node->type_legacy != SH_NODE_OUTPUT_MATERIAL) {
    return nullptr;
  }
  const bNodeSocket *socket = input_socket_find(*output_node, "Tessellation");
  if (socket == nullptr) {
    return nullptr;
  }
  return tessellation_source_node(socket);
}

void register_node_type_sh_tessellation_control()
{
  namespace file_ns = nodes::node_shader_tessellation_control_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeTessellationControl"_ustr, SH_NODE_TESSELLATION_CONTROL);
  ntype.ui_name = "Tessellation Control";
  ntype.ui_description =
      "Subdivide the surface of an EEVEE mesh material before displacement. Connect to the "
      "Tessellation input of the Material Output";
  ntype.enum_name_legacy = "TESSELLATION_CONTROL";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_gpu_material;

  bke::node_register_type(ntype);
}

}  // namespace blender
