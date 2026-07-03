/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include <algorithm>

#include <fmt/format.h>

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_time.h"

#include "DEG_depsgraph_query.hh"

#include "BKE_scene_runtime.hh"

#include "DNA_scene_types.h"

#include "WM_api.hh"

#include "eevee_instance.hh"

#include "eevee_telemetry.hh"

namespace blender::eevee {

static constexpr int stage_count = int(TelemetryStageId::Count);
static constexpr int shadow_context_count = int(TelemetryShadowContext::Count);
static constexpr int pass_readback_type_count = int(TelemetryPassReadbackType::Count);
static constexpr int material_hotspot_report_limit = 8;
static constexpr int pass_readback_name_limit = 8;

static bke::SceneEeveePerformanceRuntime *scene_eevee_performance_runtime(Scene *scene)
{
  return (scene != nullptr && scene->runtime != nullptr) ? &scene->runtime->eevee_performance :
                                                           nullptr;
}

static const bke::SceneEeveePerformanceRuntime *scene_eevee_performance_runtime(const Scene *scene)
{
  return (scene != nullptr && scene->runtime != nullptr) ? &scene->runtime->eevee_performance :
                                                           nullptr;
}

static std::string sample_progress_string(const TelemetryFrameRecord &record)
{
  const uint64_t current = (record.sample_count >= 0xFFFFFFu) ? uint64_t(record.sample_index) :
                                                             min_uu(uint64_t(record.sample_index),
                                                                    record.sample_count);
  if (record.sample_count >= 0xFFFFFFu) {
    return fmt::format("{}/Continuous", current);
  }
  return fmt::format("{}/{}", current, record.sample_count);
}

static const char *sample_status_string(const TelemetryFrameRecord &record)
{
  if (record.sample_count >= 0xFFFFFFu) {
    return "Continuous";
  }
  return (uint64_t(record.sample_index) >= record.sample_count) ? "Complete" : "In Progress";
}

static constexpr std::array<TelemetryStageInfo, stage_count> telemetry_stage_info = {{
    {TelemetryStageId::SyncBegin, "Sync.Begin", "Sync/Begin"},
    {TelemetryStageId::SyncObjects, "Sync.Objects", "Sync/Objects"},
    {TelemetryStageId::SyncEnd, "Sync.End", "Sync/End"},
    {TelemetryStageId::CaptureWorld, "Capture.World", "Capture/World"},
    {TelemetryStageId::CaptureProbes, "Capture.Probes", "Capture/Probes"},
    {TelemetryStageId::RenderTextures, "RenderTextures", "Render Textures"},
    {TelemetryStageId::MainView, "MainView", "Main View"},
    {TelemetryStageId::MainUpdateView, "MainView.Update", "Main View/Update"},
    {TelemetryStageId::MainRenderBuffersAcquire,
     "MainView.RenderBuffers",
     "Main View/Render Buffers"},
    {TelemetryStageId::MainPlanarProbesSetView,
     "MainView.PlanarProbes",
     "Main View/Planar Probes"},
    {TelemetryStageId::MainLightsSetView, "MainView.Lights", "Main View/Lights"},
    {TelemetryStageId::MainGBufferAcquire, "MainView.GBuffer", "Main View/GBuffer"},
    {TelemetryStageId::MainBackground, "Background", "Main View/Background"},
    {TelemetryStageId::MainVolumePrepass, "Volume.Prepass", "Main View/Volume Prepass"},
    {TelemetryStageId::MainDeferred, "Deferred", "Main View/Deferred"},
    {TelemetryStageId::MainDeferredPrepass, "Deferred.Prepass", "Main View/Deferred/Prepass"},
    {TelemetryStageId::MainDeferredHiZUpdate,
     "Deferred.HiZUpdate",
     "Main View/Deferred/HiZ Update"},
    {TelemetryStageId::MainDeferredProbeSetup,
     "Deferred.ProbeSetup",
     "Main View/Deferred/Probe Setup"},
    {TelemetryStageId::MainDeferredShadowSetup,
     "Deferred.ShadowSetup",
     "Main View/Deferred/Shadow Setup"},
    {TelemetryStageId::MainDeferredGBufferPass,
     "Deferred.GBufferPass",
     "Main View/Deferred/GBuffer Pass"},
    {TelemetryStageId::MainDeferredOpaque, "Deferred.Opaque", "Main View/Deferred/Opaque"},
    {TelemetryStageId::MainDeferredRefract, "Deferred.Refract", "Main View/Deferred/Refract"},
    {TelemetryStageId::MainDeferredRaytrace, "Deferred.Raytrace", "Main View/Deferred/Raytrace"},
    {TelemetryStageId::MainDeferredEvalLight,
     "Deferred.EvalLight",
     "Main View/Deferred/Eval Light"},
    {TelemetryStageId::MainDeferredSubsurface,
     "Deferred.Subsurface",
     "Main View/Deferred/Subsurface"},
    {TelemetryStageId::MainDeferredCombine, "Deferred.Combine", "Main View/Deferred/Combine"},
    {TelemetryStageId::MainDeferredShadowFilter,
     "Deferred.ShadowFilter",
     "Main View/Deferred/Shadow Filter"},
    {TelemetryStageId::MainDeferredNPR, "Deferred.NPR", "Main View/Deferred/NPR"},
    {TelemetryStageId::MainFilterBeforeVolumeFog,
     "Filter.BeforeVolumeFog",
     "Main View/Filter Before Volume Fog"},
    {TelemetryStageId::MainVolumeCompute, "Volume.Compute", "Main View/Volume Compute"},
    {TelemetryStageId::MainVolumeComputeSetup,
     "Volume.ComputeSetup",
     "Main View/Volume Compute/Setup"},
    {TelemetryStageId::MainVolumeScatter, "Volume.Scatter", "Main View/Volume Compute/Scatter"},
    {TelemetryStageId::MainVolumeIntegration,
     "Volume.Integration",
     "Main View/Volume Compute/Integration"},
    {TelemetryStageId::MainVolumeResolve, "Volume.Resolve", "Main View/Volume Resolve"},
    {TelemetryStageId::MainVolumeResolveHiZUpdate,
     "Volume.Resolve.HiZ",
     "Main View/Volume Resolve/HiZ Update"},
    {TelemetryStageId::MainVolumeResolveComposite,
     "Volume.Resolve.Composite",
     "Main View/Volume Resolve/Composite"},
    {TelemetryStageId::MainAmbientOcclusion,
     "AmbientOcclusion",
     "Main View/Ambient Occlusion"},
    {TelemetryStageId::MainForward, "Forward", "Main View/Forward"},
    {TelemetryStageId::MainForwardHiZUpdate,
     "Forward.HiZUpdate",
     "Main View/Forward/HiZ Update"},
    {TelemetryStageId::MainForwardProbeSetup,
     "Forward.ProbeSetup",
     "Main View/Forward/Probe Setup"},
    {TelemetryStageId::MainForwardShadowSetup,
     "Forward.ShadowSetup",
     "Main View/Forward/Shadow Setup"},
    {TelemetryStageId::MainForwardTransparencySetup,
     "Forward.TransparencySetup",
     "Main View/Forward/Transparency Setup"},
    {TelemetryStageId::MainForwardPrepass, "Forward.Prepass", "Main View/Forward/Prepass"},
    {TelemetryStageId::MainForwardOpaque, "Forward.Opaque", "Main View/Forward/Opaque"},
    {TelemetryStageId::MainForwardTransparent,
     "Forward.Transparent",
     "Main View/Forward/Transparent"},
    {TelemetryStageId::MainForwardResolve, "Forward.Resolve", "Main View/Forward/Resolve"},
    {TelemetryStageId::MainFilterBeforePostFX,
     "Filter.BeforePostFX",
     "Main View/Filter Before PostFX"},
    {TelemetryStageId::PostMotionBlur, "MotionBlur", "Main View/Motion Blur"},
    {TelemetryStageId::PostFilterBeforeDepthOfField,
     "Filter.BeforeDepthOfField",
     "Main View/Filter Before DOF"},
    {TelemetryStageId::PostDepthOfField, "DepthOfField", "Main View/Depth of Field"},
    {TelemetryStageId::DOFSetup, "DOF.Setup", "Main View/Depth of Field/Setup"},
    {TelemetryStageId::DOFTilePrepare, "DOF.TilePrepare", "Main View/Depth of Field/Tile Prepare"},
    {TelemetryStageId::DOFBackgroundConvolution,
     "DOF.Background",
     "Main View/Depth of Field/Background"},
    {TelemetryStageId::DOFForegroundConvolution,
     "DOF.Foreground",
     "Main View/Depth of Field/Foreground"},
    {TelemetryStageId::DOFHoleFill, "DOF.HoleFill", "Main View/Depth of Field/Hole Fill"},
    {TelemetryStageId::DOFResolve, "DOF.Resolve", "Main View/Depth of Field/Resolve"},
    {TelemetryStageId::PostFilterBeforeComposite,
     "Filter.BeforeComposite",
     "Main View/Filter Before Composite"},
    {TelemetryStageId::MainFilmAccumulate, "Film.Accumulate", "Main View/Film Accumulate"},
    {TelemetryStageId::Lookdev, "Lookdev", "Lookdev"},
    {TelemetryStageId::ReadResult, "ReadResult", "Read Result"},
    {TelemetryStageId::ShadowTilemapSetup,
     "Shadow.TilemapSetup",
     "Shadow/Tilemap Setup"},
    {TelemetryStageId::ShadowCasterUpdate,
     "Shadow.CasterUpdate",
     "Shadow/Caster Update"},
    {TelemetryStageId::ShadowTransparentCasterUpdate,
     "Shadow.TransparentCasterUpdate",
     "Shadow/Transparent Caster Update"},
    {TelemetryStageId::ShadowUsageMarking,
     "Shadow.UsageMarking",
     "Shadow/Usage Marking"},
    {TelemetryStageId::ShadowTilemapUpdate,
     "Shadow.TilemapUpdate",
     "Shadow/Tilemap Update"},
    {TelemetryStageId::ShadowUpdateFinish,
     "Shadow.UpdateFinish",
     "Shadow/Update Finish"},
    {TelemetryStageId::ShadowSurface, "Shadow.Surface", "Shadow/Surface"},
}};

bool TelemetryModule::enabled() const
{
  return inst_.scene != nullptr && (inst_.scene->eevee.flag & SCE_EEVEE_PERFORMANCE_PROFILER);
}

TelemetryRuntimeMode TelemetryModule::runtime_mode() const
{
  if (inst_.is_baking()) {
    return TelemetryRuntimeMode::Bake;
  }
  if (inst_.is_viewport_image_render) {
    return TelemetryRuntimeMode::ViewportImageRender;
  }
  if (inst_.is_viewport()) {
    return TelemetryRuntimeMode::Viewport;
  }
  return TelemetryRuntimeMode::FinalRender;
}

int TelemetryModule::average_window() const
{
  if (inst_.scene == nullptr) {
    return 8;
  }
  const int value = inst_.scene->eevee.performance_profiler_average_window;
  return (value > 0) ? value : 8;
}

bool TelemetryModule::use_time_sort() const
{
  return inst_.scene != nullptr &&
         (inst_.scene->eevee.flag & SCE_EEVEE_PERFORMANCE_PROFILER_SORT_BY_TIME) != 0;
}

bool TelemetryModule::viewport_publish_paused() const
{
  return inst_.scene != nullptr && inst_.scene->eevee.performance_profiler_viewport_pause != 0;
}

void TelemetryModule::frame_begin(const TelemetryRuntimeMode mode)
{
  if (!enabled()) {
    return;
  }

  current_frame_ = {};
  inst_.light_probes.probe_costs_reset();
  current_frame_.runtime_mode = mode;
  current_frame_.frame = (inst_.scene != nullptr) ? inst_.scene->r.cfra : 0;
  current_frame_.sample_index = ELEM(mode,
                                     TelemetryRuntimeMode::Viewport,
                                     TelemetryRuntimeMode::ViewportImageRender,
                                     TelemetryRuntimeMode::Bake) ?
                                    int(inst_.sampling.viewport_sample_index()) :
                                    int(inst_.sampling.sample_index());
  current_frame_.sample_count = inst_.sampling.sample_count();
  frame_start_time_ = BLI_time_now_seconds();
  frame_active_ = true;
}

void TelemetryModule::snapshot_features()
{
  if (inst_.scene == nullptr) {
    return;
  }

  TelemetryFeatureSnapshot &features = current_frame_.features;
  features.has_ao = (inst_.scene->eevee.flag & SCE_EEVEE_GTAO_ENABLED) != 0;
  features.has_dof = inst_.depth_of_field.postfx_enabled();
  features.has_motion_blur = inst_.motion_blur.postfx_enabled();
  features.has_volume = inst_.volume.enabled();
  features.has_raytracing = inst_.raytracing.use_raytracing();
  features.filter_material_count = inst_.filter_materials.material_count();
  features.render_texture_count = BLI_listbase_count(&inst_.scene->eevee.render_textures);
  features.light_count = inst_.lights.light_count();
  features.probe_count = inst_.light_probes.probe_count();
  features.npr_material_count = inst_.materials.npr_material_count();
  features.raycast_material_count = inst_.materials.raycast_material_count();
  features.glsl_function_material_count = inst_.materials.glsl_function_material_count();
}

bool TelemetryModule::frame_has_stage_samples(const TelemetryFrameRecord &record) const
{
  for (const TelemetryStageSample &stage : record.stages) {
    if (stage.call_count > 0) {
      return true;
    }
  }
  return false;
}

void TelemetryModule::frame_end()
{
  if (!enabled() || !frame_active_) {
    return;
  }

  current_frame_.sample_index = ELEM(current_frame_.runtime_mode,
                                     TelemetryRuntimeMode::Viewport,
                                     TelemetryRuntimeMode::ViewportImageRender,
                                     TelemetryRuntimeMode::Bake) ?
                                    int(inst_.sampling.viewport_sample_index()) :
                                    int(inst_.sampling.sample_index());
  current_frame_.sample_count = inst_.sampling.sample_count();
  const double frame_end_time = BLI_time_now_seconds();
  current_frame_.total_cpu_ms = (frame_end_time - frame_start_time_) * 1000.0;
  snapshot_features();
  current_frame_.shadow_light_costs.clear();
  for (const TelemetryShadowLightCost &cost : inst_.lights.shadow_light_costs()) {
    current_frame_.shadow_light_costs.append(cost);
  }
  current_frame_.probe_costs.clear();
  for (const TelemetryProbeCost &cost : inst_.light_probes.probe_costs()) {
    current_frame_.probe_costs.append(cost);
  }
  std::sort(current_frame_.probe_costs.begin(),
            current_frame_.probe_costs.end(),
            [](const TelemetryProbeCost &a, const TelemetryProbeCost &b) {
              return a.estimated_work > b.estimated_work;
            });
  double total_probe_work = 0.0;
  for (const TelemetryProbeCost &cost : current_frame_.probe_costs) {
    total_probe_work += cost.estimated_work;
  }
  if (total_probe_work > 0.0) {
    for (TelemetryProbeCost &cost : current_frame_.probe_costs) {
      const double share_percent = (cost.estimated_work / total_probe_work) * 100.0;
      cost.level = share_percent >= 50.0 ? "HIGH" :
                   share_percent >= 20.0 ? "MEDIUM" :
                                           "LOW";
    }
  }
  const bool is_viewport_mode = ELEM(current_frame_.runtime_mode,
                                     TelemetryRuntimeMode::Viewport,
                                     TelemetryRuntimeMode::ViewportImageRender,
                                     TelemetryRuntimeMode::Bake);
  if (is_viewport_mode && !frame_has_stage_samples(current_frame_)) {
    frame_active_ = false;
    return;
  }

  const int mode_index = int(current_frame_.runtime_mode);
  last_records_[mode_index] = current_frame_;
  has_last_record_[mode_index] = true;

  Vector<TelemetryFrameRecord> &history = history_[mode_index];
  history.append(current_frame_);
  if (history.size() > history_limit_) {
    history.remove(0);
  }

  if (inst_.scene != nullptr) {
    Scene *scene_orig = DEG_get_original(inst_.scene);
    Scene *notify_scene = (scene_orig != nullptr) ? scene_orig : inst_.scene;
    bool reports_changed = false;

    const bool publish_viewport = is_viewport_mode &&
                                  (!has_viewport_publish_ ||
                                   ((frame_end_time - last_viewport_publish_time_) >=
                                    viewport_publish_interval_seconds_) ||
                                   inst_.sampling.finished_viewport());

    if (publish_viewport) {
      maybe_publish_cached_viewport(inst_.sampling.finished_viewport());
    }

    if (current_frame_.runtime_mode == TelemetryRuntimeMode::FinalRender) {
      const std::string render_summary = render_report();
      const bke::SceneEeveePerformanceRuntime *notify_runtime =
          scene_eevee_performance_runtime(notify_scene);
      reports_changed |= (notify_runtime == nullptr) || (notify_runtime->render_report != render_summary);
      if (bke::SceneEeveePerformanceRuntime *runtime =
              scene_eevee_performance_runtime(inst_.scene))
      {
        runtime->render_report = render_summary;
      }
      if (scene_orig != nullptr && scene_orig != inst_.scene) {
        if (bke::SceneEeveePerformanceRuntime *runtime =
                scene_eevee_performance_runtime(scene_orig))
        {
          runtime->render_report = render_summary;
        }
      }
    }
    if (reports_changed) {
      WM_main_add_notifier(NC_SCENE | ND_RENDER_OPTIONS, notify_scene);
    }
  }

  frame_active_ = false;
}

void TelemetryModule::maybe_begin_viewport_frame()
{
  if (!enabled() || frame_active_) {
    return;
  }
  if (ELEM(runtime_mode(),
           TelemetryRuntimeMode::Viewport,
           TelemetryRuntimeMode::ViewportImageRender,
           TelemetryRuntimeMode::Bake))
  {
    frame_begin(runtime_mode());
  }
}

void TelemetryModule::maybe_end_viewport_frame()
{
  if (!enabled() || !frame_active_) {
    return;
  }
  if (ELEM(runtime_mode(),
           TelemetryRuntimeMode::Viewport,
           TelemetryRuntimeMode::ViewportImageRender,
           TelemetryRuntimeMode::Bake))
  {
    frame_end();
  }
}

void TelemetryModule::maybe_begin_final_frame()
{
  if (!enabled() || frame_active_) {
    return;
  }
  if (runtime_mode() == TelemetryRuntimeMode::FinalRender) {
    frame_begin(TelemetryRuntimeMode::FinalRender);
  }
}

void TelemetryModule::maybe_end_final_frame()
{
  if (!enabled() || !frame_active_) {
    return;
  }
  if (runtime_mode() == TelemetryRuntimeMode::FinalRender) {
    frame_end();
  }
}

void TelemetryModule::cancel_frame()
{
  frame_active_ = false;
}

void TelemetryModule::maybe_publish_cached_viewport(const bool force)
{
  if (!enabled() || inst_.scene == nullptr || viewport_publish_paused()) {
    return;
  }

  const TelemetryFrameRecord *record = last_record(TelemetryRuntimeMode::Viewport);
  if (record == nullptr) {
    record = last_record(TelemetryRuntimeMode::ViewportImageRender);
  }
  if (record == nullptr) {
    record = last_record(TelemetryRuntimeMode::Bake);
  }
  if (record == nullptr) {
    return;
  }

  const double now = BLI_time_now_seconds();
  if (!force && has_viewport_publish_ &&
      ((now - last_viewport_publish_time_) < viewport_publish_interval_seconds_))
  {
    return;
  }

  const std::string viewport_summary = viewport_summary_line();
  const std::string viewport_report = this->viewport_report();
  Scene *scene_orig = DEG_get_original(inst_.scene);
  Scene *notify_scene = (scene_orig != nullptr) ? scene_orig : inst_.scene;
  const bke::SceneEeveePerformanceRuntime *notify_runtime =
      scene_eevee_performance_runtime(notify_scene);
  const bool reports_changed = (notify_runtime == nullptr) ||
                               (notify_runtime->viewport_summary != viewport_summary) ||
                               (notify_runtime->viewport_report != viewport_report);

  if (bke::SceneEeveePerformanceRuntime *runtime =
          scene_eevee_performance_runtime(inst_.scene))
  {
    runtime->viewport_summary = viewport_summary;
    runtime->viewport_report = viewport_report;
  }
  if (scene_orig != nullptr && scene_orig != inst_.scene) {
    if (bke::SceneEeveePerformanceRuntime *runtime =
            scene_eevee_performance_runtime(scene_orig))
    {
      runtime->viewport_summary = viewport_summary;
      runtime->viewport_report = viewport_report;
    }
  }

  last_viewport_publish_time_ = now;
  has_viewport_publish_ = true;

  if (reports_changed) {
    WM_main_add_notifier(NC_SCENE | ND_RENDER_OPTIONS, notify_scene);
  }
}

void TelemetryModule::reset()
{
  frame_active_ = false;
  last_viewport_publish_time_ = 0.0;
  has_viewport_publish_ = false;
}

void TelemetryModule::stage_add(const TelemetryStageId stage, const double elapsed_seconds)
{
  if (!enabled() || !frame_active_) {
    return;
  }
  TelemetryStageSample &sample = current_frame_.stages[int(stage)];
  sample.cpu_ms += elapsed_seconds * 1000.0;
  sample.call_count += 1;
}

void TelemetryModule::shadow_context_add(const TelemetryShadowContext context,
                                         const double elapsed_seconds,
                                         const int loop_count)
{
  if (!enabled() || !frame_active_ || context == TelemetryShadowContext::Count) {
    return;
  }
  TelemetryShadowContextSample &sample = current_frame_.shadow_contexts[int(context)];
  sample.cpu_ms += elapsed_seconds * 1000.0;
  sample.call_count += 1;
  sample.loop_count += loop_count;
}

void TelemetryModule::material_sync_add(const bool shader_queued,
                                        const bool optimize_queued,
                                        const bool fallback,
                                        const bool failed,
                                        const char *material_name)
{
  if (!enabled() || !frame_active_) {
    return;
  }
  const auto add_sample = [&](TelemetryMaterialSyncSample &sample) {
    sample.request_count += 1;
    sample.shader_queued_count += int64_t(shader_queued);
    sample.optimize_queued_count += int64_t(optimize_queued);
    sample.fallback_count += int64_t(fallback);
    sample.failed_count += int64_t(failed);
  };
  add_sample(current_frame_.material_sync);
  const std::string hotspot_name = (material_name != nullptr && material_name[0] != '\0') ?
                                       material_name :
                                       "<None>";
  for (TelemetryMaterialHotspot &hotspot : current_frame_.material_hotspots) {
    if (hotspot.name != hotspot_name) {
      continue;
    }
    hotspot.request_count += 1;
    hotspot.shader_queued_count += int64_t(shader_queued);
    hotspot.optimize_queued_count += int64_t(optimize_queued);
    hotspot.fallback_count += int64_t(fallback);
    hotspot.failed_count += int64_t(failed);
    return;
  }
  TelemetryMaterialHotspot hotspot;
  hotspot.name = hotspot_name;
  hotspot.request_count = 1;
  hotspot.shader_queued_count = int64_t(shader_queued);
  hotspot.optimize_queued_count = int64_t(optimize_queued);
  hotspot.fallback_count = int64_t(fallback);
  hotspot.failed_count = int64_t(failed);
  current_frame_.material_hotspots.append(hotspot);
}

void TelemetryModule::shader_wait_add(const int64_t queued_shaders,
                                      const int64_t queued_textures,
                                      const double elapsed_seconds)
{
  if (!enabled() || !frame_active_) {
    return;
  }

  TelemetryShaderWaitSample &sample = current_frame_.shader_waits;
  sample.wait_count += 1;
  sample.queued_shader_count += queued_shaders;
  sample.queued_texture_count += queued_textures;
  sample.cpu_ms += elapsed_seconds * 1000.0;
}

void TelemetryModule::pass_readback_add(const TelemetryPassReadbackType type,
                                        const char *name,
                                        const int width,
                                        const int height,
                                        const int channels,
                                        const double elapsed_seconds)
{
  if (!enabled() || !frame_active_ || type == TelemetryPassReadbackType::Count || width <= 0 ||
      height <= 0 || channels <= 0)
  {
    return;
  }

  TelemetryPassReadbackSample &sample = current_frame_.pass_readbacks[int(type)];
  sample.pass_count += 1;
  sample.pixel_count += int64_t(width) * int64_t(height);
  sample.output_value_count += int64_t(width) * int64_t(height) * int64_t(channels);
  sample.cpu_ms += elapsed_seconds * 1000.0;
  if (name != nullptr && name[0] != '\0' && sample.pass_count <= pass_readback_name_limit) {
    if (!sample.names.empty()) {
      sample.names += ", ";
    }
    sample.names += name;
  }
}

const TelemetryFrameRecord *TelemetryModule::last_record(const TelemetryRuntimeMode mode) const
{
  const int mode_index = int(mode);
  return has_last_record_[mode_index] ? &last_records_[mode_index] : nullptr;
}

double TelemetryModule::averaged_total_cpu_ms(const TelemetryRuntimeMode mode) const
{
  const Vector<TelemetryFrameRecord> &history = history_[int(mode)];
  if (history.is_empty()) {
    return 0.0;
  }

  const int window = min_ii(average_window(), history.size());
  double total = 0.0;
  for (int offset = 0; offset < window; offset++) {
    total += history[history.size() - 1 - offset].total_cpu_ms;
  }
  return total / double(window);
}

std::array<double, stage_count> TelemetryModule::averaged_stage_values(
    const TelemetryRuntimeMode mode) const
{
  std::array<double, stage_count> result{};
  const Vector<TelemetryFrameRecord> &history = history_[int(mode)];
  if (history.is_empty()) {
    return result;
  }

  const int window = min_ii(average_window(), history.size());
  for (int offset = 0; offset < window; offset++) {
    const TelemetryFrameRecord &record = history[history.size() - 1 - offset];
    for (int stage_index = 0; stage_index < stage_count; stage_index++) {
      result[stage_index] += record.stages[stage_index].cpu_ms;
    }
  }
  for (double &value : result) {
    value /= double(window);
  }
  return result;
}

const char *TelemetryModule::stage_label(const TelemetryStageId stage)
{
  return (stage == TelemetryStageId::Count) ? "Unknown" : stage_info(stage).label;
}

const char *TelemetryModule::shadow_context_label(const TelemetryShadowContext context)
{
  switch (context) {
    case TelemetryShadowContext::MainView:
      return "MainView";
    case TelemetryShadowContext::PlanarProbe:
      return "PlanarProbe";
    case TelemetryShadowContext::CaptureProbe:
      return "CaptureProbe";
    case TelemetryShadowContext::Bake:
      return "Bake";
    case TelemetryShadowContext::Other:
      return "Other";
    case TelemetryShadowContext::Count:
      break;
  }
  return "Unknown";
}

const char *TelemetryModule::pass_readback_type_label(const TelemetryPassReadbackType type)
{
  switch (type) {
    case TelemetryPassReadbackType::RenderPass:
      return "RenderPass";
    case TelemetryPassReadbackType::AOV:
      return "AOV";
    case TelemetryPassReadbackType::NativePostFX:
      return "NativePostFX";
    case TelemetryPassReadbackType::Count:
      break;
  }
  return "Unknown";
}

const TelemetryStageInfo &TelemetryModule::stage_info(const TelemetryStageId stage)
{
  return telemetry_stage_info[(stage == TelemetryStageId::Count) ? 0 : int(stage)];
}

Span<const TelemetryStageInfo> TelemetryModule::stage_infos()
{
  return Span<const TelemetryStageInfo>(telemetry_stage_info.data(), telemetry_stage_info.size());
}

std::string TelemetryModule::viewport_summary_line() const
{
  const TelemetryFrameRecord *record = last_record(TelemetryRuntimeMode::Viewport);
  if (record == nullptr) {
    record = last_record(TelemetryRuntimeMode::ViewportImageRender);
  }
  if (record == nullptr) {
    return "";
  }

  const double total_cpu_ms = averaged_total_cpu_ms(record->runtime_mode);
  const auto averages = averaged_stage_values(record->runtime_mode);
  const double sync_ms = averages[int(TelemetryStageId::SyncBegin)] +
                         averages[int(TelemetryStageId::SyncObjects)] +
                         averages[int(TelemetryStageId::SyncEnd)];
  const double main_ms = averages[int(TelemetryStageId::MainView)];
  const double deferred_ms = averages[int(TelemetryStageId::MainDeferred)];
  const double dof_ms = averages[int(TelemetryStageId::PostDepthOfField)];
  const double filter_ms = averages[int(TelemetryStageId::MainFilterBeforeVolumeFog)] +
                           averages[int(TelemetryStageId::MainFilterBeforePostFX)] +
                           averages[int(TelemetryStageId::PostFilterBeforeDepthOfField)] +
                           averages[int(TelemetryStageId::PostFilterBeforeComposite)];

  return fmt::format("Perf CPU {:.2f} ms | Sample {} ({}) | Sync {:.2f} | Main {:.2f} | Deferred {:.2f} | DOF {:.2f} | Filter {:.2f}",
                     total_cpu_ms,
                     sample_progress_string(*record),
                     sample_status_string(*record),
                     sync_ms,
                     main_ms,
                     deferred_ms,
                     dof_ms,
                     filter_ms);
}

Vector<int> TelemetryModule::sorted_stage_indices(const TelemetryFrameRecord &record) const
{
  Vector<int> indices;
  indices.reserve(stage_count);
  for (int stage_index = 0; stage_index < stage_count; stage_index++) {
    indices.append(stage_index);
  }

  if (use_time_sort()) {
    std::sort(indices.begin(), indices.end(), [&](const int a, const int b) {
      return record.stages[a].cpu_ms > record.stages[b].cpu_ms;
    });
  }
  return indices;
}

std::string TelemetryModule::format_shadow_lights_report(const TelemetryFrameRecord &record) const
{
  static constexpr int shadow_light_report_limit = 16;
  if (record.shadow_light_costs.is_empty()) {
    return "";
  }

  std::string result = "  Shadow Lights:\n";
  const int light_count = min_ii(shadow_light_report_limit, record.shadow_light_costs.size());
  for (const int index : IndexRange(light_count)) {
    const TelemetryShadowLightCost &cost = record.shadow_light_costs[index];
    result += fmt::format(
        "    - {} type={} tilemaps={} estimated_views={} sync_dirty_tilemaps={} "
        "tilemap_view_share={:.1f}% level={}\n",
        cost.name,
        cost.type,
        cost.tilemaps,
        cost.estimated_views,
        cost.sync_dirty_tilemaps,
        cost.estimated_share_percent,
        cost.level);
  }
  if (record.shadow_light_costs.size() > shadow_light_report_limit) {
    result += fmt::format("    - ... {} more shadow lights\n",
                          record.shadow_light_costs.size() - shadow_light_report_limit);
  }
  return result;
}

std::string TelemetryModule::format_shadow_contexts_report(const TelemetryFrameRecord &record) const
{
  double total_context_cpu_ms = 0.0;
  int active_contexts = 0;
  for (const TelemetryShadowContextSample &sample : record.shadow_contexts) {
    total_context_cpu_ms += sample.cpu_ms;
    active_contexts += int(sample.call_count > 0);
  }
  if (active_contexts == 0) {
    return "";
  }

  std::string result = "  Shadow Contexts:\n";
  for (int context_index = 0; context_index < shadow_context_count; context_index++) {
    const TelemetryShadowContextSample &sample = record.shadow_contexts[context_index];
    const double ms_per_call = (sample.call_count > 0) ?
                                   sample.cpu_ms / double(sample.call_count) :
                                   0.0;
    const double ms_per_loop = (sample.loop_count > 0) ? sample.cpu_ms / double(sample.loop_count) :
                                                        0.0;
    const double share_percent = (total_context_cpu_ms > 0.0) ?
                                     (sample.cpu_ms / total_context_cpu_ms) * 100.0 :
                                     0.0;
    result += fmt::format(
        "    - {} cpu={:.3f} ms calls={} loops={} ms/call={:.3f} ms/loop={:.3f} share={:.1f}%\n",
        shadow_context_label(TelemetryShadowContext(context_index)),
        sample.cpu_ms,
        sample.call_count,
        sample.loop_count,
        ms_per_call,
        ms_per_loop,
        share_percent);
  }
  return result;
}

std::string TelemetryModule::format_shader_waits_report(const TelemetryFrameRecord &record) const
{
  const TelemetryShaderWaitSample &sample = record.shader_waits;
  if (sample.wait_count == 0) {
    return "";
  }

  const double sync_end_cpu = record.stages[int(TelemetryStageId::SyncEnd)].cpu_ms;
  const double sync_end_share = (sync_end_cpu > 0.0) ? (sample.cpu_ms / sync_end_cpu) * 100.0 :
                                                       0.0;
  return fmt::format(
      "  Shader Waits:\n"
      "    - waits={} cpu={:.3f} ms sync_end_share={:.1f}% queued_shaders={} queued_textures={}\n",
      sample.wait_count,
      sample.cpu_ms,
      sync_end_share,
      sample.queued_shader_count,
      sample.queued_texture_count);
}

std::string TelemetryModule::format_pass_readbacks_report(const TelemetryFrameRecord &record) const
{
  int64_t total_passes = 0;
  int64_t total_pixels = 0;
  int64_t total_output_values = 0;
  double total_cpu_ms = 0.0;
  for (const TelemetryPassReadbackSample &sample : record.pass_readbacks) {
    total_passes += sample.pass_count;
    total_pixels += sample.pixel_count;
    total_output_values += sample.output_value_count;
    total_cpu_ms += sample.cpu_ms;
  }
  if (total_passes == 0) {
    return "";
  }

  std::string result = "  Pass Readback:\n";
  const double total_data_mb = double(total_output_values) * double(sizeof(float)) / 1000000.0;
  result += fmt::format(
      "    - Total passes={} pixels={:.3f}M values={:.3f}M data={:.3f}MB cpu={:.3f} ms\n",
      total_passes,
      double(total_pixels) / 1000000.0,
      double(total_output_values) / 1000000.0,
      total_data_mb,
      total_cpu_ms);
  for (int type_index = 0; type_index < pass_readback_type_count; type_index++) {
    const TelemetryPassReadbackSample &sample = record.pass_readbacks[type_index];
    if (sample.pass_count == 0) {
      continue;
    }
    const double share_percent = (total_cpu_ms > 0.0) ? (sample.cpu_ms / total_cpu_ms) * 100.0 :
                                                        0.0;
    const double pixels_m = double(sample.pixel_count) / 1000000.0;
    const double values_m = double(sample.output_value_count) / 1000000.0;
    const double data_mb = double(sample.output_value_count) * double(sizeof(float)) / 1000000.0;
    std::string names = sample.names;
    if (!names.empty() && sample.pass_count > pass_readback_name_limit) {
      names += fmt::format(", ... {} more", sample.pass_count - pass_readback_name_limit);
    }
    result += fmt::format(
        "    - {} passes={} cpu={:.3f} ms readback_share={:.1f}% pixels={:.3f}M values={:.3f}M data={:.3f}MB names=[{}]\n",
        pass_readback_type_label(TelemetryPassReadbackType(type_index)),
        sample.pass_count,
        sample.cpu_ms,
        share_percent,
        pixels_m,
        values_m,
        data_mb,
        names);
  }
  return result;
}

std::string TelemetryModule::format_material_sync_report(const TelemetryFrameRecord &record) const
{
  const TelemetryMaterialSyncSample &total = record.material_sync;
  const int64_t total_requests = total.request_count;
  if (total_requests == 0) {
    return "";
  }
  Vector<TelemetryMaterialHotspot> hotspots = record.material_hotspots;
  std::sort(hotspots.begin(),
            hotspots.end(),
            [](const TelemetryMaterialHotspot &a, const TelemetryMaterialHotspot &b) {
              const int64_t a_penalty = a.failed_count * 1000000 + a.fallback_count * 10000 +
                                        a.shader_queued_count * 100 + a.optimize_queued_count * 10;
              const int64_t b_penalty = b.failed_count * 1000000 + b.fallback_count * 10000 +
                                        b.shader_queued_count * 100 + b.optimize_queued_count * 10;
              if (a_penalty != b_penalty) {
                return a_penalty > b_penalty;
              }
              return a.request_count > b.request_count;
            });

  std::string result = "  Material Sync:\n";
  result += fmt::format(
      "    - Total requests={} shader_queued={} optimize_queued={} fallbacks={} failed={}\n",
      total_requests,
      total.shader_queued_count,
      total.optimize_queued_count,
      total.fallback_count,
      total.failed_count);

  if (!hotspots.is_empty()) {
    const int hotspot_count = min_ii(material_hotspot_report_limit, hotspots.size());
    for (const int index : IndexRange(hotspot_count)) {
      const TelemetryMaterialHotspot &hotspot = hotspots[index];
      const double share_percent = (total_requests > 0) ?
                                       (double(hotspot.request_count) / double(total_requests)) *
                                           100.0 :
                                       0.0;
      result += fmt::format(
          "    - Material {} requests={} request_share={:.1f}% shader_queued={} optimize_queued={} fallbacks={} failed={}\n",
          hotspot.name,
          hotspot.request_count,
          share_percent,
          hotspot.shader_queued_count,
          hotspot.optimize_queued_count,
          hotspot.fallback_count,
          hotspot.failed_count);
    }
    if (hotspots.size() > material_hotspot_report_limit) {
      result += fmt::format("    - ... {} more materials\n",
                            hotspots.size() - material_hotspot_report_limit);
    }
  }

  return result;
}

std::string TelemetryModule::format_probe_costs_report(const TelemetryFrameRecord &record) const
{
  if (record.probe_costs.is_empty()) {
    return "";
  }

  double total_work = 0.0;
  for (const TelemetryProbeCost &cost : record.probe_costs) {
    total_work += cost.estimated_work;
  }

  std::string result = "  Probe Costs:\n";
  for (const TelemetryProbeCost &cost : record.probe_costs) {
    const double share_percent = (total_work > 0.0) ?
                                     (cost.estimated_work / total_work) * 100.0 :
                                     0.0;
    result += fmt::format(
        "    - {} type={} updated={} total={} rendered_views={} resolution={} "
        "estimated_work={:.3f}M work_share={:.1f}% level={}\n",
        cost.name,
        cost.type,
        cost.updated,
        cost.total,
        cost.rendered_views,
        cost.resolution,
        cost.estimated_work,
        share_percent,
        cost.level);
  }
  return result;
}

Vector<std::string> TelemetryModule::viewport_overlay_lines(const bool include_stage_list) const
{
  const TelemetryFrameRecord *record = last_record(TelemetryRuntimeMode::Viewport);
  if (record == nullptr) {
    record = last_record(TelemetryRuntimeMode::ViewportImageRender);
  }
  if (record == nullptr) {
    return {};
  }

  const auto averages = averaged_stage_values(record->runtime_mode);
  Vector<std::string> lines;
  lines.append(viewport_summary_line());
  lines.append(fmt::format("Feat AO={} DOF={} MB={} Vol={} RT={}",
                           record->features.has_ao ? "On" : "Off",
                           record->features.has_dof ? "On" : "Off",
                           record->features.has_motion_blur ? "On" : "Off",
                           record->features.has_volume ? "On" : "Off",
                           record->features.has_raytracing ? "On" : "Off"));
  lines.append(fmt::format("Cnt F={} RTex={} L={} P={} NPR={} Ray={} GLSL={}",
                           record->features.filter_material_count,
                           record->features.render_texture_count,
                           record->features.light_count,
                           record->features.probe_count,
                           record->features.npr_material_count,
                           record->features.raycast_material_count,
                           record->features.glsl_function_material_count));

  if (!include_stage_list) {
    return lines;
  }

  {
    const int stage_index = int(TelemetryStageId::MainUpdateView);
    const TelemetryStageSample &stage = record->stages[stage_index];
    const double avg_ms = averages[stage_index];
    lines.append(fmt::format("MV.Update               {:>6.3f} | {:>6.3f} | c{}",
                             stage.cpu_ms,
                             avg_ms,
                             stage.call_count));
  }

  int display_index = 1;
  for (const int stage_index : sorted_stage_indices(*record)) {
    if (stage_index == int(TelemetryStageId::MainUpdateView)) {
      continue;
    }
    const TelemetryStageSample &stage = record->stages[stage_index];
    const double avg_ms = averages[stage_index];
    if (stage.cpu_ms <= 0.1 && avg_ms <= 0.05) {
      continue;
    }
    lines.append(fmt::format("{:>2}. {:<22} {:>6.3f} | {:>6.3f} | c{}",
                             display_index++,
                             stage_label(TelemetryStageId(stage_index)),
                             stage.cpu_ms,
                             avg_ms,
                             stage.call_count));
  }
  return lines;
}

std::string TelemetryModule::viewport_report() const
{
  const TelemetryFrameRecord *record = last_record(TelemetryRuntimeMode::Viewport);
  if (record == nullptr) {
    record = last_record(TelemetryRuntimeMode::ViewportImageRender);
  }
  if (record == nullptr) {
    return "";
  }

  const auto averages = averaged_stage_values(record->runtime_mode);
  std::string result = fmt::format(
      "Viewport CPU: {:.3f} ms\n"
      "Frame: {}\n"
      "Sample Progress: {}\n"
      "Sampling: {}\n"
      "Features: AO={} DOF={} MB={} Volume={} Raytrace={} Filters={} RenderTextures={} Lights={} Probes={} NPR Mats={} Raycast Mats={} GLSL Mats={}\n",
      record->total_cpu_ms,
      record->frame,
      sample_progress_string(*record),
      sample_status_string(*record),
      record->features.has_ao ? "On" : "Off",
      record->features.has_dof ? "On" : "Off",
      record->features.has_motion_blur ? "On" : "Off",
      record->features.has_volume ? "On" : "Off",
      record->features.has_raytracing ? "On" : "Off",
      record->features.filter_material_count,
      record->features.render_texture_count,
      record->features.light_count,
      record->features.probe_count,
      record->features.npr_material_count,
      record->features.raycast_material_count,
      record->features.glsl_function_material_count);

  for (const int stage_index : sorted_stage_indices(*record)) {
    const TelemetryStageSample &stage = record->stages[stage_index];
    const double avg_ms = averages[stage_index];
    const double ms_per_call = (stage.call_count > 0) ? stage.cpu_ms / double(stage.call_count) :
                                                        0.0;
    result += fmt::format(
        "  - {:<26} {:>8.3f} ms | avg {:>8.3f} | calls {} | {:.3f} ms/call\n",
                          stage_label(TelemetryStageId(stage_index)),
                          stage.cpu_ms,
                          avg_ms,
        stage.call_count,
        ms_per_call);
  }
  result += format_shadow_contexts_report(*record);
  result += format_shadow_lights_report(*record);
  result += format_probe_costs_report(*record);
  return result;
}

std::string TelemetryModule::render_report() const
{
  const TelemetryFrameRecord *record = last_record(TelemetryRuntimeMode::FinalRender);
  if (record == nullptr) {
    return "";
  }

  std::string result = fmt::format(
      "EEVEE Performance Summary\n"
      "  Frame: {}\n"
      "  Sample Progress: {}\n"
      "  Sampling: {}\n"
      "  Sample Count: {}\n"
      "  Total CPU: {:.3f} ms\n"
      "  Features: AO={} DOF={} MB={} Volume={} Raytrace={} Filters={} RenderTextures={} Lights={} Probes={} NPR Mats={} Raycast Mats={} GLSL Mats={}\n",
      record->frame,
      sample_progress_string(*record),
      sample_status_string(*record),
      record->sample_count,
      record->total_cpu_ms,
      record->features.has_ao ? "On" : "Off",
      record->features.has_dof ? "On" : "Off",
      record->features.has_motion_blur ? "On" : "Off",
      record->features.has_volume ? "On" : "Off",
      record->features.has_raytracing ? "On" : "Off",
      record->features.filter_material_count,
      record->features.render_texture_count,
      record->features.light_count,
      record->features.probe_count,
      record->features.npr_material_count,
      record->features.raycast_material_count,
      record->features.glsl_function_material_count);

  for (const int stage_index : sorted_stage_indices(*record)) {
    const TelemetryStageSample &stage = record->stages[stage_index];
    const double value = stage.cpu_ms;
    const double ms_per_call = (stage.call_count > 0) ? value / double(stage.call_count) : 0.0;
    const double ms_per_sample = (record->sample_count > 0) ?
                                     value / double(record->sample_count) :
                                     0.0;
    result += fmt::format(
        "  - {:<26} {:>8.3f} ms ({} call{}, {:.3f} ms/call, {:.3f} ms/sample)\n",
        stage_label(TelemetryStageId(stage_index)),
        value,
        stage.call_count,
        stage.call_count == 1 ? "" : "s",
        ms_per_call,
        ms_per_sample);
  }
  result += format_shadow_contexts_report(*record);
  result += format_shadow_lights_report(*record);
  result += format_probe_costs_report(*record);
  return result;
}

ScopedTelemetrySample::ScopedTelemetrySample(TelemetryModule &telemetry, const TelemetryStageId stage)
    : telemetry_(telemetry.enabled() && telemetry.frame_active() ? &telemetry : nullptr),
      stage_(stage)
{
  if (telemetry_ != nullptr) {
    start_time_ = BLI_time_now_seconds();
  }
}

ScopedTelemetrySample::~ScopedTelemetrySample()
{
  if (telemetry_ != nullptr) {
    telemetry_->stage_add(stage_, BLI_time_now_seconds() - start_time_);
  }
}

}  // namespace blender::eevee
