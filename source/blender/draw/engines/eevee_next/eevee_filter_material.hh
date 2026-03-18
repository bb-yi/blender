/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include "BLI_vector.hh"

#include "GPU_material.hh"

#include "eevee_shader_shared.hh"

namespace blender::eevee {

class Instance;

class FilterMaterialModule {
 private:
  struct FilterPassEntry {
    ::SceneFilterMaterial *scene_filter = nullptr;
    ::Material *material = nullptr;
    GPUMaterial *gpumat = nullptr;
  };

  Instance &inst_;

  blender::Vector<FilterPassEntry> entries_;
  Framebuffer framebuffer_ = {"FilterMaterial.Framebuffer"};
  Texture ping_tx_ = {"FilterMaterial.Ping"};
  Texture pong_tx_ = {"FilterMaterial.Pong"};
  bool uses_scene_depth_ = false;
  bool uses_scene_normal_ = false;

 public:
  FilterMaterialModule(Instance &inst) : inst_(inst) {}

  void init();
  void begin_sync();
  void end_sync() {}

  bool uses_aov() const
  {
    for (const FilterPassEntry &entry : entries_) {
      if (entry.gpumat != nullptr && GPU_material_flag_get(entry.gpumat, GPU_MATFLAG_AOV)) {
        return true;
      }
    }
    return false;
  }

  bool uses_scene_depth() const
  {
    return uses_scene_depth_;
  }

  bool uses_scene_normal() const
  {
    return uses_scene_normal_;
  }

  GPUTexture *render(View &view, GPUTexture *input_tx, int2 extent);
};

}  // namespace blender::eevee
