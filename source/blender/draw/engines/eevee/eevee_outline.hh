/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DRW_gpu_wrapper.hh"
#include "draw_pass.hh"

struct Object;
struct GPUMaterial;

namespace blender {
struct Material;
}

namespace blender::eevee {

using namespace draw;

class Instance;

class OutlineModule {
 private:
  Instance &inst_;
  bool enabled_ = false;
  bool use_in_combined_ = false;

  PassSimple detect_ps_ = {"Outline.Detect"};
  PassMain shell_ps_ = {"Outline.Shell"};
  PassSimple shell_boundary_ps_ = {"Outline.ShellBoundary"};
  PassMain freestyle_edge_ps_ = {"Outline.FreestyleEdge"};
  PassSimple jfa_init_ps_ = {"Outline.JFA.Init"};
  PassSimple jfa_step_ps_ = {"Outline.JFA.Step"};
  PassSimple resolve_ps_ = {"Outline.Resolve"};

  Framebuffer detect_fb_ = {"Outline.Detect.FB"};
  Framebuffer jfa_init_fb_ = {"Outline.JFA.Init.FB"};
  Framebuffer resolve_fb_ = {"Outline.Resolve.FB"};
  Framebuffer shell_fb_ = {"Outline.Shell.FB"};
  Framebuffer shell_boundary_fb_ = {"Outline.ShellBoundary.FB"};
  Framebuffer occlusion_fb_ = {"Outline.Occlusion.FB"};

  TextureFromPool edge_seed_tx_ = {"Outline.EdgeSeed"};
  TextureFromPool shell_color_tx_ = {"Outline.ShellColor"};
  TextureFromPool resolved_outline_tx_ = {"Outline.Resolved"};
  TextureFromPool resolved_depth_tx_ = {"Outline.ResolvedDepth"};
  TextureFromPool resolved_velocity_tx_ = {"Outline.ResolvedVelocity"};
  TextureFromPool occlusion_depth_tx_ = {"Outline.OcclusionDepth"};
  SwapChain<TextureFromPool, 2> jfa_tx_;

  int jfa_step_size_ = 1;
  int3 jfa_dispatch_size_ = int3(1);

 public:
  OutlineModule(Instance &inst) : inst_(inst) {}

  void begin_sync();
  PassMain::Sub *shell_add(blender::Material *blender_mat, GPUMaterial *gpumat);
  void sync_object(Object *ob, ResourceHandleRange res_handle);

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
