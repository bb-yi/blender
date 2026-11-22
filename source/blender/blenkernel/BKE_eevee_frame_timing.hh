#pragma once

#include <array>

namespace blender::bke {

enum class EeveeEngineStage { Depsgraph, ResultSetup, ResultMerge, DrawSetup, Cleanup, Count };

inline constexpr std::array eevee_engine_stage_names = {
    "Engine.Depsgraph", "Engine.ResultSetup", "Engine.ResultMerge", "Engine.DrawSetup", "Engine.Cleanup"};
static_assert(eevee_engine_stage_names.size() == int(EeveeEngineStage::Count));

struct EeveeFrameTiming {
  double render_ms = -1.0;
  double engine_ms = 0.0;
  double compositor_ms = 0.0;
  double output_nodes_ms = 0.0;
  double save_ms = -1.0;
  double preparation_ms = -1.0;
  bool first_render = false;
  std::array<double, int(EeveeEngineStage::Count)> engine_stages{};

  double unclassified_ms() const;
};

}  // namespace blender::bke
