/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

#include "BLI_map.hh"
#include "BLI_vector.hh"
#include "DRW_gpu_wrapper.hh"
#include "draw_handle.hh"
#include "draw_pass.hh"
#include "eevee_defines.hh"

struct GPUMaterial;

namespace blender::eevee {

class Instance;

/* Camera-only, undithered body depth. Each draw-manager instance has its own layer, including
 * instances sharing mesh data and materials. It never enters scene depth or user stencil. */
class OutlineShellModule {
 private:
  struct ObjectDepth {
    draw::PassMain pass = {"OutlineShell.SelfDepth"};
  };

  Instance &inst_;
  Map<uint, int> object_layers_;
  Vector<std::unique_ptr<ObjectDepth>> objects_;
  int object_count_ = 0;
  bool has_objects_ = false;
  bool layers_dirty_ = true;
  draw::StorageArrayBuffer<int, 16> layers_ = {"outline_shell_layer_buf"};
  draw::TextureFromPool depth_tx_ = {"OutlineShell.SelfDepth"};
  draw::Framebuffer depth_fb_ = {"OutlineShell.SelfDepth"};

 public:
  explicit OutlineShellModule(Instance &inst) : inst_(inst) {}

  void begin_sync();
  draw::PassMain::Sub *material_add(draw::ResourceHandle handle, GPUMaterial *gpumat);
  void render(draw::View &view, int2 extent);
  void release()
  {
    depth_tx_.release();
  }

  template<typename PassType> void bind_resources(PassType &pass)
  {
    pass.bind_texture(OUTLINE_SHELL_DEPTH_TEX_SLOT, &depth_tx_);
    pass.bind_ssbo(OUTLINE_SHELL_LAYER_BUF_SLOT, &layers_);
  }
};

}  // namespace blender::eevee
