/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::eevee {

uint64_t telemetry_source_id_generate();

class Instance;
}  // namespace blender::eevee

namespace blender {
struct DrawPerformanceMetrics;
namespace bke {
struct SceneEeveePerformanceSnapshot;
struct SceneEeveePerformanceSnapshotSet;
}
}

namespace blender::eevee {

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
  SyncBeginWorld = 3,
  SyncBeginSceneModules = 4,
  SyncBeginViewEffects = 5,
  SyncBeginNPRPost = 6,
  SyncEndShaderReadiness = 7,
  SyncEndMaterialsVelocity = 8,
  SyncEndVolumeShadowsLights = 9,
  SyncEndFrameState = 10,
  SyncEndNPRPost = 11,
  SyncEndProbesUniforms = 12,
  CaptureWorld = 13,
  CaptureProbes = 14,
  RenderTextures = 15,
  MainView = 16,
  MainUpdateView = 17,
  MainRenderBuffersAcquire = 18,
  MainPlanarProbesSetView = 19,
  MainLightsSetView = 20,
  MainGBufferAcquire = 21,
  MainBackground = 22,
  MainVolumePrepass = 23,
  MainDeferred = 24,
  MainDeferredPrepass = 25,
  MainDeferredHiZUpdate = 26,
  MainDeferredProbeSetup = 27,
  MainDeferredShadowSetup = 28,
  MainDeferredGBufferPass = 29,
  MainDeferredOpaque = 30,
  MainDeferredRefract = 31,
  MainDeferredRaytrace = 32,
  MainDeferredEvalLight = 33,
  MainDeferredSubsurface = 34,
  MainDeferredCombine = 35,
  MainDeferredShadowFilter = 36,
  MainDeferredNPR = 37,
  MainFilterBeforeVolumeFog = 38,
  MainVolumeCompute = 39,
  MainVolumeComputeSetup = 40,
  MainVolumeScatter = 41,
  MainVolumeIntegration = 42,
  MainVolumeResolve = 43,
  MainVolumeResolveHiZUpdate = 44,
  MainVolumeResolveComposite = 45,
  MainAmbientOcclusion = 46,
  MainForward = 47,
  MainForwardHiZUpdate = 48,
  MainForwardProbeSetup = 49,
  MainForwardShadowSetup = 50,
  MainForwardTransparencySetup = 51,
  MainForwardPrepass = 52,
  MainForwardOpaque = 53,
  MainForwardTransparent = 54,
  MainForwardResolve = 55,
  MainFilterBeforePostFX = 56,
  PostMotionBlur = 57,
  PostFilterBeforeDepthOfField = 58,
  PostDepthOfField = 59,
  DOFSetup = 60,
  DOFTilePrepare = 61,
  DOFBackgroundConvolution = 62,
  DOFForegroundConvolution = 63,
  DOFHoleFill = 64,
  DOFResolve = 65,
  PostFilterBeforeComposite = 66,
  MainFilmAccumulate = 67,
  Lookdev = 68,
  ReadResult = 69,
  ShadowTilemapSetup = 70,
  ShadowCasterUpdate = 71,
  ShadowTransparentCasterUpdate = 72,
  ShadowUsageMarking = 73,
  ShadowTilemapUpdate = 74,
  ShadowUpdateFinish = 75,
  ShadowSurface = 76,
  DrawSyncShared = 77,
  DrawSyncEngineSetup = 78,
  DrawSyncEngineInit = 79,
  DrawSyncManagerBegin = 80,
  DrawSyncEngineBegin = 81,
  DrawSyncModulesBegin = 82,
  DrawSyncObjectIteration = 83,
  DrawSyncDupliExtraction = 84,
  DrawSyncDelayedExtraction = 85,
  DrawSyncExtractionWait = 86,
  DrawSyncCurvesUpdate = 87,
  DrawSyncEngineEnd = 88,
  DrawSyncManagerEnd = 89,
  DrawSubmissionShared = 90,
  DrawSubmissionFramebuffer = 91,
  DrawSubmissionCallbacksPre = 92,
  DrawSubmissionEngineDraw = 93,
  DrawSubmissionCallbacksPost = 94,
  DrawSubmissionFramebufferRestore = 95,
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

struct TelemetryScopeNode {
  TelemetryStageId stage = TelemetryStageId::Count;
  double cpu_ms = 0.0;
  /* Average of this exact root-to-node scope path over the active history window. */
  double average_cpu_ms = 0.0;
  int call_count = 0;
  Vector<int> children;
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
  uint64_t source_id = 0;
  uint64_t epoch = 0;
  uint64_t capture_seq = 0;
  uint64_t playback_session_id = 0;
  bool is_playback = false;
  uint32_t scene_session_uid = 0;
  uint64_t render_run_id = 0;
  std::string view_layer_name;
  std::string render_view_name;
  int frame = 0;
  int sample_index = 0;
  uint64_t sample_count = 0;
  int resolution_x = 0;
  int resolution_y = 0;
  bool has_last_evaluation = false;
  double last_evaluation_ms = 0.0;
  uint64_t depsgraph_eval_serial = 0;
  double total_cpu_ms = 0.0;
  double average_total_cpu_ms = 0.0;
  std::array<double, int(TelemetryStageId::Count)> average_stage_values = {};
  double draw_sync_ms = 0.0;
  double draw_submission_ms = 0.0;
  double profiler_accounting_ms = 0.0;
  bool has_shared_draw_timing = false;
  std::array<TelemetryStageSample, int(TelemetryStageId::Count)> stages = {};
  Vector<TelemetryScopeNode> scope_nodes;
  std::array<TelemetryShadowContextSample, int(TelemetryShadowContext::Count)> shadow_contexts = {};
  TelemetryShaderWaitSample shader_waits;
  std::array<TelemetryPassReadbackSample, int(TelemetryPassReadbackType::Count)> pass_readbacks =
      {};
  TelemetryMaterialSyncSample material_sync;
  Vector<TelemetryMaterialHotspot> material_hotspots;
  int64_t material_hotspot_untracked_count = 0;
  TelemetryFeatureSnapshot features;
  Vector<TelemetryShadowLightCost> shadow_light_costs;
  Vector<TelemetryProbeCost> probe_costs;
};

struct TelemetryStageInfo {
  TelemetryStageId id;
  const char *label;
  const char *tree_path;
};

struct TelemetrySourceState {
  uint64_t source_id = telemetry_source_id_generate();
  std::shared_ptr<const void> lifetime = std::make_shared<int>(0);
  uint64_t epoch = 1;
  uint64_t capture_seq = 0;
  double last_viewport_publish_time = 0.0;
  bool has_viewport_publish = false;
  /* Text is kept per source so a paused viewport never reads another source's Scene-level writer. */
  std::string last_published_viewport_summary;
  std::string last_published_viewport_report;
  bool viewport_was_finished = false;
  std::array<Vector<TelemetryFrameRecord>, int(TelemetryRuntimeMode::Count)> history;
  std::array<TelemetryFrameRecord, int(TelemetryRuntimeMode::Count)> last_records = {};
  std::array<bool, int(TelemetryRuntimeMode::Count)> has_last_record = {};
  bool playback_active = false;
  bool has_playback_peak = false;
  uint64_t next_playback_session = 0;
  TelemetryFrameRecord playback_peak;
  Vector<TelemetryFrameRecord> playback_peak_history;
  bool profiler_inputs_initialized = false;
  bool profiler_enabled = false;
  int extent_x = 0;
  int extent_y = 0;
  int output_offset_x = 0;
  int output_offset_y = 0;
  int output_extent_x = 0;
  int output_extent_y = 0;
  bool binding_initialized = false;
  uint32_t scene_session_uid = 0;
  std::string view_layer_name;
  std::string render_view_name;
  uint64_t render_run_id = 0;
  bool closed = false;
};

class TelemetryModule {
 private:
  static constexpr int history_limit_ = 64;
  static constexpr int playback_peak_history_limit_ = 8;
  static constexpr double viewport_publish_interval_seconds_ = 0.25;

