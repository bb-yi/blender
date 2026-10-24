/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <array>
#include <cstdint>
#include "BLI_time.h"

namespace blender {

enum class GPUMaterialProfileStage {
  LookupWait,
  AnimationState,
  Inline,
  Preprocess,
  Execute,
  NPR,
  Textures,
  Codegen,
  CompileWait,
  Cleanup,
  GLSLParse,
  Count,
};
enum class GPUMaterialProfileCounter {
  MaterialHits,
  MaterialBuilds,
  PassHits,
  PassMisses,
  GLSLCacheHits,
  GLSLCacheMisses,
  GLSLSourceBytes,
  GLSLTokens,
  GLSLCacheBytes,
  UniformUpdates,
  Count,
};
struct GPUMaterialProfile {
  std::array<double, int(GPUMaterialProfileStage::Count)> seconds = {};
  std::array<uint64_t, int(GPUMaterialProfileCounter::Count)> counts = {};
};

/* Scoped to one render source on its sync thread. Nested Pre-FX sources restore their parent. */
extern thread_local GPUMaterialProfile *gpu_material_profile_active;

class GPUMaterialProfileContext {
  GPUMaterialProfile *previous_;

 public:
  explicit GPUMaterialProfileContext(GPUMaterialProfile *profile)
      : previous_(gpu_material_profile_active)
  {
    gpu_material_profile_active = profile;
  }
  ~GPUMaterialProfileContext()
  {
    gpu_material_profile_active = previous_;
  }
};

class GPUMaterialProfileTimer {
  GPUMaterialProfile *profile_;
  GPUMaterialProfileStage stage_;
  double start_;

 public:
  explicit GPUMaterialProfileTimer(GPUMaterialProfileStage stage)
      : profile_(gpu_material_profile_active),
        stage_(stage),
        start_(profile_ ? BLI_time_now_seconds() : 0.0)
  {
  }
  ~GPUMaterialProfileTimer()
  {
    stop();
  }
  void stop()
  {
    if (profile_) {
      profile_->seconds[int(stage_)] += BLI_time_now_seconds() - start_;
      profile_ = nullptr;
    }
  }
};

inline void GPU_material_profile_count(GPUMaterialProfileCounter counter, uint64_t amount = 1)
{
  if (gpu_material_profile_active) {
    gpu_material_profile_active->counts[int(counter)] += amount;
  }
}
inline void GPU_material_profile_max(GPUMaterialProfileCounter counter, uint64_t value)
{
  if (gpu_material_profile_active) {
    uint64_t &current = gpu_material_profile_active->counts[int(counter)];
    current = current < value ? value : current;
  }
}
}  // namespace blender
