/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_main_invariants.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_listbase.h"

#include "DEG_depsgraph.hh"

#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "GPU_material.hh"

#include "NOD_defaults.hh"

#include "WM_api.hh"
#include "WM_types.hh"
#include <optional>

namespace blender {

static void send_notifiers_after_node_tree_change(ID *id, bNodeTree *ntree)
{
  WM_main_add_notifier(NC_NODE | NA_EDITED, id);

  if (ntree->type == NTREE_SHADER && id != nullptr) {
    if (GS(id->name) == ID_MA) {
      WM_main_add_notifier(NC_MATERIAL | ND_SHADING, id);
    }
    else if (GS(id->name) == ID_LA) {
      WM_main_add_notifier(NC_LAMP | ND_LIGHTING, id);
    }
    else if (GS(id->name) == ID_WO) {
      WM_main_add_notifier(NC_WORLD | ND_WORLD, id);
    }
  }
  else if (ntree->type == NTREE_COMPOSIT) {
    WM_main_add_notifier(NC_SCENE | ND_NODES, id);
  }
  else if (ntree->type == NTREE_TEXTURE) {
    WM_main_add_notifier(NC_TEXTURE | ND_NODES, id);
  }
  else if (ntree->type == NTREE_GEOMETRY) {
    WM_main_add_notifier(NC_OBJECT | ND_MODIFIER, id);
    if (ntree->geometry_node_asset_traits) {
      if (ntree->geometry_node_asset_traits->flag & GEO_NODE_ASSET_TOOL) {
        /* Notifier to re-register node group operators. */
        WM_main_add_notifier(NC_NODE | ND_NODE_ASSET_DATA, id);
      }
    }
  }
}

static void propagate_node_tree_changes(Main &bmain,
                                        const std::optional<Span<ID *>> modified_ids,
                                        const Span<bNodeTree *> extra_modified_trees)
{
  NodeTreeUpdateExtraParams params;
  params.tree_changed_fn = [&bmain](bNodeTree &ntree, ID &owner_id) {
    send_notifiers_after_node_tree_change(&owner_id, &ntree);
    DEG_id_tag_update(&ntree.id, ID_RECALC_SYNC_TO_EVAL);

    if (ntree.type != NTREE_SHADER || GS(owner_id.name) != ID_MA) {
      if (ntree.type == NTREE_SHADER && GS(owner_id.name) == ID_LA) {
        Light &light = reinterpret_cast<Light &>(owner_id);
        GPU_material_free(&light.gpumaterial);
        DEG_id_tag_update(&light.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
      }
      return;
    }

    Material &material = reinterpret_cast<Material &>(owner_id);
    if (material.eevee_domain != MA_EEVEE_DOMAIN_FILTER) {
      return;
    }

    GPU_material_free(&material.gpumaterial);
    DEG_id_tag_update(&material.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);

    for (Scene *scene = static_cast<Scene *>(bmain.scenes.first); scene != nullptr;
         scene = scene->id.next ? reinterpret_cast<Scene *>(scene->id.next) : nullptr)
    {
      bool uses_filter_material = false;
      for (SceneFilterMaterial *filter_entry = static_cast<SceneFilterMaterial *>(
               scene->eevee.filter_materials.first);
           filter_entry != nullptr;
           filter_entry = filter_entry->next)
      {
        if (filter_entry->material == &material) {
          uses_filter_material = true;
          break;
        }
      }
      if (!uses_filter_material) {
        continue;
      }
      DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
      WM_main_add_notifier(NC_SCENE | ND_RENDER_OPTIONS, scene);
    }
  };
  params.tree_output_changed_fn = [](bNodeTree &ntree, ID & /*owner_id*/) {
    DEG_id_tag_update(&ntree.id, ID_RECALC_NTREE_OUTPUT);
  };

  std::optional<Vector<bNodeTree *>> modified_trees;
  if (modified_ids.has_value()) {
    modified_trees.emplace();
    for (ID *id : *modified_ids) {
      if (GS(id->name) == ID_NT) {
        modified_trees->append(reinterpret_cast<bNodeTree *>(id));
      }
    }
    modified_trees->extend(extra_modified_trees);
  }

  BKE_ntree_update(bmain, modified_trees, params);
}

static bNodeTree *ensure_light_default_shader_nodes(Light &light)
{
  bNodeTree *ntree = light.nodetree;
  if (ntree == nullptr || ntree->type != NTREE_SHADER) {
    return nullptr;
  }
  if (!nodes::node_tree_light_shader_default_ensure(*ntree)) {
    return nullptr;
  }
  BKE_ntree_update_tag_all(ntree);
  return ntree;
}

static Vector<bNodeTree *> ensure_default_light_shader_nodes(
    Main &bmain, const std::optional<Span<ID *>> modified_ids)
{
  Vector<bNodeTree *> changed_trees;
  if (modified_ids.has_value()) {
    for (ID *id : *modified_ids) {
      if (GS(id->name) == ID_LA) {
        if (bNodeTree *ntree = ensure_light_default_shader_nodes(reinterpret_cast<Light &>(*id))) {
          changed_trees.append(ntree);
        }
      }
    }
    return changed_trees;
  }

  for (Light *light = static_cast<Light *>(bmain.lights.first); light != nullptr;
       light = static_cast<Light *>(light->id.next))
  {
    if (bNodeTree *ntree = ensure_light_default_shader_nodes(*light)) {
      changed_trees.append(ntree);
    }
  }
  return changed_trees;
}

void BKE_main_ensure_invariants(Main &bmain, const std::optional<Span<ID *>> modified_ids)
{
  Vector<bNodeTree *> changed_trees = ensure_default_light_shader_nodes(bmain, modified_ids);
  propagate_node_tree_changes(bmain, modified_ids, changed_trees);
}

void BKE_main_ensure_invariants(Main &bmain, ID &modified_id)
{
  BKE_main_ensure_invariants(bmain, {{&modified_id}});
}

}  // namespace blender
