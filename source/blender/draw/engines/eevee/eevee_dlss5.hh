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

#include <memory>

#include "BLI_math_vector_types.hh"

#include "DRW_gpu_wrapper.hh"
#include "draw_pass.hh"

#include "dlss5_d3d12.hh"

namespace blender::gpu {
class Texture;
}

namespace blender::eevee {

class Instance;

struct Dlss5FrameInputs {
  gpu::Texture *color = nullptr;
  gpu::Texture *base_color = nullptr;
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
  bool active_reported_ = false;
  bool failure_reported_ = false;
  bool retry_blocked_ = false;
  int2 retry_input_extent_ = int2(-1);
  int2 retry_output_extent_ = int2(-1);
  std::unique_ptr<Dlss5D3D12Session> d3d12_session_;
  gpu::Texture *last_display_texture_ = nullptr;
  draw::Texture scene_linear_output_tx_ = {"DLSS5.SceneLinearOutput"};
  draw::Framebuffer display_color_fb_ = {"DLSS5.DisplayColor"};
  draw::PassSimple color_convert_ps_ = {"DLSS5.ColorConvert"};
  draw::Framebuffer hdr_reconstruct_fb_ = {"DLSS5.HDRReconstruct"};
  draw::PassSimple hdr_reconstruct_ps_ = {"DLSS5.HDRReconstruct"};
  draw::Framebuffer depth_convert_fb_ = {"DLSS5.DepthConvert"};
  draw::PassSimple depth_convert_ps_ = {"DLSS5.DepthConvert"};
  draw::Framebuffer velocity_convert_fb_ = {"DLSS5.VelocityConvert"};
  draw::PassSimple velocity_convert_ps_ = {"DLSS5.VelocityConvert"};
  bool color_convert_inverse_ = false;

  bool prepare_display_color(gpu::Texture *source, gpu::Texture *destination);
  bool reconstruct_scene_linear(gpu::Texture *source,
                                gpu::Texture *input,
                                gpu::Texture *original,
                                gpu::Texture *destination);
  bool prepare_velocity(gpu::Texture *source, gpu::Texture *destination, int2 extent);

 public:
  explicit Dlss5Module(Instance &inst);
  ~Dlss5Module();

  gpu::Texture *process(const Dlss5FrameInputs &inputs);

  bool available() const;
  const char *status() const;
  gpu::Texture *display_texture() const
  {
    return last_display_texture_;
  }
};

}  // namespace blender::eevee
