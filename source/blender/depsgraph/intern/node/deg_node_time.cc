/* SPDX-FileCopyrightText: 2013 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#include "intern/node/deg_node_time.hh"

#include "BKE_modifier.hh"
#include "DNA_object_types.h"

#include "intern/depsgraph.hh"
#include "intern/depsgraph_relation.hh"
#include "intern/eval/deg_eval_copy_on_write.h"
#include "intern/node/deg_node_component.hh"
#include "intern/node/deg_node_id.hh"
#include "intern/node/deg_node_operation.hh"

#include "CLG_log.h"

namespace blender::deg {

static bool keep_modifier_geometry_on_time_change(const Depsgraph &graph, const Node &node)
{
  if (graph.mode != DAG_EVAL_VIEWPORT || !graph.is_active ||
      node.get_class() != NodeClass::OPERATION)
  {
    return false;
  }
  const auto &operation = static_cast<const OperationNode &>(node);
  if (operation.opcode != OperationCode::MODIFIER) {
    return false;
  }
  const ID *id = operation.owner->owner->id_cow;
  if (!id || GS(id->name) != ID_OB || !deg_eval_copy_is_expanded(id)) {
    return false;
  }
  const auto &object = reinterpret_cast<const Object &>(*id);
  const ModifierData *modifier = BKE_modifiers_findby_name(&object, operation.name.c_str());
  if (object.mode != OB_MODE_OBJECT || !modifier ||
      !BKE_modifier_has_time_independent_instances(*modifier))
  {
    return false;
  }
  static CLG_LogRef log = {"geometry_nodes.time_dependency"};
  CLOG_INFO(&log, "Reusing %s / %s on frame change", object.id.name + 2, modifier->name);
  return true;
}

void TimeSourceNode::tag_update(Depsgraph * /*graph*/, eUpdateSource /*source*/)
{
  tagged_for_update = true;
}

void TimeSourceNode::flush_update_tag(Depsgraph *graph)
{
  if (!tagged_for_update) {
    return;
  }
  for (Relation *rel : outlinks) {
    Node *node = rel->to;
    /* Keep all other dependency links. A change in inputs can still schedule the modifier and
     * replace its time-dependence result, including when a disabled branch is enabled. */
    if (keep_modifier_geometry_on_time_change(*graph, *node)) {
      continue;
    }
    node->tag_update(graph, DEG_UPDATE_SOURCE_TIME);
  }
}

}  // namespace blender::deg
