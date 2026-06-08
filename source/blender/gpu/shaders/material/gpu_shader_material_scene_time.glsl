/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_scene_time(float scale,
                     float &frame,
                     float &seconds,
                     float &timeline,
                     float &scaled_frame)
{
  scene_time_uniforms(seconds, frame);
  timeline = uniform_buf.scene_time.timeline;

  float scale_safe = (abs(scale) > 1e-8f) ? scale : 1.0f;
  scaled_frame = frame / scale_safe;
}
