/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>
#include <cstdint>
#include <string>

namespace blender::gpu { class Texture; }
namespace blender::draw { class ObjectRef; class View; }
namespace blender::eevee {
class Instance;

class GeometryLineModule {
  struct Impl;
  std::unique_ptr<Impl> impl_;

 public:
  explicit GeometryLineModule(Instance &instance);
  ~GeometryLineModule();
  void begin_sync();
  void sync_object(const draw::ObjectRef &object);
  void end_sync();
  void render(draw::View &view);
  /** Freeze the requested frame before the renderer evaluates shutter subframes. */
  void capture_reference_geometry();
  void reset_accumulation();
  void accumulate(draw::View &view);
  void present(draw::View &view);
  gpu::Texture *composite(draw::View &view, gpu::Texture *scene);
  gpu::Texture *texture() const;
  gpu::Texture *pass_texture() const;
  gpu::Texture *depth_texture() const;
  gpu::Texture *velocity_texture() const;
  float *read_pass();
  bool enabled() const;
  bool use_in_combined() const;
  bool is_quick_preview() const;
  bool uses_aov() const;
  bool uses_aov(uint32_t hash, bool value_type) const;
  std::string timing_info() const;
};

}  // namespace blender::eevee
