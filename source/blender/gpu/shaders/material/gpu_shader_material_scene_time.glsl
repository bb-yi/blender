/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_scene_time(float scale,
                     out float frame,
                     out float seconds,
                     out float timeline,
                     out float scaled_frame)
{
  frame = uniform_buf.scene_time.frame;
  seconds = uniform_buf.scene_time.seconds;
  timeline = uniform_buf.scene_time.timeline;

  float scale_safe = (abs(scale) > 1e-8f) ? scale : 1.0f;
  scaled_frame = frame / scale_safe;
}
