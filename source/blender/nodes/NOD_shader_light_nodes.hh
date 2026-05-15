/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_string_ref.hh"

namespace blender {

struct bContext;

namespace nodes {

bool light_eevee_shader_nodes_poll(const bContext *C);
bool light_eevee_shader_node_type_supported(StringRefNull idname);

}  // namespace nodes
}  // namespace blender
