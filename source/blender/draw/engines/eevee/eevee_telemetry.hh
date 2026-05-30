/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include <array>
#include <string>

#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::eevee {

class Instance;

enum class TelemetryRuntimeMode : uint8_t {
  Viewport = 0,
  ViewportImageRender = 1,
  FinalRender = 2,
  Bake = 3,
  Count,
};

enum class TelemetryShadowContext : uint8_t {
  MainView = 0,
  PlanarProbe = 1,
  CaptureProbe = 2,
  Bake = 3,
  Other = 4,
  Count,
};

enum class TelemetryPassReadbackType : uint8_t {
  RenderPass = 0,
  AOV = 1,
  NativePostFX = 2,
  Count,
};

enum class TelemetryStageId : uint8_t {
  SyncBegin = 0,
  SyncObjects = 1,
  SyncEnd = 2,
  CaptureWorld = 3,
  CaptureProbes = 4,
  RenderTextures = 5,
  MainView = 6,
  MainUpdateView = 7,
  MainRenderBuffersAcquire = 8,
  MainPlanarProbesSetView = 9,
  MainLightsSetView = 10,
  MainGBufferAcquire = 11,
  MainBackground = 12,
  MainVolumePrepass = 13,
  MainDeferred = 14,
  MainDeferredPrepass = 15,
  MainDeferredHiZUpdate = 16,
  MainDeferredProbeSetup = 17,
  MainDeferredShadowSetup = 18,
  MainDeferredGBufferPass = 19,
  MainDeferredOpaque = 20,
  MainDeferredRefract = 21,
  MainDeferredRaytrace = 22,
  MainDeferredEvalLight = 23,
  MainDeferredSubsurface = 24,
  MainDeferredCombine = 25,
  MainDeferredShadowFilter = 26,
  MainDeferredNPR = 27,
  MainFilterBeforeVolumeFog = 28,
  MainVolumeCompute = 29,
  MainVolumeComputeSetup = 30,
  MainVolumeScatter = 31,
  MainVolumeIntegration = 32,
  MainVolumeResolve = 33,
  MainVolumeResolveHiZUpdate = 34,
  MainVolumeResolveComposite = 35,
  MainAmbientOcclusion = 36,
  MainForward = 37,
  MainForwardHiZUpdate = 38,
  MainForwardProbeSetup = 39,
  MainForwardShadowSetup = 40,
  MainForwardTransparencySetup = 41,
  MainForwardPrepass = 42,
  MainForwardOpaque = 43,
  MainForwardTransparent = 44,
  MainForwardResolve = 45,
  MainFilterBeforePostFX = 46,
  PostMotionBlur = 47,
  PostFilterBeforeDepthOfField = 48,
  PostDepthOfField = 49,
  DOFSetup = 50,
  DOFTilePrepare = 51,
  DOFBackgroundConvolution = 52,
  DOFForegroundConvolution = 53,
  DOFHoleFill = 54,
  DOFResolve = 55,
  PostFilterBeforeComposite = 56,
  MainFilmAccumulate = 57,
  Lookdev = 58,
  ReadResult = 59,
  ShadowTilemapSetup = 60,
  ShadowCasterUpdate = 61,
  ShadowTransparentCasterUpdate = 62,
  ShadowUsageMarking = 63,
  ShadowTilemapUpdate = 64,
  ShadowUpdateFinish = 65,
  ShadowSurface = 66,
  Count,
};

struct TelemetryFeatureSnapshot {
  bool has_ao = false;
  bool has_dof = false;
  bool has_motion_blur = false;
  bool has_volume = false;
  bool has_raytracing = false;
  int filter_material_count = 0;
  int render_texture_count = 0;
  int light_count = 0;
  int probe_count = 0;
  int npr_material_count = 0;
  int raycast_material_count = 0;
  int glsl_function_material_count = 0;
};

struct TelemetryStageSample {
  double cpu_ms = 0.0;
  int call_count = 0;
};

struct TelemetryShadowLightCost {
  std::string name;
  std::string type;
  int tilemaps = 0;
  int estimated_views = 0;
  int sync_dirty_tilemaps = 0;
  double estimated_share_percent = 0.0;
  std::string level;
};

struct TelemetryShadowContextSample {
  double cpu_ms = 0.0;
  int call_count = 0;
  int loop_count = 0;
};

struct TelemetryMaterialSyncSample {
  int64_t request_count = 0;
  int64_t shader_queued_count = 0;
  int64_t optimize_queued_count = 0;
  int64_t fallback_count = 0;
  int64_t failed_count = 0;
};

struct TelemetryMaterialHotspot {
  std::string name;
  int64_t request_count = 0;
  int64_t shader_queued_count = 0;
  int64_t optimize_queued_count = 0;
  int64_t fallback_count = 0;
  int64_t failed_count = 0;
};

struct TelemetryShaderWaitSample {
  int64_t wait_count = 0;
  int64_t queued_shader_count = 0;
  int64_t queued_texture_count = 0;
  double cpu_ms = 0.0;
};

struct TelemetryPassReadbackSample {
  int64_t pass_count = 0;
  int64_t pixel_count = 0;
  int64_t output_value_count = 0;
  double cpu_ms = 0.0;
  std::string names;
};

