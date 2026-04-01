/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "DRW_gpu_wrapper.hh"
#include "DRW_render.hh"

#include "BLI_vector.hh"

#include "GPU_material.hh"

#include "eevee_filter_material_shared.hh"

namespace blender::draw {
class View;
}

namespace blender::eevee {

using namespace draw;
using FilterObjectInfoBuf = draw::UniformArrayBuffer<FilterObjectInfoData, FILTER_OBJECT_INFO_MAX>;

class Instance;

class FilterMaterialModule {
 private:
  struct FilterPassEntry {
    SceneFilterMaterial *scene_filter = nullptr;
    blender::Material *material = nullptr;
    GPUMaterial *gpumat = nullptr;
  };

  Instance &inst_;

  blender::Vector<FilterPassEntry> entries_;
  Framebuffer framebuffer_ = {"FilterMaterial.Framebuffer"};
  Texture ping_tx_ = {"FilterMaterial.Ping"};
  Texture pong_tx_ = {"FilterMaterial.Pong"};
  FilterObjectInfoBuf filter_object_info_buf_ = {"FilterObjectInfoBuf"};
  bool uses_scene_depth_ = false;
  bool uses_scene_normal_ = false;
  bool uses_scene_position_ = false;
  bool uses_cryptomatte_object_ = false;

  void update_filter_object_info_buffer(GPUMaterial *gpumat);

 public:
  FilterMaterialModule(Instance &inst) : inst_(inst) {}

  void init();
  void begin_sync();
  void end_sync() {}

  bool uses_aov() const;
  bool uses_scene_depth() const
  {
    return uses_scene_depth_;
  }
  bool uses_scene_normal() const
  {
    return uses_scene_normal_;
  }
  bool uses_scene_position() const
  {
    return uses_scene_position_;
  }
  bool uses_cryptomatte_object() const
  {
    return uses_cryptomatte_object_;
  }

  bool has_stage_entries(SceneEEVEEFilterExecutionStage stage) const;
  gpu::Texture *render_stage(draw::View &view,
                             gpu::Texture *input_tx,
                             int2 extent,
                             SceneEEVEEFilterExecutionStage stage);
};

}  // namespace blender::eevee
