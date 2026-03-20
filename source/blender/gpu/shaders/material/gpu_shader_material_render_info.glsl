/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_render_info(out float width, out float height)
{
  width = float(uniform_buf.film.extent.x);
  height = float(uniform_buf.film.extent.y);
}
