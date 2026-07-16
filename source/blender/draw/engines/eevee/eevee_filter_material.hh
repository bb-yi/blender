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

#include <string>
#include <memory>
#include <vector>

struct bNode;

namespace blender::draw {
class View;
}

namespace blender::eevee {

using namespace draw;
using FilterObjectInfoBuf = draw::UniformArrayBuffer<FilterObjectInfoData, FILTER_OBJECT_INFO_MAX>;
using FilterGraphInputHandleBuf =
    draw::UniformArrayBuffer<FilterGraphInputHandleData, FILTER_GRAPH_INPUT_MAX>;
class Instance;

class FilterMaterialModule {
 private:
  struct FilterPassEntry {
    const bNode *graph_node = nullptr;
    blender::Material *material = nullptr;
    GPUMaterial *gpumat = nullptr;
    bool uses_aov_input = false;
    bool uses_aov_output = false;
    bool uses_filter_object_info = false;
    blender::Vector<std::string> conflicting_aov_names;
  };

  struct GraphTexturePoolEntry {
    std::unique_ptr<Texture> texture;
    gpu::TextureFormat format = gpu::TextureFormat::SFLOAT_16_16_16_16;
    int2 extent = int2(0);
    int layers = 0;
    bool used = false;
  };

  Instance &inst_;

  blender::Vector<FilterPassEntry> entries_;
  Framebuffer framebuffer_ = {"FilterMaterial.Framebuffer"};
  Texture ping_tx_ = {"FilterMaterial.Ping"};
  Texture pong_tx_ = {"FilterMaterial.Pong"};
  Texture pass_tx_ = {"FilterMaterial.Pass"};
  Texture aov_color_snapshot_tx_ = {"FilterMaterial.AOVColorSnapshot"};
  Texture aov_value_snapshot_tx_ = {"FilterMaterial.AOVValueSnapshot"};
  Texture graph_black_tx_ = {"FilterMaterial.GraphBlack"};
  Vector<std::unique_ptr<Framebuffer>> graph_input_fbs_;
  std::vector<GraphTexturePoolEntry> graph_texture_pool_;
  int2 graph_texture_pool_stage_extent_ = int2(0);
  FilterObjectInfoBuf filter_object_info_buf_ = {"FilterObjectInfoBuf"};
  FilterGraphInputHandleBuf filter_graph_input_buf_ = {"FilterGraphInputBuf"};
  bool uses_scene_depth_ = false;
  bool uses_scene_normal_ = false;
  bool uses_scene_position_ = false;
  bool uses_cryptomatte_object_ = false;
  bool uses_aov_ = false;
  blender::Vector<std::string> used_aov_names_;
  bool uses_scene_time_ = false;

  void reset_graph_texture_pool(int2 stage_extent);
  Texture *acquire_graph_texture(const char *name,
                                 gpu::TextureFormat format,
                                 int2 extent,
                                 int layers);
  void update_filter_object_info_buffer(GPUMaterial *gpumat);
  bool sync_pass_entry(blender::Material *material, FilterPassEntry &entry);

 public:
  FilterMaterialModule(Instance &inst) : inst_(inst) {}

  void init();
  void begin_sync();
  void end_sync() {}

  bool uses_aov() const;
  bool uses_aov_name(const char *name) const;
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
  bool uses_scene_time() const
  {
    return uses_scene_time_;
  }
  bool uses_cryptomatte_object() const
  {
    return uses_cryptomatte_object_;
  }
  int material_count() const
  {
    return int(entries_.size());
  }

  bool has_stage_entries(SceneEEVEEFilterExecutionStage stage) const;
  gpu::Texture *render_stage(draw::View &view,
                             gpu::Texture *input_tx,
                             int2 extent,
                             SceneEEVEEFilterExecutionStage stage);
};

}  // namespace blender::eevee
