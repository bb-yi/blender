/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "eevee_dlss5.hh"

namespace blender {
struct Scene;
struct ViewLayer;
struct View3D;
}  // namespace blender

namespace blender::eevee {

/** SuperSampling (NGX feature 1), independent of feature-18 Neural Rendering. */
class DlssSrModule {
  Instance &inst_;
  Dlss5D3D12Session session_{true};
  bool active_ = false;
  bool failed_ = false;
  bool reset_ = true;
  int quality_ = 0;
  float percentage_ = 0.0f;
  int evaluated_samples_ = 0;
  int2 input_extent_ = int2(0);
  int2 output_extent_ = int2(0);
  draw::Framebuffer convert_fb_{"DLSS.SR.Convert"};
  draw::PassSimple convert_ps_{"DLSS.SR.Convert"};
  void publish_status(const char *status);

 public:
  explicit DlssSrModule(Instance &inst) : inst_(inst) {}
  /** Returns a static precision/format restriction, or nullptr. Never changes scene settings. */
  static const char *fallback_reason(const Scene &scene,
                                     const ViewLayer &layer,
                                     const View3D *v3d);
  void init(int2 output_extent);
  gpu::Texture *process(gpu::Texture *color, gpu::Texture *outline, draw::View &view);
  void invalidate()
  {
    reset_ = true;
  }
  bool active() const
  {
    return active_;
  }
  bool failed() const
  {
    return failed_;
  }
  int2 input_extent() const
  {
    return input_extent_;
  }
  /** Keep the failure latched while reinitializing the frame at native resolution. */
  void use_native_after_failure()
  {
    active_ = false;
  }
  void readback_complete();
};

}  // namespace blender::eevee
