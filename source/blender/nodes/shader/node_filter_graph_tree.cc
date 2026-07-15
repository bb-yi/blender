/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include <array>

#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "BLI_index_range.hh"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "NOD_filter_graph.hh"

#include "RNA_prototypes.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "node_common.h"

namespace blender::nodes {

static bool is_eevee_filter_graph_tree(const bNodeTree *ntree)
{
  return ntree != nullptr && STREQ(ntree->idname, eevee_filter_graph_tree_idname.c_str());
}

static bool is_stage_output(const bNode &node)
{
  return node.type_legacy == EEVEE_FILTER_GRAPH_NODE_STAGE_OUTPUT;
}

static int stage_index(const int stage)
{
  switch (stage) {
    case SCE_EEVEE_FILTER_STAGE_BEFORE_DEPTH_OF_FIELD:
    case SCE_EEVEE_FILTER_STAGE_BEFORE_COMPOSITE:
    case SCE_EEVEE_FILTER_STAGE_BEFORE_VOLUME_FOG:
    case SCE_EEVEE_FILTER_STAGE_BEFORE_POSTFX:
      return stage;
  }
  return -1;
}

void filter_graph_stage_output_activate(bNodeTree &ntree, bNode &node)
{
  if (!is_stage_output(node)) {
    return;
  }
  for (bNode *other_node : ntree.all_nodes()) {
    if (is_stage_output(*other_node) && other_node->custom1 == node.custom1) {
      other_node->flag &= ~NODE_DO_OUTPUT;
    }
  }
  node.flag |= NODE_DO_OUTPUT;
}

void filter_graph_stage_outputs_ensure(bNodeTree &ntree)
{
  std::array<bNode *, 4> first_output = {};
  std::array<bNode *, 4> active_output = {};

  for (bNode *node : ntree.all_nodes()) {
    if (!is_stage_output(*node)) {
      continue;
    }
    const int index = stage_index(node->custom1);
    if (index < 0) {
      node->flag &= ~NODE_DO_OUTPUT;
      continue;
    }
    if (first_output[index] == nullptr) {
      first_output[index] = node;
    }
    if (node->flag & NODE_DO_OUTPUT) {
      if (active_output[index] == nullptr) {
        active_output[index] = node;
      }
      else {
        node->flag &= ~NODE_DO_OUTPUT;
      }
    }
  }

  for (const int index : IndexRange(first_output.size())) {
    if (first_output[index] != nullptr && active_output[index] == nullptr) {
      first_output[index]->flag |= NODE_DO_OUTPUT;
    }
  }
}

static void filter_graph_get_from_context(const bContext *C,
                                          bke::bNodeTreeType * /*treetype*/,
                                          bNodeTree **r_ntree,
                                          ID **r_id,
                                          ID **r_from)
{
  Scene *scene = CTX_data_scene(C);
  *r_from = nullptr;
  *r_id = (scene != nullptr) ? &scene->id : nullptr;
  if (scene == nullptr) {
    *r_ntree = nullptr;
    return;
  }

  *r_ntree = scene->eevee.filter_graph;
}

static void foreach_nodeclass(void *calldata, bke::bNodeClassCallback func)
{
  func(calldata, NODE_CLASS_INPUT, N_("Input"));
  func(calldata, NODE_CLASS_OUTPUT, N_("Output"));
  func(calldata, NODE_CLASS_SHADER, N_("Filter"));
  func(calldata, NODE_CLASS_GROUP, N_("Group"));
  func(calldata, NODE_CLASS_LAYOUT, N_("Layout"));
}

static void update(bNodeTree *ntree)
{
  filter_graph_sync_filter_pass_interfaces_from_materials(*ntree);
  filter_graph_stage_outputs_ensure(*ntree);
  ntree_update_reroute_nodes(ntree);
}

static bool filter_graph_validate_link(eNodeSocketDatatype from, eNodeSocketDatatype to)
{
  return from == SOCK_IMAGE && to == SOCK_IMAGE;
}

static bool filter_graph_socket_type_valid(bke::bNodeTreeType * /*ntreetype*/,
                                           bke::bNodeSocketType *socket_type)
{
  return bke::node_is_static_socket_type(*socket_type) && socket_type->type == SOCK_IMAGE;
}

bke::bNodeTreeType *ntreeType_EeveeFilterGraph;

void register_node_tree_type_eevee_filter_graph()
{
  bke::bNodeTreeType *tt = ntreeType_EeveeFilterGraph = MEM_new<bke::bNodeTreeType>(__func__);

  tt->type = NTREE_EEVEE_FILTER_GRAPH;
  tt->idname = UString(eevee_filter_graph_tree_idname);
  tt->group_idname = "EeveeFilterGraphNodeGroup"_ustr;
  tt->ui_name = N_("Eevee Filter Graph");
  tt->ui_icon = ICON_NODE_COMPOSITING;
  tt->ui_description = N_("Build non-linear Eevee fullscreen filter material graphs");

  tt->foreach_nodeclass = foreach_nodeclass;
  tt->update = update;
  tt->get_from_context = filter_graph_get_from_context;
  tt->validate_link = filter_graph_validate_link;
  tt->valid_socket_type = filter_graph_socket_type_valid;
  tt->rna_ext.srna = RNA_EeveeFilterGraphNodeTree;

  bke::node_tree_type_add(*tt);
}

}  // namespace blender::nodes

namespace blender {

void register_node_tree_type_eevee_filter_graph()
{
  nodes::register_node_tree_type_eevee_filter_graph();
}

}  // namespace blender
