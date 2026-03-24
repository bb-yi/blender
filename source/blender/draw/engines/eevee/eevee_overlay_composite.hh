/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include "DNA_scene_types.h"

#include "DRW_gpu_wrapper.hh"
#include "DRW_render.hh"

#include "BLI_vector.hh"

namespace blender::draw {
class View;
}

namespace blender::eevee {

using namespace draw;

class Instance;

class OverlayCompositeModule {
 private:
  struct OverlayPassEntry {
    SceneOverlayInput *scene_input = nullptr;
    blender::Image *color_image = nullptr;
    gpu::Texture *color_tx = nullptr;
  };

  Instance &inst_;

  blender::Vector<OverlayPassEntry> entries_;
  Framebuffer framebuffer_ = {"OverlayComposite.Framebuffer"};
  Texture ping_tx_ = {"OverlayComposite.Ping"};
  Texture pong_tx_ = {"OverlayComposite.Pong"};

 public:
  OverlayCompositeModule(Instance &inst) : inst_(inst) {}

  void init() {}
  void begin_sync();
  void end_sync() {}

  bool enabled() const
  {
    return !entries_.is_empty();
  }

  void render(draw::View &view, gpu::Texture **input_tx, gpu::Texture **output_tx);
};

}  // namespace blender::eevee