  Instance &inst_;
  std::shared_ptr<TelemetrySourceState> source_state_;
  bool frame_active_ = false;
  double frame_start_time_ = 0.0;
  /* Starts before Draw Manager metrics are merged, so this bookkeeping is visible separately from
   * the measured Draw CPU total. */
  double profiler_accounting_start_time_ = 0.0;
  TelemetryFrameRecord current_frame_;
  Vector<int> scope_stack_;

 public:
  TelemetryModule(Instance &inst, std::shared_ptr<TelemetrySourceState> source_state = nullptr)
      : inst_(inst),
        source_state_(source_state ? std::move(source_state) :
                                     std::make_shared<TelemetrySourceState>())
  {}
  ~TelemetryModule();

  bool enabled() const;
  bool epoch_inputs_update(bool profiler_enabled,
                           int extent_x,
                           int extent_y,
                           int output_offset_x,
                           int output_offset_y,
                           int output_extent_x,
                           int output_extent_y);
  bool source_binding_update(uint32_t scene_session_uid, const char *view_layer_name);
  void render_view_name_set(const char *render_view_name);
  void render_run_id_set(uint64_t render_run_id);
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
  void reset_epoch(bool clear_source_session = false);
  void draw_performance_end(const blender::DrawPerformanceMetrics &metrics);

  uint64_t source_id();
  uint64_t epoch() const
  {
    return source_state_->epoch;
  }

  void stage_add(TelemetryStageId stage, double elapsed_seconds);
  int scope_begin(TelemetryStageId stage);
  void scope_end(int node_index, double elapsed_seconds);
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
  const TelemetryFrameRecord *last_viewport_record() const;
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
  void merge_draw_performance(const blender::DrawPerformanceMetrics &metrics);
  bool finalize_playback_session();
  void source_deactivate();
  void publish_viewport_snapshot(const TelemetryFrameRecord &record,
                                 const std::string &summary,
                                 const std::string &report);
  void publish_render_snapshot(const TelemetryFrameRecord &record,
                               const std::string &report);
  std::shared_ptr<const bke::SceneEeveePerformanceSnapshot> build_snapshot(
      const TelemetryFrameRecord &record,
      const char *kind,
      const std::string &summary,
      const std::string &report,
      uint64_t capture_seq) const;
};

class ScopedTelemetrySample {
 private:
  TelemetryModule *telemetry_ = nullptr;
  TelemetryStageId stage_;
  int scope_node_index_ = -1;
  double start_time_ = 0.0;

 public:
  ScopedTelemetrySample(TelemetryModule &telemetry, TelemetryStageId stage);
  ~ScopedTelemetrySample();
};

}  // namespace blender::eevee
