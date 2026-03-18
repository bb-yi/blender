/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

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

}  // namespace blender
