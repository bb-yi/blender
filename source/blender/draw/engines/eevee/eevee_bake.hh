/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender {

struct Depsgraph;
struct Object;
struct RenderEngine;

namespace eevee {

void eevee_bake(RenderEngine *engine,
                Depsgraph *depsgraph,
                Object *object,
                int pass_type,
                int pass_filter,
                int width,
                int height);

}  // namespace eevee
}  // namespace blender
