/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Optional DLSS5 integration boundary for EEVEE.
 *
 * Windows Vulkan/D3D12 interop for Neural Rendering of the resolved Film image.
 * Unsupported backends preserve native EEVEE output.
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
  int2 guide_extent = int2(0);
  int guide_overscan = 0;
  int guide_scale = 1;
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
 * The executor returns a scene-linear texture with output_extent, or the input
 * on failure. Native Film history remains separate from the processed output.
 */
class Dlss5Module {
 private:
  Instance &inst_;
  bool reported_ = false;
  bool active_reported_ = false;
  bool failure_reported_ = false;
  bool retry_blocked_ = false;
  bool force_history_reset_ = true;
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

  void publish_status(bool viewport, const char *status);
  bool prepare_display_color(gpu::Texture *source, gpu::Texture *destination);
  bool reconstruct_scene_linear(gpu::Texture *source,
                                gpu::Texture *input,
                                gpu::Texture *original,
                                gpu::Texture *destination,
                                float intensity);
  bool prepare_velocity(const Dlss5FrameInputs &inputs, gpu::Texture *destination, draw::View &view);

 public:
  explicit Dlss5Module(Instance &inst);
  ~Dlss5Module();

  gpu::Texture *process(const Dlss5FrameInputs &inputs, draw::View &view);
  /* Load D3D12 + nvngx_dlssnr.dll once. Games do this at startup, not on toggle. */
  void warmup();
  void render_readback_complete();

  void invalidate()
  {
    last_display_texture_ = nullptr;
    retry_blocked_ = false;
    force_history_reset_ = true;
  }
  bool available() const;
  const char *status() const;
  gpu::Texture *display_texture() const;
};

}  // namespace blender::eevee
