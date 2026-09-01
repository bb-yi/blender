/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"

namespace blender::gpu {
class Texture;
}

namespace blender::eevee {

struct Dlss5NRSettings {
  float intensity = 1.0f;
  float local_tone_strength = 1.0f;
  float local_structure_strength = 1.0f;
  float skin_structure_strength = -1.0f;
  bool use_auto_mask = false;
  bool ui_correction = false;
};

struct Dlss5D3D12Frame {
  gpu::Texture *color = nullptr;
  gpu::Texture *depth = nullptr;
  gpu::Texture *velocity = nullptr;
  int2 input_extent = int2(0);
  int2 output_extent = int2(0);
  float2 jitter = float2(0.0f);
  bool reset_history = false;
  bool color_is_scene_linear = true;
  bool depth_is_reverse_z = true;
  bool velocity_is_pixel_space = true;
  float exposure_scale = 1.0f;
  Dlss5NRSettings settings;
};

/**
 * Windows D3D12/NGX session used by the optional EEVEE DLSS5 adapter.
 *
 * The class is deliberately a no-op on unsupported platforms or backends.
 * D3D12-owned resources are imported into the active Vulkan backend through
 * GPU_texture_create_2d_from_external().
 */
class Dlss5D3D12Session {
 private:
  struct Impl;
  Impl *impl_ = nullptr;

 public:
  Dlss5D3D12Session();
  ~Dlss5D3D12Session();

  Dlss5D3D12Session(const Dlss5D3D12Session &) = delete;
  Dlss5D3D12Session &operator=(const Dlss5D3D12Session &) = delete;

  bool ensure_resources(int2 input_extent, int2 output_extent);
  bool copy_inputs_and_evaluate(const Dlss5D3D12Frame &frame,
                                bool copy_color = true,
                                bool copy_velocity = true);
  bool wait_for_output();
  void reset();

  gpu::Texture *color_texture() const;
  gpu::Texture *depth_texture() const;
  gpu::Texture *velocity_texture() const;
  gpu::Texture *output_texture() const;

  bool available() const;
  const char *status() const;
};

}  // namespace blender::eevee
