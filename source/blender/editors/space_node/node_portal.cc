#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_node_tree_zones.hh"
#include "BKE_report.hh"

#include "BLI_listbase.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_string_utils.hh"

#include "BLT_translation.hh"

#include "ED_render.hh"
#include "ED_screen.hh"

#include "NOD_shader_nodes_inline.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_icons.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

using bke::node_tree_runtime::find_shader_portal_input_node;
using bke::node_tree_runtime::shader_portal_output_nodes;

static bNodeTree &portal_tree(bContext *C)
{
  return *CTX_wm_space_node(C)->edittree;
}

static bool portal_editable(bContext *C)
{
  return ED_operator_node_editable(C) && portal_tree(C).type == NTREE_SHADER;
}

static bool portal_navigable(bContext *C)
{
  return ED_operator_node_active(C) && portal_tree(C).type == NTREE_SHADER &&
         CTX_wm_region(C) && CTX_wm_region(C)->regiontype == RGN_TYPE_WINDOW;
}

static NodeShaderPortal *portal_storage(const bNode *node, const int node_type)
{
  return node && node->type_legacy == node_type ? static_cast<NodeShaderPortal *>(node->storage) :
                                                 nullptr;
}

static bNode *portal_operator_node(bNodeTree &tree, PointerRNA *ptr)
{
  tree.ensure_topology_cache();
  const int node_id = RNA_int_get(ptr, "node_id");
  return node_id > 0 ? tree.node_by_id(node_id) : bke::node_get_active(tree);
}

static void portal_def_node_id(wmOperatorType *ot)
{
  RNA_def_property_flag(RNA_def_int(ot->srna, "node_id", 0, 0, INT_MAX, "Node", "", 0, INT_MAX),
                       PROP_HIDDEN | PROP_SKIP_SAVE);
}

static void portal_update(bContext *C, bNodeTree &tree)
{
  BKE_main_ensure_invariants(*CTX_data_main(C), tree.id);
  WM_event_add_notifier(C, NC_NODE | NA_EDITED, nullptr);
}

static bool portal_link_loses_active_socket(const bNodeTree &tree,
                                            const bNodeLink &link,
                                            const eNodeSocketDatatype new_type)
{
  if (!tree.typeinfo->validate_link(new_type, link.tosock->type)) {
    return true;
  }
  for (const bNodeLink *existing : link.tosock->directly_linked_links()) {
    if (existing->fromnode == link.fromnode && existing->fromsock->type == new_type) {
      return true;
    }
  }
  return false;
}

static int portal_affected_links_num(const bNodeTree &tree,
                                     const bNode &node,
                                     const NodeShaderPortal &storage,
                                     const eNodeSocketDatatype type,
                                     const bool disabled)
{
  int count = 0;
  for (const bNodeLink &link : tree.links) {
    if (link.fromnode != &node) {
      continue;
    }
    if (disabled ? (link.fromsock->type == storage.data_type &&
                    portal_link_loses_active_socket(tree, link, type)) :
                   link.fromsock->type == type)
    {
      count++;
    }
  }
  return count;
}

static int portal_type_icon(const int type)
{
  switch (type) {
    case SOCK_FLOAT:
      return ICON_NODE_SOCKET_FLOAT;
    case SOCK_VECTOR:
      return ICON_NODE_SOCKET_VECTOR;
    case SOCK_RGBA:
      return ICON_NODE_SOCKET_RGBA;
    case SOCK_SHADER:
      return ICON_NODE_SOCKET_SHADER;
    default:
      return ICON_NONE;
  }
}

