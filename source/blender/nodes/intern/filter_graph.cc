/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase_wrapper.hh"
#include "BLI_string.h"

#include "BKE_main.hh"
#include "BKE_main_invariants.hh"

#include "DEG_depsgraph.hh"

#include "NOD_filter_graph.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::nodes {

void filter_graph_tag_tree_changed(Main &bmain, bNodeTree &ntree)
{
  BKE_main_ensure_invariants(bmain, ntree.id);
  DEG_id_tag_update(&ntree.id, ID_RECALC_SYNC_TO_EVAL);
  WM_main_add_notifier(NC_NODE | NA_EDITED, &ntree);

  if (!STREQ(ntree.idname, eevee_filter_graph_tree_idname.c_str())) {
    return;
  }

  for (Scene &scene : bmain.scenes) {
    if (scene.eevee.filter_graph != &ntree) {
      continue;
    }
    DEG_id_tag_update(&scene.id, ID_RECALC_SYNC_TO_EVAL);
    WM_main_add_notifier(NC_SCENE | ND_NODES, &scene);
    WM_main_add_notifier(NC_SCENE | ND_RENDER_OPTIONS, &scene);
  }
}

}  // namespace blender::nodes