struct TelemetryProbeCost {
  std::string name;
  std::string type;
  int updated = 0;
  int total = 0;
  int rendered_views = 0;
  int resolution = 0;
  double estimated_work = 0.0;
  std::string level;
};

struct TelemetryFrameRecord {
  TelemetryRuntimeMode runtime_mode = TelemetryRuntimeMode::Viewport;
  int frame = 0;
  int sample_index = 0;
  uint64_t sample_count = 0;
  double total_cpu_ms = 0.0;
  std::array<TelemetryStageSample, int(TelemetryStageId::Count)> stages = {};
  std::array<TelemetryShadowContextSample, int(TelemetryShadowContext::Count)> shadow_contexts = {};
  TelemetryShaderWaitSample shader_waits;
  std::array<TelemetryPassReadbackSample, int(TelemetryPassReadbackType::Count)> pass_readbacks =
      {};
  TelemetryMaterialSyncSample material_sync;
  Vector<TelemetryMaterialHotspot> material_hotspots;
  TelemetryFeatureSnapshot features;
  Vector<TelemetryShadowLightCost> shadow_light_costs;
  Vector<TelemetryProbeCost> probe_costs;
};

struct TelemetryStageInfo {
  TelemetryStageId id;
  const char *label;
  const char *tree_path;
};

class TelemetryModule {
 private:
  static constexpr int history_limit_ = 64;
  static constexpr double viewport_publish_interval_seconds_ = 0.25;

  Instance &inst_;
  bool frame_active_ = false;
  double frame_start_time_ = 0.0;
  double last_viewport_publish_time_ = 0.0;
  bool has_viewport_publish_ = false;
  TelemetryFrameRecord current_frame_;
  std::array<Vector<TelemetryFrameRecord>, int(TelemetryRuntimeMode::Count)> history_;
  std::array<TelemetryFrameRecord, int(TelemetryRuntimeMode::Count)> last_records_ = {};
  std::array<bool, int(TelemetryRuntimeMode::Count)> has_last_record_ = {};

 public:
  TelemetryModule(Instance &inst) : inst_(inst) {}

  bool enabled() const;
  TelemetryRuntimeMode runtime_mode() const;
  int average_window() const;
  bool frame_active() const
  {
    return frame_active_;
  }

  void frame_begin(TelemetryRuntimeMode mode);
  void frame_end();

  void maybe_begin_viewport_frame();
  void maybe_end_viewport_frame();
  void maybe_begin_final_frame();
  void maybe_end_final_frame();
  void maybe_publish_cached_viewport(bool force = false);
  void cancel_frame();
  void reset();

  void stage_add(TelemetryStageId stage, double elapsed_seconds);
  void shadow_context_add(TelemetryShadowContext context, double elapsed_seconds, int loop_count);
  void material_sync_add(bool shader_queued,
                         bool optimize_queued,
                         bool fallback,
                         bool failed,
                         const char *material_name);
  void shader_wait_add(int64_t queued_shaders, int64_t queued_textures, double elapsed_seconds);
  void pass_readback_add(TelemetryPassReadbackType type,
                         const char *name,
                         int width,
                         int height,
                         int channels,
                         double elapsed_seconds);

  std::string viewport_summary_line() const;
  Vector<std::string> viewport_overlay_lines(bool include_stage_list) const;
  std::string viewport_report() const;
  std::string render_report() const;

  static const char *stage_label(TelemetryStageId stage);
  static const char *shadow_context_label(TelemetryShadowContext context);
  static const char *pass_readback_type_label(TelemetryPassReadbackType type);
  static const TelemetryStageInfo &stage_info(TelemetryStageId stage);
  static Span<const TelemetryStageInfo> stage_infos();

 private:
  void snapshot_features();
  bool frame_has_stage_samples(const TelemetryFrameRecord &record) const;
  const TelemetryFrameRecord *last_record(TelemetryRuntimeMode mode) const;
  double averaged_total_cpu_ms(TelemetryRuntimeMode mode) const;
  std::array<double, int(TelemetryStageId::Count)> averaged_stage_values(
      TelemetryRuntimeMode mode) const;
  bool use_time_sort() const;
  bool viewport_publish_paused() const;
  Vector<int> sorted_stage_indices(const TelemetryFrameRecord &record) const;
  std::string format_shadow_lights_report(const TelemetryFrameRecord &record) const;
  std::string format_shadow_contexts_report(const TelemetryFrameRecord &record) const;
  std::string format_shader_waits_report(const TelemetryFrameRecord &record) const;
  std::string format_pass_readbacks_report(const TelemetryFrameRecord &record) const;
  std::string format_material_sync_report(const TelemetryFrameRecord &record) const;
  std::string format_probe_costs_report(const TelemetryFrameRecord &record) const;
};

class ScopedTelemetrySample {
 private:
  TelemetryModule *telemetry_ = nullptr;
  TelemetryStageId stage_;
  double start_time_ = 0.0;

 public:
  ScopedTelemetrySample(TelemetryModule &telemetry, TelemetryStageId stage);
  ~ScopedTelemetrySample();
};

}  // namespace blender::eevee
