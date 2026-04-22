/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DRW_gpu_wrapper.hh"
#include "draw_pass.hh"

namespace blender::eevee {

using namespace draw;

class Instance;

class OutlineModule {
 private:
  Instance &inst_;
  bool enabled_ = false;

  PassSimple detect_ps_ = {"Outline.Detect"};
  PassSimple jfa_init_ps_ = {"Outline.JFA.Init"};
  PassSimple jfa_step_ps_ = {"Outline.JFA.Step"};
  PassSimple resolve_ps_ = {"Outline.Resolve"};

  Framebuffer detect_fb_ = {"Outline.Detect.FB"};
  Framebuffer jfa_init_fb_ = {"Outline.JFA.Init.FB"};

  TextureFromPool edge_seed_tx_ = {"Outline.EdgeSeed"};
  SwapChain<TextureFromPool, 2> jfa_tx_;

  int jfa_step_size_ = 1;
  int3 jfa_dispatch_size_ = int3(1);

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
