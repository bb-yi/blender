/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

namespace blender {

struct bContext;
struct bNodeTree;
struct ID;
struct Main;
struct Scene;

namespace nodes {

/**
 * Assumes nothing being done in ntree yet, sets the default in/out node.
 * Called from shading buttons or header.
 */
void node_tree_shader_default(const bContext *C, Main *bmain, ID *id);

/**
 * Adds the default Eevee light shader nodes to a light shader tree if they are missing.
 */
bool node_tree_light_shader_default_ensure(bNodeTree &ntree);

/**
 * Assumes nothing being done in ntree yet, sets the default in/out node.
 * Called from compositing buttons or header.
 */
void node_tree_composit_default(const bContext *C, Scene *sce);

/**
 * Initializes an empty compositing node tree with default nodes.
 */
void node_tree_composit_default_init(const bContext *C, bNodeTree *ntree);

}  // namespace nodes
}  // namespace blender
