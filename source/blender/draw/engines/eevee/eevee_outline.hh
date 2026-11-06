/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DRW_gpu_wrapper.hh"
#include "draw_pass.hh"

struct Object;
struct GPUMaterial;

namespace blender::eevee {

using namespace draw;

class Instance;

class OutlineModule {
 private:
  Instance &inst_;
  bool enabled_ = false;
  bool use_in_combined_ = false;
  float resolution_scale_ = 1.0f;
  int surface_identity_ = 0;
  int2 outline_extent_ = int2(1);

  PassSimple detect_ps_ = {"Outline.Detect"};
  PassSimple source_classify_ps_ = {"Outline.SourceClassify"};
  PassSimple source_dispatch_ps_ = {"Outline.SourceDispatch"};
  PassSimple classify_ps_ = {"Outline.Classify"};
  PassSimple dispatch_ps_ = {"Outline.Dispatch"};
  PassMain freestyle_edge_ps_ = {"Outline.FreestyleEdge"};
  PassSimple factor_blur_ps_ = {"Outline.FactorBlur"};
  PassSimple jfa_init_ps_[2] = {{"Outline.JFA.Init"}, {"Outline.JFA.Init"}};
  PassSimple jfa_step_ps_[4] = {{"Outline.JFA.Step"},
                                {"Outline.JFA.Step"},
                                {"Outline.JFA.Step.Visible"},
                                {"Outline.JFA.Step.Visible"}};
  PassSimple resolve_ps_[2] = {{"Outline.Resolve"}, {"Outline.Resolve"}};
  PassSimple upsample_ps_ = {"Outline.Upsample"};

  Framebuffer detect_fb_ = {"Outline.Detect.FB"};
  Framebuffer factor_blur_fb_ = {"Outline.FactorBlur.FB"};
  Framebuffer jfa_init_fb_ = {"Outline.JFA.Init.FB"};
  Framebuffer resolve_fb_ = {"Outline.Resolve.FB"};
  Framebuffer upsample_fb_ = {"Outline.Upsample.FB"};
  Framebuffer occlusion_fb_ = {"Outline.Occlusion.FB"};

  SwapChain<TextureFromPool, 2> edge_seed_tx_;
  TextureFromPool resolved_outline_tx_ = {"Outline.Resolved"};
  TextureFromPool resolved_depth_tx_ = {"Outline.ResolvedDepth"};
  TextureFromPool resolved_velocity_tx_ = {"Outline.ResolvedVelocity"};
  TextureFromPool reduced_outline_tx_ = {"Outline.Reduced"};
  TextureFromPool reduced_depth_tx_ = {"Outline.ReducedDepth"};
  TextureFromPool reduced_velocity_tx_ = {"Outline.ReducedVelocity"};
  TextureFromPool reduced_id_tx_ = {"Outline.ReducedID"};
  TextureFromPool occlusion_depth_tx_ = {"Outline.OcclusionDepth"};
  gpu::Texture *outline_occlusion_depth_tx_ = nullptr;
  int use_outline_occlusion_depth_ = 0;
  SwapChain<TextureFromPool, 2> jfa_tx_;

  int jfa_step_size_ = 1;
  int3 jfa_dispatch_size_ = int3(1);
  int3 source_dispatch_size_ = int3(1);
  StorageBuffer<uint4> classification_buf_ = {"Outline.Classification"};
  DrawIndirectBuf original_detect_draw_buf_ = {"Outline.Detect.Original.Draw"};
  DrawIndirectBuf cached_detect_draw_buf_ = {"Outline.Detect.Cached.Draw"};
  DrawIndirectBuf factor_blur_draw_buf_ = {"Outline.FactorBlur.Draw"};
  DrawIndirectBuf jfa_init_draw_buf_ = {"Outline.JFA.Init.Draw"};
  DispatchIndirectBuf jfa_dispatch_buf_ = {"Outline.JFA.Dispatch"};
  bool packed_jfa_support_checked_ = false;
  bool packed_jfa_supported_ = false;

 public:
  OutlineModule(Instance &inst) : inst_(inst) {}

  void begin_sync();
  PassMain::Sub *material_add(GPUMaterial *gpumat);
  void sync_object(Object *ob, ResourceHandleRange res_handle, PassMain::Sub *pass, int material);

  void sync();
  void render(View &view, int2 extent);
  void release_result();

  bool enabled() const
  {
    return enabled_;
  }

  bool use_in_combined() const
  {
    return enabled_ && use_in_combined_;
  }

  bool has_result() const
  {
    return resolved_outline_tx_.is_valid();
  }

  gpu::Texture *resolved_texture_or(gpu::Texture *fallback)
  {
    return has_result() ? resolved_outline_tx_.gpu_texture() : fallback;
  }

  gpu::Texture *resolved_texture()
  {
    return has_result() ? resolved_outline_tx_.gpu_texture() : nullptr;
  }

  gpu::Texture *resolved_depth_texture()
  {
    return has_result() && resolved_depth_tx_.is_valid() ? resolved_depth_tx_.gpu_texture() :
                                                           nullptr;
  }

  gpu::Texture *resolved_velocity_texture()
  {
    return has_result() && resolved_velocity_tx_.is_valid() ?
               resolved_velocity_tx_.gpu_texture() :
               nullptr;
  }
};

}  // namespace blender::eevee
