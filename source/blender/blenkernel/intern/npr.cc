/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "DNA_material_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLT_translation.hh"

#include "MEM_guardedalloc.h"

#include "NOD_shader.h"

namespace blender {

bNodeTree *BKE_npr_tree_add(Main *bmain, const char *name)
{
  bNodeTree *ntree = bke::node_tree_add_tree(bmain, name, ntreeType_Shader->idname);

  bNode *input = bke::node_add_node(nullptr, *ntree, "ShaderNodeNPR_Input");
  bNode *output = bke::node_add_node(nullptr, *ntree, "ShaderNodeNPR_Output");

  input->location[0] = -220.0f;
  input->location[1] = 40.0f;
  output->location[0] = 180.0f;
  output->location[1] = 40.0f;

  bke::node_add_link(*ntree,
                     *input,
                     *bke::node_find_socket(*input, SOCK_OUT, "Combined Color"),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Color"));

  bke::node_set_active(*ntree, *output);
  BKE_ntree_update_after_single_tree_change(*bmain, *ntree);
  return ntree;
}

MaterialNPRLayer *BKE_material_npr_layer_add(Material *material, bNodeTree *tree)
{
  MaterialNPRLayer *layer = MEM_new_zeroed<MaterialNPRLayer>("MaterialNPRLayer");
  layer->enabled = true;
  layer->uid = BLI_listbase_count(&material->npr_layers) + 1;
  STRNCPY(layer->name, DATA_("NPR Layer"));
  BLI_uniquename(&material->npr_layers,
                 layer,
                 DATA_("NPR Layer"),
                 '.',
                 offsetof(MaterialNPRLayer, name),
                 sizeof(layer->name));
  layer->node_tree = tree;
  if (layer->node_tree != nullptr) {
    id_us_plus(&layer->node_tree->id);
  }
  BLI_addtail(&material->npr_layers, layer);
  material->active_npr_layer_index = BLI_findindex(&material->npr_layers, layer);
  return layer;
}

void BKE_material_npr_layer_remove(Material *material, MaterialNPRLayer *layer)
{
  if (layer->node_tree != nullptr) {
    id_us_min(&layer->node_tree->id);
  }
  BLI_remlink(&material->npr_layers, layer);
  MEM_delete(layer);
  material->active_npr_layer_index = min_ii(
      material->active_npr_layer_index, BLI_listbase_count(&material->npr_layers) - 1);
}

}  // namespace blender
