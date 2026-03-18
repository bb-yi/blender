/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_node.hh"

#include "NOD_shader.h"

namespace blender {

bNodeTree *BKE_npr_tree_add(Main *bmain, const char *name)
{
  return bke::node_tree_add_tree(bmain, name, ntreeType_Shader->idname);
}

}  // namespace blender
