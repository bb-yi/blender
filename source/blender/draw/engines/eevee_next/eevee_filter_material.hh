/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include "GPU_material.hh"

#include "eevee_shader_shared.hh"

namespace blender::eevee {

class Instance;

class FilterMaterialModule {
 private:
  Instance &inst_;

  ::Material *material_ = nullptr;
  GPUMaterial *gpumat_ = nullptr;
  GPUTexture *scene_color_tx_ = nullptr;

  PassSimple pass_ = {"FilterMaterial.Pass"};
  Framebuffer framebuffer_ = {"FilterMaterial.Framebuffer"};
  Texture output_tx_ = {"FilterMaterial.Output"};

 public:
  FilterMaterialModule(Instance &inst) : inst_(inst) {}

  void init() {}
  void begin_sync();
  void end_sync() {}

  bool uses_aov() const
  {
    return gpumat_ != nullptr && GPU_material_flag_get(gpumat_, GPU_MATFLAG_AOV);
  }

  GPUTexture *render(View &view, GPUTexture *input_tx, int2 extent);
};

}  // namespace blender::eevee
