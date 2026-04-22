/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_pass.hh"

namespace blender::eevee {

using namespace draw;

class Instance;

class OutlineModule {
 private:
  Instance &inst_;
  bool enabled_ = false;

  PassSimple detect_ps_ = {"Outline.Detect"};
  PassSimple expand_ps_ = {"Outline.Expand"};
  Framebuffer framebuffer_ = {"Outline.Framebuffer"};
  TextureFromPool edge_seed_tx_ = {"Outline.EdgeSeed"};

 public:
  OutlineModule(Instance &inst) : inst_(inst) {}

  void sync();
  void render(View &view, Framebuffer &combined_fb, int2 extent);

  bool enabled() const
  {
    return enabled_;
  }
};

}  // namespace blender::eevee