static const EnumPropertyItem *portal_source_items(bContext *C,
                                                   PointerRNA *ptr,
                                                   PropertyRNA * /*prop*/,
                                                   bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  if (C && portal_editable(C)) {
    bNodeTree &tree = portal_tree(C);
    const bNode *out = portal_operator_node(tree, ptr);
    if (const NodeShaderPortal *out_data = portal_storage(out, SH_NODE_PORTAL_OUT)) {
      for (const bNode &node : tree.nodes) {
        const NodeShaderPortal *data = portal_storage(&node, SH_NODE_PORTAL_IN);
        if (!data || !data->name[0] || find_shader_portal_input_node(tree, data->name) != &node) {
          continue;
        }
        const auto type = eNodeSocketDatatype(data->data_type);
        const bool affects_links = type != out_data->data_type &&
                                   (portal_affected_links_num(tree, *out, *out_data, type, true) ||
                                    portal_affected_links_num(tree, *out, *out_data, type, false));
        const EnumPropertyItem item = {node.identifier,
                                       node.name,
                                       affects_links ? ICON_ERROR : portal_type_icon(data->data_type),
                                       data->name,
                                       affects_links ? N_("Changing type affects output links") :
                                                       ""};
        RNA_enum_item_add(&items, &items_num, &item);
      }
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

static wmOperatorStatus portal_bind_exec(bContext *C, wmOperator *op)
{
  bNodeTree &tree = portal_tree(C);
  bNode *out = portal_operator_node(tree, op->ptr);
  NodeShaderPortal *out_data = portal_storage(out, SH_NODE_PORTAL_OUT);
  const bNode *in = tree.node_by_id(RNA_enum_get(op->ptr, "source"));
  const NodeShaderPortal *in_data = portal_storage(in, SH_NODE_PORTAL_IN);
  if (!out_data || !in_data || !in_data->name[0] ||
      find_shader_portal_input_node(tree, in_data->name) != in)
  {
    BKE_report(op->reports, RPT_WARNING, "Choose an available, uniquely named Portal In");
    return OPERATOR_CANCELLED;
  }
  if (out_data->data_type != in_data->data_type && !RNA_boolean_get(op->ptr, "allow_type_change")) {
    const auto type = eNodeSocketDatatype(in_data->data_type);
    const int disabled = portal_affected_links_num(tree, *out, *out_data, type, true);
    const int enabled = portal_affected_links_num(tree, *out, *out_data, type, false);
    if (disabled || enabled) {
      char message[192];
      BLI_snprintf_utf8(message,
                        sizeof(message),
                        IFACE_("Changing type will disable %d existing links and enable %d links"),
                        disabled,
                        enabled);
      if (G.background || !CTX_wm_window(C)) {
        BKE_report(op->reports, RPT_WARNING, message);
        return OPERATOR_CANCELLED;
      }
      std::string details = message;
      int shown = 0;
      for (const bNodeLink &link : tree.links) {
        if (link.fromnode != out ||
            (link.fromsock->type != type &&
             !(link.fromsock->type == out_data->data_type &&
               portal_link_loses_active_socket(tree, link, type))))
        {
          continue;
        }
        if (shown++ == 4) {
          details += "\n...";
          break;
        }
        details += "\n";
        details += link.fromsock->type == out_data->data_type ? IFACE_("Disable") : IFACE_("Enable");
        details += ": ";
        details += link.tonode->name;
        details += " / ";
        details += link.tosock->name;
      }
      RNA_int_set(op->ptr, "node_id", out->identifier);
      RNA_boolean_set(op->ptr, "allow_type_change", true);
      return WM_operator_confirm_ex(C,
                                    op,
                                    IFACE_("Change Portal Source"),
                                    details.c_str(),
                                    IFACE_("Change"),
                                    ui::AlertIcon::Warning,
                                    true);
    }
  }
  STRNCPY(out_data->name, in_data->name);
  BKE_ntree_update_tag_node_property(&tree, out);
  portal_update(C, tree);
  return OPERATOR_FINISHED;
}

void NODE_OT_shader_portal_bind(wmOperatorType *ot)
{
  ot->name = "Choose Portal Source";
  ot->idname = "NODE_OT_shader_portal_bind";
  ot->description = "Search Portal In nodes and use the selected source's name and type";
  ot->poll = portal_editable;
  ot->invoke = WM_enum_search_invoke;
  ot->exec = portal_bind_exec;
  ot->flag = OPTYPE_UNDO;
  portal_def_node_id(ot);
  ot->prop = RNA_def_enum(ot->srna, "source", rna_enum_dummy_NULL_items, 0, "Source", "");
  RNA_def_enum_funcs(ot->prop, portal_source_items);
  RNA_def_property_flag(RNA_def_boolean(ot->srna, "allow_type_change", false, "Confirm Type Change", ""),
                       PROP_HIDDEN | PROP_SKIP_SAVE);
}

static wmOperatorStatus portal_rename_exec(bContext *C, wmOperator *op)
{
  bNodeTree &tree = portal_tree(C);
  bNode *in = portal_operator_node(tree, op->ptr);
  NodeShaderPortal *data = portal_storage(in, SH_NODE_PORTAL_IN);
  if (!data) {
    return OPERATOR_CANCELLED;
  }
  char name[sizeof(data->name)];
  RNA_string_get(op->ptr, "name", name);
  const char *error = nullptr;
  if (!bke::node_tree_runtime::rename_shader_portal_input(tree, *in, name, &error)) {
    if (error) {
      BKE_report(op->reports, RPT_WARNING, error);
    }
    return OPERATOR_CANCELLED;
  }
  portal_update(C, tree);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus portal_rename_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  bNode *in = portal_operator_node(portal_tree(C), op->ptr);
  const NodeShaderPortal *data = portal_storage(in, SH_NODE_PORTAL_IN);
  if (!data) {
    return OPERATOR_CANCELLED;
  }
  RNA_int_set(op->ptr, "node_id", in->identifier);
  if (RNA_struct_property_is_set(op->ptr, "name")) {
    return portal_rename_exec(C, op);
  }
  RNA_string_set(op->ptr, "name", data->name);
  return WM_operator_props_dialog_popup(C, op, 240);
}

void NODE_OT_shader_portal_rename(wmOperatorType *ot)
{
  ot->name = "Rename Portal";
  ot->idname = "NODE_OT_shader_portal_rename";
  ot->description = "Rename the Portal In and its paired outputs";
  ot->poll = portal_editable;
  ot->invoke = portal_rename_invoke;
  ot->exec = portal_rename_exec;
  ot->flag = OPTYPE_UNDO;
  portal_def_node_id(ot);
  ot->prop = RNA_def_string(ot->srna, "name", nullptr, sizeof(NodeShaderPortal::name), "Name", "");
  RNA_def_property_flag(ot->prop, PROP_SKIP_SAVE);
}

static const EnumPropertyItem *portal_output_items(bContext *C,
                                                   PointerRNA *ptr,
                                                   PropertyRNA * /*prop*/,
                                                   bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  if (C && portal_navigable(C)) {
    bNodeTree &tree = portal_tree(C);
    if (const bNode *in = portal_operator_node(tree, ptr)) {
      for (const bNode *out : shader_portal_output_nodes(tree, *in)) {
        const EnumPropertyItem item = {out->identifier, out->name, ICON_NONE, out->name, out->label};
        RNA_enum_item_add(&items, &items_num, &item);
      }
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

static wmOperatorStatus portal_jump_to_output_exec(bContext *C, wmOperator *op)
{
  bNodeTree &tree = portal_tree(C);
  const bNode *in = portal_operator_node(tree, op->ptr);
  bNode *out = tree.node_by_id(RNA_enum_get(op->ptr, "output"));
  if (!in || !out || !shader_portal_output_nodes(tree, *in).contains(out)) {
    return OPERATOR_CANCELLED;
  }
  node_select_single(*C, *out);
  space_node_view_flag(*C,
                       *CTX_wm_space_node(C),
                       *CTX_wm_region(C),
                       NODE_SELECT,
                       WM_operator_smooth_viewtx_get(op));
  return OPERATOR_FINISHED;
}

void NODE_OT_shader_portal_jump_to_output(wmOperatorType *ot)
{
  ot->name = "Jump to Portal Out";
  ot->idname = "NODE_OT_shader_portal_jump_to_output";
  ot->description = "Search this portal's paired outputs and focus the selected node";
  ot->poll = portal_navigable;
  ot->invoke = WM_enum_search_invoke;
  ot->exec = portal_jump_to_output_exec;
  ot->flag = OPTYPE_UNDO;
  portal_def_node_id(ot);
  ot->prop = RNA_def_enum(ot->srna, "output", rna_enum_dummy_NULL_items, 0, "Output", "");
  RNA_def_enum_funcs(ot->prop, portal_output_items);
}

static bool portal_supports_type(const eNodeSocketDatatype type)
{
  return ELEM(type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA, SOCK_SHADER);
}

static bool portal_link_can_convert(const bNodeLink &link)
{
  return (link.flag & NODE_LINK_VALID) && !link.is_muted() && !bke::node_link_is_hidden(link) &&
         !link.fromnode->is_muted() && !link.fromnode->is_dangling_reroute() &&
         (portal_supports_type(link.fromsock->type) ||
          (ELEM(link.fromsock->type, SOCK_INT, SOCK_BOOLEAN) &&
           portal_supports_type(link.tosock->type)));
}

static bNodeSocket &portal_socket(bNode &node, const bool input, const int type)
{
  for (bNodeSocket &socket : input ? node.inputs : node.outputs) {
    if (socket.type == type) {
      return socket;
    }
  }
  BLI_assert_unreachable();
  return *(input ? node.inputs : node.outputs).first();
}

static wmOperatorStatus portal_from_link(bContext *C, wmOperator *op, bNodeLink &link)
{
  bNodeTree &tree = portal_tree(C);
  if (!portal_link_can_convert(link) || !tree.zones()) {
    BKE_report(op->reports, RPT_WARNING, "Choose a valid, unmuted shader link with a supported type");
    return OPERATOR_CANCELLED;
  }
  bNode &source = *link.fromnode;
  bNode &target = *link.tonode;
  bNodeSocket &source_socket = *link.fromsock;
  const int type = portal_supports_type(source_socket.type) ? source_socket.type : link.tosock->type;
  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));
  bNode &in = *bke::node_add_static_node(C, tree, SH_NODE_PORTAL_IN);
  bNode &out = *bke::node_add_static_node(C, tree, SH_NODE_PORTAL_OUT);
  NodeShaderPortal &in_data = *portal_storage(&in, SH_NODE_PORTAL_IN);
  NodeShaderPortal &out_data = *portal_storage(&out, SH_NODE_PORTAL_OUT);
  in_data.data_type = out_data.data_type = type;
  const std::string name = BLI_uniquename_cb(
      [&](const StringRef candidate) {
        for (const bNode &node : tree.nodes) {
          if (&node == &in || &node == &out ||
              !ELEM(node.type_legacy, SH_NODE_PORTAL_IN, SH_NODE_PORTAL_OUT) || !node.storage)
          {
            continue;
          }
          if (candidate == static_cast<const NodeShaderPortal *>(node.storage)->name) {
            return true;
          }
        }
        return false;
      },
      '.',
      in_data.name);
  STRNCPY(in_data.name, name.c_str());
  STRNCPY(out_data.name, in_data.name);
  in.typeinfo->updatefunc(&tree, &in);
  out.typeinfo->updatefunc(&tree, &out);
  in.location[0] = source.location[0] + source.width + 30.0f;
  in.location[1] = source.location[1];
  out.location[0] = target.location[0] - out.width - 30.0f;
  out.location[1] = target.location[1];
  bke::node_add_link(tree, source, source_socket, in, portal_socket(in, true, type));
  link.fromnode = &out;
  link.fromsock = &portal_socket(out, false, type);
  BKE_ntree_update_tag_link_changed(&tree);
  node_select_single(*C, in);
  bke::node_set_selected(out, true);
  tree_draw_order_update(tree);
  portal_update(C, tree);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus portal_from_link_exec(bContext *C, wmOperator *op)
{
  bNodeTree &tree = portal_tree(C);
  tree.ensure_topology_cache();
  bNodeLink *selected = nullptr;
  for (bNodeLink &link : tree.links) {
    if (link.fromnode->is_selected() && link.tonode->is_selected() &&
        !bke::node_link_is_hidden(link))
    {
      if (selected) {
        selected = nullptr;
        break;
      }
      selected = &link;
    }
  }
  if (!selected) {
    BKE_report(op->reports, RPT_WARNING, "Hover a wire or select the endpoints of exactly one wire");
    return OPERATOR_CANCELLED;
  }
  return portal_from_link(C, op, *selected);
}

static wmOperatorStatus portal_from_link_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const ARegion *region = CTX_wm_region(C);
  if (region && region->regiontype == RGN_TYPE_WINDOW) {
    bNodeTree &tree = portal_tree(C);
    tree.ensure_topology_cache();
    bNodeLink *closest = nullptr;
    float min_dist_squared = 64.0f;
    const float2 cursor(float(event->mval[0]), float(event->mval[1]));
    for (bNodeLink &link : tree.links) {
      if (bke::node_link_is_hidden(link)) {
        continue;
      }
      std::array<float2, NODE_LINK_RESOL + 1> points;
      node_link_bezier_points_evaluated(link, points);
      for (float2 &point : points) {
        ui::view2d_view_to_region_fl(&region->v2d, point.x, point.y, &point.x, &point.y);
      }
      for (const int index : IndexRange(points.size() - 1)) {
        const float dist_squared = dist_squared_to_line_segment_v2(
            cursor, points[index], points[index + 1]);
        if (dist_squared < min_dist_squared) {
          closest = &link;
          min_dist_squared = dist_squared;
        }
      }
    }
    if (closest) {
      return portal_from_link(C, op, *closest);
    }
  }
  return portal_from_link_exec(C, op);
}

void NODE_OT_shader_portal_from_link(wmOperatorType *ot)
{
  ot->name = "Convert Link to Portal";
  ot->idname = "NODE_OT_shader_portal_from_link";
  ot->description = "Replace the wire under the cursor or between selected nodes with a Portal pair";
  ot->poll = portal_editable;
  ot->invoke = portal_from_link_invoke;
  ot->exec = portal_from_link_exec;
  ot->flag = OPTYPE_UNDO;
}

static wmOperatorStatus portal_to_links_exec(bContext *C, wmOperator *op)
{
  bNodeTree &tree = portal_tree(C);
  tree.ensure_topology_cache();
  const bke::bNodeTreeZones *zones = tree.zones();
  if (!zones) {
    BKE_report(op->reports, RPT_WARNING, "Repair cyclic or invalid zone links before restoring wires");
    return OPERATOR_CANCELLED;
  }
  struct RestoreOutput {
    bNode *out;
    bNodeSocket *source;
    Vector<bNodeLink *> links;
  };
  Vector<RestoreOutput> outputs;
  for (bNode *out : tree.all_nodes()) {
    const NodeShaderPortal *data = portal_storage(out, SH_NODE_PORTAL_OUT);
    if (!data || !out->is_selected()) {
      continue;
    }
    const eNodeSocketDatatype portal_type = eNodeSocketDatatype(data->data_type);
    const bNodeSocket *source = bke::node_tree_runtime::find_shader_portal_origin_socket(tree, *out);
    if (!source || !portal_supports_type(portal_type)) {
      BKE_reportf(op->reports, RPT_WARNING, "Portal Out '%s' has no connected source", out->name);
      return OPERATOR_CANCELLED;
    }
    RestoreOutput restore{out, const_cast<bNodeSocket *>(source), {}};
    for (bNodeLink &link : tree.links) {
      if (link.fromnode != out) {
        continue;
      }
      if (link.fromsock->type != portal_type || !(link.flag & NODE_LINK_VALID) ||
          !zones->link_between_sockets_is_allowed(*source, *link.tosock) ||
          !tree.typeinfo->validate_link(source->type, portal_type) ||
          !tree.typeinfo->validate_link(source->type, link.tosock->type))
      {
        BKE_reportf(op->reports, RPT_WARNING, "Cannot restore all links from '%s' safely", out->name);
        return OPERATOR_CANCELLED;
      }
      restore.links.append(&link);
    }
    if (!restore.links.is_empty()) {
      outputs.append(std::move(restore));
    }
  }
  if (outputs.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  Main &bmain = *CTX_data_main(C);
  ED_preview_kill_jobs(CTX_wm_manager(C), &bmain);
  for (RestoreOutput &restore : outputs) {
    bNode *from_node = &restore.source->owner_node();
    float x = restore.out->location[0];
    bNodeSocket *converted = nodes::add_shader_portal_conversion(
        tree,
        *from_node,
        *restore.source,
        restore.source->type,
        eNodeSocketDatatype(portal_storage(restore.out, SH_NODE_PORTAL_OUT)->data_type),
        [&](const UString type) -> bNode & {
          bNode &node = *bke::node_add_node(C, tree, type);
          node.location[0] = x;
          node.location[1] = restore.out->location[1];
          x += node.width + 30.0f;
          from_node = &node;
          return node;
        });
    bNodeSocket *from_socket = converted ? converted : restore.source;
    for (bNodeLink *link : restore.links) {
      link->fromnode = from_node;
      link->fromsock = from_socket;
    }
    BKE_ntree_update_tag_link_changed(&tree);
    bke::node_remove_node(&bmain, tree, *restore.out, true);
  }
  portal_update(C, tree);
  return OPERATOR_FINISHED;
}

void NODE_OT_shader_portal_to_links(wmOperatorType *ot)
{
  ot->name = "Restore Portal Wires";
  ot->idname = "NODE_OT_shader_portal_to_links";
  ot->description = "Replace selected Portal Out nodes with wires from their sources";
  ot->poll = portal_editable;
  ot->exec = portal_to_links_exec;
  ot->flag = OPTYPE_UNDO;
}

}  // namespace blender::ed::space_node
