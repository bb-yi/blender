/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

namespace blender {

struct Main;
struct bNodeTree;

bNodeTree *BKE_npr_tree_add(Main *bmain, const char *name);

}  // namespace blender
