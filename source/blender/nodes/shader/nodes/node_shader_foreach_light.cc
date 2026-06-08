/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "../../../blenloader/BLO_read_write.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "NOD_sh_zones.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender {

namespace nodes::node_shader_foreach_light_cc {

static void node_layout_ex(ui::Layout &layout, bContext *C, PointerRNA *current_node_ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(current_node_ptr->owner_id);
  bNode *current_node = static_cast<bNode *>(current_node_ptr->data);

  const bke::bNodeTreeZones *zones = ntree.zones();
  if (!zones) {
    return;
  }
  const bke::bNodeTreeZone *zone = zones->get_zone_by_node(current_node->identifier);
  if (!zone || !zone->output_node_id) {
    return;
  }

  bNode &output_node = const_cast<bNode &>(*zone->output_node());

  if (ui::Layout *panel = layout.panel(C, "foreach_light_items", false, IFACE_("For Each Light Items")))
  {
    socket_items::ui::draw_items_list_with_operators<ShForeachLightItemsAccessor>(
        C, panel, ntree, output_node);
    socket_items::ui::draw_active_item_props<ShForeachLightItemsAccessor>(
        ntree, output_node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

namespace input_node {

NODE_STORAGE_FUNCS(NodeShaderForeachLightInput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Vector>("Normal"_ustr).hide_value();
  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Vector>("Direction"_ustr);
  b.add_output<decl::Float>("Distance"_ustr);
  b.add_output<decl::Float>("Attenuation"_ustr);
  b.add_output<decl::Float>("Shadow Mask"_ustr);

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  if (node && tree) {
    const NodeShaderForeachLightInput &storage = node_storage(*node);
    const bNode *output_node = tree->node_by_id(storage.output_node_id);
    if (output_node) {
      const auto &output_storage = *static_cast<const NodeShaderForeachLightOutput *>(
          output_node->storage);
      for (const int i : IndexRange(output_storage.items_num)) {
        const NodeShaderForeachLightItem &item = output_storage.items[i];
        const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
        const StringRefNull name = item.name ? item.name : "";
        const std::string identifier = ShForeachLightItemsAccessor::socket_identifier_for_item(
            item);
        if (socket_type == SOCK_RGBA) {
          auto &input_decl = b.add_input<decl::Color>(UString(name), UString(identifier))
                                 .default_value(ColorGeometry4f(0.0f, 0.0f, 0.0f, 1.0f))
                                 .socket_name_ptr(
                                     &tree->id, *ShForeachLightItemsAccessor::item_srna, &item, "name");
          auto &output_decl = b.add_output(socket_type, UString(name), UString(identifier))
                                  .align_with_previous();
          input_decl.structure_type(StructureType::Dynamic);
          output_decl.structure_type(StructureType::Dynamic);
        }
        else {
          auto &input_decl = b.add_input(socket_type, UString(name), UString(identifier))
                                 .socket_name_ptr(
                                     &tree->id, *ShForeachLightItemsAccessor::item_srna, &item, "name");
          auto &output_decl = b.add_output(socket_type, UString(name), UString(identifier))
                                  .align_with_previous();
          input_decl.structure_type(StructureType::Dynamic);
          output_decl.structure_type(StructureType::Dynamic);
        }
      }
    }
  }
  b.add_input<decl::Extend>(""_ustr, "__extend__"_ustr).structure_type(StructureType::Dynamic);
  b.add_output<decl::Extend>(""_ustr, "__extend__"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeShaderForeachLightInput *data = MEM_new<NodeShaderForeachLightInput>(__func__);
  data->output_node_id = 0;
  node->storage = data;
}

static void node_label(const bNodeTree * /*ntree*/,
                       const bNode * /*node*/,
                       char *label,
                       const int label_maxncpy)
{
  BLI_strncpy_utf8(label, IFACE_("For Each Light"), label_maxncpy);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  bNode *output_node = params.ntree.node_by_id(node_storage(params.node).output_node_id);
  if (!output_node) {
    return true;
  }
  return socket_items::try_add_item_via_any_extend_socket<ShForeachLightItemsAccessor>(
      params.ntree, params.node, *output_node, params.link);
}

static int node_shader_fn(GPUMaterial *mat,
                          bNode *node,
                          bNodeExecData * /*execdata*/,
                          GPUNodeStack *in,
                          GPUNodeStack *out)
{
  if (!in[0].link) {
    GPU_link(mat, "world_normals_get", &in[0].link);
  }
  GPU_material_flag_set(mat, GPU_MATFLAG_NPR_FOREACH_LIGHT);
  const int zone_id = node_storage(*node).output_node_id;
  return GPU_stack_link_zone(mat, node, "FOREACH_LIGHT_BEGIN", in, out, zone_id, false, 1, 5);
}

static void register_node()
{
  static bke::bNodeType ntype;
  sh_node_type_base(&ntype, "ShaderNodeForeachLightInput"_ustr, SH_NODE_FOREACH_LIGHT_INPUT);
  ntype.enum_name_legacy = "FOREACH_LIGHT_INPUT";
  ntype.ui_name = "For Each Light Input";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.labelfunc = node_label;
  ntype.gather_link_search_ops = nullptr;
  ntype.insert_link = node_insert_link;
  ntype.no_muting = true;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.add_ui_poll = npr_shader_nodes_poll;
  ntype.gpu_fn = node_shader_fn;
  bke::node_type_storage(
      ntype, "NodeShaderForeachLightInput", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);
}

}  // namespace input_node

namespace output_node {

NODE_STORAGE_FUNCS(NodeShaderForeachLightOutput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (node && tree) {
    const NodeShaderForeachLightOutput &storage = node_storage(*node);
    for (const int i : IndexRange(storage.items_num)) {
      const NodeShaderForeachLightItem &item = storage.items[i];
      const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
      const StringRefNull name = item.name ? item.name : "";
      const std::string identifier = ShForeachLightItemsAccessor::socket_identifier_for_item(item);
      auto &input_decl = b.add_input(socket_type, UString(name), UString(identifier))
                             .socket_name_ptr(
                                 &tree->id, *ShForeachLightItemsAccessor::item_srna, &item, "name")
                             .hide_value();
      auto &output_decl = b.add_output(socket_type, UString(name), UString(identifier))
                              .align_with_previous();
      input_decl.structure_type(StructureType::Dynamic);
      output_decl.structure_type(StructureType::Dynamic);
    }
  }
  b.add_input<decl::Extend>(""_ustr, "__extend__"_ustr).structure_type(StructureType::Dynamic);
  b.add_output<decl::Extend>(""_ustr, "__extend__"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeShaderForeachLightOutput *data = MEM_new<NodeShaderForeachLightOutput>(__func__);
  data->next_identifier = 0;
  data->items = MEM_new_array<NodeShaderForeachLightItem>(1, __func__);
  data->items[0].name = BLI_strdup(DATA_("Zone IO"));
  data->items[0].socket_type = SOCK_RGBA;
  data->items[0].identifier = data->next_identifier++;
  data->items_num = 1;
  node->storage = data;
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<ShForeachLightItemsAccessor>(*node);
  MEM_delete(static_cast<NodeShaderForeachLightOutput *>(node->storage));
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeShaderForeachLightOutput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_new<NodeShaderForeachLightOutput>(__func__, dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;
  socket_items::copy_array<ShForeachLightItemsAccessor>(*src_node, *dst_node);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<ShForeachLightItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ShForeachLightItemsAccessor>();
}

static int node_shader_fn(GPUMaterial *mat,
                          bNode *node,
                          bNodeExecData * /*execdata*/,
                          GPUNodeStack *in,
                          GPUNodeStack *out)
{
  const int zone_id = node->identifier;
  return GPU_stack_link_zone(mat, node, "FOREACH_LIGHT_END", in, out, zone_id, true, 0, 0);
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<ShForeachLightItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<ShForeachLightItemsAccessor>(&reader, node);
}

static void register_node()
{
  static bke::bNodeType ntype;
  sh_node_type_base(&ntype, "ShaderNodeForeachLightOutput"_ustr, SH_NODE_FOREACH_LIGHT_OUTPUT);
  ntype.enum_name_legacy = "FOREACH_LIGHT_OUTPUT";
  ntype.ui_name = "For Each Light Output";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.labelfunc = input_node::node_label;
  ntype.insert_link = node_insert_link;
  ntype.no_muting = true;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  ntype.add_ui_poll = npr_shader_nodes_poll;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  ntype.gpu_fn = node_shader_fn;
  bke::node_type_storage(
      ntype, "NodeShaderForeachLightOutput", node_free_storage, node_copy_storage);
  bke::node_register_type(ntype);
}

}  // namespace output_node

}  // namespace nodes::node_shader_foreach_light_cc

namespace nodes {

StructRNA **ShForeachLightItemsAccessor::item_srna = &RNA_ShaderForeachLightItem;
int ShForeachLightItemsAccessor::node_type = SH_NODE_FOREACH_LIGHT_OUTPUT;

void ShForeachLightItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  writer->write_string(item.name);
}

void ShForeachLightItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace nodes

Span<NodeShaderForeachLightItem> NodeShaderForeachLightOutput::items_span() const
{
  return Span<NodeShaderForeachLightItem>(items, items_num);
}

MutableSpan<NodeShaderForeachLightItem> NodeShaderForeachLightOutput::items_span()
{
  return MutableSpan<NodeShaderForeachLightItem>(items, items_num);
}

void register_node_type_sh_foreach_light()
{
  nodes::node_shader_foreach_light_cc::input_node::register_node();
  nodes::node_shader_foreach_light_cc::output_node::register_node();
}

}  // namespace blender
