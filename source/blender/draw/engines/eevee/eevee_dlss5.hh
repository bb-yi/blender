/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Optional DLSS5 integration boundary for EEVEE.
 *
 * The first implementation deliberately remains pass-through. It records the
 * exact EEVEE resources and frame metadata that a future D3D12 executor needs,
 * without adding an NGX dependency to Blender's default build.
 */

#pragma once

#include "BLI_math_vector_types.hh"

namespace blender::gpu {
class Texture;
}

namespace blender::eevee {

class Instance;

struct Dlss5FrameInputs {
  gpu::Texture *color = nullptr;
  gpu::Texture *depth = nullptr;
  gpu::Texture *velocity = nullptr;
  int2 input_extent = int2(0);
  int2 output_extent = int2(0);
  float2 jitter = float2(0.0f);
  bool reset_history = false;
  bool is_viewport = false;
  bool color_is_scene_linear = true;
  bool depth_is_reverse_z = true;
  bool velocity_is_packed = true;
  bool velocity_is_pixel_space = false;
  float exposure_scale = 1.0f;
};

/**
 * EEVEE-side contract for an optional DLSS5 executor.
 *
 * The executor must return a texture with output_extent. Until a backend is
 * implemented, process() returns color unchanged. Keeping the boundary here
 * lets us validate ordering and input metadata without changing rendering.
 */
class Dlss5Module {
 private:
  Instance &inst_;
  bool reported_ = false;

 public:
  explicit Dlss5Module(Instance &inst) : inst_(inst) {}

  gpu::Texture *process(const Dlss5FrameInputs &inputs);

  bool available() const
  {
    return false;
  }
};

}  // namespace blender::eevee
