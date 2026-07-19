/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <sstream>

#include <fmt/format.h>

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_time.h"
#include "BLI_threads.h"

#include "DEG_depsgraph_query.hh"

#include "BKE_scene_runtime.hh"

#include "DNA_scene_types.h"

#include "RE_engine.h"

#include "WM_api.hh"

#include "eevee_instance.hh"

#include "eevee_telemetry.hh"

namespace blender::eevee {

static constexpr int stage_count = int(TelemetryStageId::Count);
static constexpr int shadow_context_count = int(TelemetryShadowContext::Count);
static constexpr int pass_readback_type_count = int(TelemetryPassReadbackType::Count);
static constexpr int material_hotspot_report_limit = 8;
static constexpr int material_hotspot_tracking_limit = 32;
static constexpr int pass_readback_name_limit = 8;
static std::atomic<uint64_t> next_telemetry_source_id{1};

uint64_t telemetry_source_id_generate()
{
  return next_telemetry_source_id.fetch_add(1, std::memory_order_relaxed);
}

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

static Vector<std::string> split_overlay_text(const std::string &text)
{
  Vector<std::string> lines;
  std::stringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      lines.append(line);
    }
  }
  return lines;
}

static int scope_node_find_or_add(TelemetryFrameRecord &record,
                                  const int parent_index,
                                  const TelemetryStageId stage)
{
  for (const int child_index : record.scope_nodes[parent_index].children) {
    if (record.scope_nodes[child_index].stage == stage) {
      return child_index;
    }
  }

  TelemetryScopeNode node;
  node.stage = stage;
  const int node_index = record.scope_nodes.size();
  record.scope_nodes.append(std::move(node));
  record.scope_nodes[parent_index].children.append(node_index);
  return node_index;
}

static void scope_node_add_sample(TelemetryFrameRecord &record,
                                  const int parent_index,
                                  const TelemetryStageId stage,
                                  const double cpu_ms)
{
  const int node_index = scope_node_find_or_add(record, parent_index, stage);
  TelemetryScopeNode &node = record.scope_nodes[node_index];
  node.cpu_ms += cpu_ms;
  node.call_count += 1;
}

static const TelemetryScopeNode *scope_node_find_path(
    const TelemetryFrameRecord &record, const Vector<TelemetryStageId> &path)
{
  if (record.scope_nodes.is_empty()) {
    return nullptr;
  }

  int parent_index = 0;
  for (const TelemetryStageId stage : path) {
    int matching_child_index = -1;
    for (const int child_index : record.scope_nodes[parent_index].children) {
      if (record.scope_nodes[child_index].stage == stage) {
        matching_child_index = child_index;
        break;
      }
    }
    if (matching_child_index < 0) {
      return nullptr;
    }
    parent_index = matching_child_index;
  }
  return &record.scope_nodes[parent_index];
}

static void scope_node_averages_update(TelemetryFrameRecord &record,
                                       const Vector<TelemetryFrameRecord> &history,
                                       const int average_count,
                                       const int node_index,
                                       Vector<TelemetryStageId> &path)
{
  TelemetryScopeNode &node = record.scope_nodes[node_index];
  path.append(node.stage);

  double total_cpu_ms = 0.0;
  /* Keep the window denominator stable: an absent path contributes zero for that frame. */
  for (int offset = 0; offset < average_count; offset++) {
    const TelemetryFrameRecord &history_record = history[history.size() - 1 - offset];
    if (const TelemetryScopeNode *history_node = scope_node_find_path(history_record, path)) {
      total_cpu_ms += history_node->cpu_ms;
    }
  }
  node.average_cpu_ms = (average_count > 0) ?
                            total_cpu_ms / double(average_count) :
                            0.0;

  for (const int child_index : node.children) {
    scope_node_averages_update(record, history, average_count, child_index, path);
  }
  path.pop_last();
}

static void scope_node_reparent_from_root(TelemetryFrameRecord &record,
                                          const TelemetryStageId stage,
                                          const int new_parent_index)
{
  int child_index = -1;
  for (const int root_child_index : record.scope_nodes[0].children) {
    if (record.scope_nodes[root_child_index].stage == stage) {
      child_index = root_child_index;
      break;
    }
  }
  if (child_index < 0) {
    return;
  }

  for (int index = 0; index < record.scope_nodes[0].children.size(); index++) {
    if (record.scope_nodes[0].children[index] == child_index) {
      record.scope_nodes[0].children.remove(index);
      break;
    }
  }
  record.scope_nodes[new_parent_index].children.append(child_index);
}

static const char *telemetry_mode_label(const TelemetryRuntimeMode mode)
{
  switch (mode) {
    case TelemetryRuntimeMode::Viewport:
      return "VIEWPORT";
    case TelemetryRuntimeMode::ViewportImageRender:
      return "VIEWPORT_RENDER";
    case TelemetryRuntimeMode::FinalRender:
      return "FINAL_RENDER";
    case TelemetryRuntimeMode::Bake:
      return "BAKE";
    case TelemetryRuntimeMode::Count:
      break;
  }
  return "UNKNOWN";
}

static bke::SceneEeveePerformanceNode snapshot_scope_node(
    const TelemetryFrameRecord &record,
    const int node_index,
    const std::string &parent_id)
{
  const TelemetryScopeNode &source = record.scope_nodes[node_index];
  const TelemetryStageInfo &info = TelemetryModule::stage_info(source.stage);
  bke::SceneEeveePerformanceNode result;
  result.id = parent_id + "/stage-" + std::to_string(int(source.stage));
  result.kind = (source.stage >= TelemetryStageId::DrawSyncShared) ? "phase" : "stage";
  result.label = info.label;
  result.current_ms = source.cpu_ms;
  result.average_ms = source.average_cpu_ms;
  result.calls = source.call_count;
  result.active = source.call_count > 0;
  double children_ms = 0.0;
  for (const int child_index : source.children) {
    bke::SceneEeveePerformanceNode child = snapshot_scope_node(record, child_index, result.id);
    children_ms += child.current_ms;
    result.children.push_back(std::move(child));
  }
  result.self_ms = std::max(0.0, result.current_ms - children_ms);
  return result;
}

static constexpr std::array<TelemetryStageInfo, stage_count> telemetry_stage_info = {{
    {TelemetryStageId::SyncBegin, "Sync.Begin", "Sync/Begin"},
    {TelemetryStageId::SyncObjects, "Sync.Objects", "Sync/Objects"},
    {TelemetryStageId::SyncEnd, "Sync.End", "Sync/End"},
    {TelemetryStageId::SyncBeginWorld, "Sync.Begin.World", "Sync/Begin/World"},
    {TelemetryStageId::SyncBeginSceneModules,
     "Sync.Begin.SceneModules",
     "Sync/Begin/Scene Modules"},
    {TelemetryStageId::SyncBeginViewEffects,
     "Sync.Begin.ViewEffects",
     "Sync/Begin/View Effects"},
    {TelemetryStageId::SyncBeginNPRPost, "Sync.Begin.NPRPost", "Sync/Begin/NPR Post"},
    {TelemetryStageId::SyncEndShaderReadiness,
     "Sync.End.ShaderReadiness",
     "Sync/End/Shader Readiness"},
    {TelemetryStageId::SyncEndMaterialsVelocity,
     "Sync.End.MaterialsVelocity",
     "Sync/End/Materials + Velocity"},
    {TelemetryStageId::SyncEndVolumeShadowsLights,
     "Sync.End.VolumeShadowsLights",
     "Sync/End/Volume + Shadows + Lights"},
    {TelemetryStageId::SyncEndFrameState,
     "Sync.End.FrameState",
     "Sync/End/Frame State"},
    {TelemetryStageId::SyncEndNPRPost, "Sync.End.NPRPost", "Sync/End/NPR Post"},
    {TelemetryStageId::SyncEndProbesUniforms,
     "Sync.End.ProbesUniforms",
     "Sync/End/Probes + Uniforms"},
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
    {TelemetryStageId::DrawSyncShared, "Draw.Sync.Shared", "Draw Sync (Shared)"},
    {TelemetryStageId::DrawSyncEngineSetup,
     "Draw.Sync.EngineSetup",
     "Draw Sync (Shared)/Engine Setup"},
    {TelemetryStageId::DrawSyncEngineInit, "Draw.Sync.EngineInit", "Draw Sync (Shared)/Engine Init"},
    {TelemetryStageId::DrawSyncManagerBegin,
     "Draw.Sync.ManagerBegin",
     "Draw Sync (Shared)/Manager Begin"},
    {TelemetryStageId::DrawSyncEngineBegin,
     "Draw.Sync.EngineBegin",
     "Draw Sync (Shared)/Engine Begin Sync"},
    {TelemetryStageId::DrawSyncModulesBegin,
     "Draw.Sync.ModulesBegin",
     "Draw Sync (Shared)/Modules Begin"},
    {TelemetryStageId::DrawSyncObjectIteration,
     "Draw.Sync.ObjectIteration",
     "Draw Sync (Shared)/Object Iteration"},
    {TelemetryStageId::DrawSyncDupliExtraction,
     "Draw.Sync.DupliExtraction",
     "Draw Sync (Shared)/Dupli Extraction"},
    {TelemetryStageId::DrawSyncDelayedExtraction,
     "Draw.Sync.DelayedExtraction",
     "Draw Sync (Shared)/Delayed Extraction"},
    {TelemetryStageId::DrawSyncExtractionWait,
     "Draw.Sync.ExtractionWait",
     "Draw Sync (Shared)/Extraction Wait"},
    {TelemetryStageId::DrawSyncCurvesUpdate,
     "Draw.Sync.CurvesUpdate",
     "Draw Sync (Shared)/Curves Update"},
    {TelemetryStageId::DrawSyncEngineEnd,
     "Draw.Sync.EngineEnd",
     "Draw Sync (Shared)/Engine End Sync"},
    {TelemetryStageId::DrawSyncManagerEnd,
     "Draw.Sync.ManagerEnd",
     "Draw Sync (Shared)/Manager End"},
    {TelemetryStageId::DrawSubmissionShared,
     "Draw.Submission.Shared",
     "Draw/Submission (Shared)"},
    {TelemetryStageId::DrawSubmissionFramebuffer,
     "Draw.Submission.Framebuffer",
     "Draw/Submission (Shared)/Framebuffer"},
    {TelemetryStageId::DrawSubmissionCallbacksPre,
     "Draw.Submission.CallbacksPre",
     "Draw/Submission (Shared)/Pre Callbacks"},
    {TelemetryStageId::DrawSubmissionEngineDraw,
     "Draw.Submission.EngineDraw",
     "Draw/Submission (Shared)/Engine Draw"},
    {TelemetryStageId::DrawSubmissionCallbacksPost,
     "Draw.Submission.CallbacksPost",
     "Draw/Submission (Shared)/Post Callbacks"},
    {TelemetryStageId::DrawSubmissionFramebufferRestore,
     "Draw.Submission.FramebufferRestore",
     "Draw/Submission (Shared)/Framebuffer Restore"},
}};

bool TelemetryModule::enabled() const
{
  if (inst_.scene == nullptr ||
      (inst_.scene->eevee.flag & SCE_EEVEE_PERFORMANCE_PROFILER) == 0)
  {
    return false;
  }
  /* Material/thumbnail previews use the same EEVEE render callback but are not the user's final
   * render. Do not let them clear or publish into the final-render performance registry. */
  return inst_.render == nullptr || (inst_.render->flag & RE_ENGINE_PREVIEW) == 0;
}

void TelemetryModule::source_deactivate()
{
  if (!source_state_ || source_state_->closed) {
    return;
  }

  frame_active_ = false;
  scope_stack_.clear();
  source_state_->closed = true;

  /* Profiler disable is a live transition, so the Instance's Scene pointer is still valid and a
   * closed snapshot can be published immediately. */
  if (inst_.scene == nullptr) {
    return;
  }
  const TelemetryFrameRecord *record = last_viewport_record();
  if (record != nullptr) {
    this->publish_viewport_snapshot(*record, viewport_summary_line(), viewport_report());
  }
  if (BLI_thread_is_main()) {
    Scene *notify_scene = DEG_get_original(inst_.scene);
    if (notify_scene == nullptr) {
      notify_scene = inst_.scene;
    }
    WM_main_add_notifier(NC_SPACE | ND_SPACE_OUTLINER | NA_EDITED, notify_scene);
  }
}

bool TelemetryModule::epoch_inputs_update(const bool profiler_enabled,
                                          const int extent_x,
                                          const int extent_y,
                                          const int output_offset_x,
                                          const int output_offset_y,
                                          const int output_extent_x,
                                          const int output_extent_y)
{
  if (!source_state_->profiler_inputs_initialized) {
    const bool reactivating = source_state_->closed && profiler_enabled;
    source_state_->profiler_inputs_initialized = true;
    source_state_->profiler_enabled = profiler_enabled;
    source_state_->extent_x = extent_x;
    source_state_->extent_y = extent_y;
    source_state_->output_offset_x = output_offset_x;
    source_state_->output_offset_y = output_offset_y;
    source_state_->output_extent_x = output_extent_x;
    source_state_->output_extent_y = output_extent_y;
    if (reactivating) {
      reset_epoch(true);
      source_state_->closed = false;
    }
    return reactivating;
  }
  const bool changed = source_state_->profiler_enabled != profiler_enabled ||
                       source_state_->extent_x != extent_x ||
                       source_state_->extent_y != extent_y ||
                       source_state_->output_offset_x != output_offset_x ||
                       source_state_->output_offset_y != output_offset_y ||
                       source_state_->output_extent_x != output_extent_x ||
                       source_state_->output_extent_y != output_extent_y;
  if (changed) {
    const bool profiler_disabled = source_state_->profiler_enabled && !profiler_enabled;
    const bool profiler_changed = source_state_->profiler_enabled != profiler_enabled;
    if (profiler_disabled) {
      this->source_deactivate();
    }
    source_state_->profiler_enabled = profiler_enabled;
    source_state_->extent_x = extent_x;
    source_state_->extent_y = extent_y;
    source_state_->output_offset_x = output_offset_x;
    source_state_->output_offset_y = output_offset_y;
    source_state_->output_extent_x = output_extent_x;
    source_state_->output_extent_y = output_extent_y;
    /* Toggling the profiler starts a fresh source session. Geometry changes start a fresh epoch
     * as well, because timing histories from different render borders are not comparable. */
    reset_epoch(profiler_changed);
    if (profiler_enabled) {
      source_state_->closed = false;
    }
  }
  return changed;
}

bool TelemetryModule::source_binding_update(const uint32_t scene_session_uid,
                                            const char *view_layer_name)
{
  const std::string layer_name = view_layer_name ? view_layer_name : "";
  const bool was_closed = source_state_->closed;
  /* The previous state identifies an instance teardown, while the current flag distinguishes it
   * from a source that is still closed because the profiler is disabled. */
  const bool reactivating = was_closed && source_state_->profiler_enabled && enabled();
  if (!source_state_->binding_initialized) {
    source_state_->binding_initialized = true;
    source_state_->scene_session_uid = scene_session_uid;
    source_state_->view_layer_name = layer_name;
    if (reactivating) {
      reset_epoch(true);
      source_state_->closed = false;
    }
    return reactivating;
  }
  const bool changed = source_state_->scene_session_uid != scene_session_uid ||
                       source_state_->view_layer_name != layer_name;
  if (changed || reactivating) {
    reset_epoch(true);
  }
  if (changed) {
    source_state_->scene_session_uid = scene_session_uid;
    source_state_->view_layer_name = layer_name;
  }
  if (reactivating) {
    source_state_->closed = false;
  }
  return changed || reactivating;
}

void TelemetryModule::render_view_name_set(const char *render_view_name)
{
  source_state_->render_view_name = render_view_name ? render_view_name : "";
}

void TelemetryModule::render_run_id_set(const uint64_t render_run_id)
{
  source_state_->render_run_id = render_run_id;
}

uint64_t TelemetryModule::source_id()
{
  return source_state_->source_id;
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

  profiler_accounting_start_time_ = 0.0;
  current_frame_ = {};
  scope_stack_.clear();
  TelemetryScopeNode root;
  root.stage = TelemetryStageId::Count;
  current_frame_.scope_nodes.append(std::move(root));
  inst_.light_probes.probe_costs_reset();
  current_frame_.runtime_mode = mode;
  current_frame_.source_id = source_id();
  current_frame_.epoch = source_state_->epoch;
  current_frame_.scene_session_uid = source_state_->scene_session_uid;
  current_frame_.render_run_id = source_state_->render_run_id;
  current_frame_.view_layer_name = source_state_->view_layer_name;
  current_frame_.render_view_name = source_state_->render_view_name;
  const int2 frame_extent = inst_.film.display_extent_get();
  current_frame_.resolution_x = frame_extent.x;
  current_frame_.resolution_y = frame_extent.y;
  current_frame_.is_playback = (mode == TelemetryRuntimeMode::Viewport) && inst_.is_playback;
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

  const bool measure_profiler_accounting = current_frame_.has_shared_draw_timing;
  const double profiler_accounting_start = measure_profiler_accounting ?
                                               (profiler_accounting_start_time_ > 0.0 ?
                                                    profiler_accounting_start_time_ :
                                                    BLI_time_now_seconds()) :
                                               0.0;

  current_frame_.sample_index = ELEM(current_frame_.runtime_mode,
                                     TelemetryRuntimeMode::Viewport,
                                     TelemetryRuntimeMode::ViewportImageRender,
                                     TelemetryRuntimeMode::Bake) ?
                                    int(inst_.sampling.viewport_sample_index()) :
                                    int(inst_.sampling.sample_index());
  current_frame_.sample_count = inst_.sampling.sample_count();
  const double frame_end_time = BLI_time_now_seconds();
  if (!current_frame_.has_shared_draw_timing) {
    current_frame_.total_cpu_ms = (frame_end_time - frame_start_time_) * 1000.0;
  }
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
                                     TelemetryRuntimeMode::ViewportImageRender);
  if (is_viewport_mode && !frame_has_stage_samples(current_frame_)) {
    frame_active_ = false;
    scope_stack_.clear();
    profiler_accounting_start_time_ = 0.0;
    return;
  }

  /* History averages only read scalar timings and the scope tree. Keep the potentially large
   * light/probe/material diagnostic vectors out of every retained history entry; otherwise the
   * profiler itself periodically copies and frees them while trying to explain a frame. */
  Vector<TelemetryShadowLightCost> shadow_light_costs =
      std::move(current_frame_.shadow_light_costs);
  Vector<TelemetryProbeCost> probe_costs = std::move(current_frame_.probe_costs);
  Vector<TelemetryMaterialHotspot> material_hotspots =
      std::move(current_frame_.material_hotspots);

  const int mode_index = int(current_frame_.runtime_mode);
  current_frame_.capture_seq = source_state_->capture_seq + 1;
  Vector<TelemetryFrameRecord> &history = source_state_->history[mode_index];
  history.append(current_frame_);
  if (history.size() > history_limit_) {
    history.remove(0);
  }
  const int average_count = min_ii(average_window(), history.size());
  if (average_count > 0) {
    double total_average = 0.0;
    std::array<double, stage_count> stage_average{};
    for (int offset = 0; offset < average_count; offset++) {
      const TelemetryFrameRecord &history_record =
          history[history.size() - 1 - offset];
      total_average += history_record.total_cpu_ms;
      for (int stage_index = 0; stage_index < stage_count; stage_index++) {
        stage_average[stage_index] += history_record.stages[stage_index].cpu_ms;
      }
    }
    current_frame_.average_total_cpu_ms = total_average / double(average_count);
    for (int stage_index = 0; stage_index < stage_count; stage_index++) {
      current_frame_.average_stage_values[stage_index] =
          stage_average[stage_index] / double(average_count);
    }
    Vector<TelemetryStageId> scope_path;
    for (const int child_index : current_frame_.scope_nodes[0].children) {
      scope_node_averages_update(
          current_frame_, history, average_count, child_index, scope_path);
    }
  }
  if (measure_profiler_accounting) {
    current_frame_.profiler_accounting_ms =
        (BLI_time_now_seconds() - profiler_accounting_start) * 1000.0;
  }
  current_frame_.shadow_light_costs = std::move(shadow_light_costs);
  current_frame_.probe_costs = std::move(probe_costs);
  current_frame_.material_hotspots = std::move(material_hotspots);
  source_state_->last_records[mode_index] = current_frame_;
  source_state_->has_last_record[mode_index] = true;
  source_state_->capture_seq++;

  if (inst_.scene != nullptr) {
    Scene *scene_orig = DEG_get_original(inst_.scene);
    Scene *notify_scene = (scene_orig != nullptr) ? scene_orig : inst_.scene;

    /* Publish every completed viewport sample. This keeps sample progress and short sync spikes
     * visible instead of merging several samples behind a wall-clock throttle. */
    if (is_viewport_mode) {
      maybe_publish_cached_viewport();
    }

    if (current_frame_.runtime_mode == TelemetryRuntimeMode::FinalRender) {
      const std::string render_summary = render_report();
      publish_render_snapshot(current_frame_, render_summary);
      if (BLI_thread_is_main()) {
        WM_main_add_notifier(NC_SPACE | ND_SPACE_OUTLINER | NA_EDITED, notify_scene);
      }
    }
  }

  frame_active_ = false;
  profiler_accounting_start_time_ = 0.0;
}

void TelemetryModule::maybe_begin_viewport_frame()
{
  if (!enabled() || frame_active_) {
    return;
  }
  if (inst_.draw_ctx == nullptr ||
      !ELEM(inst_.draw_ctx->mode, DRWContext::VIEWPORT, DRWContext::VIEWPORT_RENDER))
  {
    return;
  }
  if (ELEM(runtime_mode(), TelemetryRuntimeMode::Viewport, TelemetryRuntimeMode::ViewportImageRender))
  {
    frame_begin(runtime_mode());
  }
}

void TelemetryModule::maybe_end_viewport_frame()
{
  if (!enabled() || !frame_active_) {
    return;
  }
  if (inst_.draw_ctx == nullptr ||
      !ELEM(inst_.draw_ctx->mode, DRWContext::VIEWPORT, DRWContext::VIEWPORT_RENDER))
  {
    return;
  }
  if (ELEM(runtime_mode(), TelemetryRuntimeMode::Viewport, TelemetryRuntimeMode::ViewportImageRender))
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
  profiler_accounting_start_time_ = 0.0;
  scope_stack_.clear();
}

void TelemetryModule::maybe_publish_cached_viewport()
{
  if (!enabled() || inst_.scene == nullptr || viewport_publish_paused()) {
    return;
  }

  const TelemetryFrameRecord *record = last_viewport_record();
  if (record == nullptr) {
    return;
  }

  const std::string viewport_summary = viewport_summary_line();
  const std::string viewport_report = this->viewport_report();
  publish_viewport_snapshot(*record, viewport_summary, viewport_report);

  if (BLI_thread_is_main()) {
    /* Capture sequence and sample progress change even when rounded timing text does not. */
    Scene *notify_scene = DEG_get_original(inst_.scene);
    if (notify_scene == nullptr) {
      notify_scene = inst_.scene;
    }
    WM_main_add_notifier(NC_SPACE | ND_SPACE_OUTLINER | NA_EDITED, notify_scene);
  }
}

void TelemetryModule::reset()
{
  frame_active_ = false;
  profiler_accounting_start_time_ = 0.0;
  scope_stack_.clear();
}

void TelemetryModule::reset_epoch(const bool clear_source_session)
{
  this->reset();
  current_frame_ = {};
  for (Vector<TelemetryFrameRecord> &history : source_state_->history) {
    history.clear();
  }
  for (bool &has_last_record : source_state_->has_last_record) {
    has_last_record = false;
  }
  source_state_->last_records = {};
  source_state_->epoch++;
  if (clear_source_session) {
    source_state_->last_published_viewport_summary.clear();
    source_state_->last_published_viewport_report.clear();
    /* Expire snapshots associated with the previous Scene, ViewLayer, or source session. The next
     * publish receives a fresh token and is therefore not mistaken for the old source. */
    source_state_->lifetime = std::make_shared<int>(0);
  }
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

void TelemetryModule::merge_draw_performance(const blender::DrawPerformanceMetrics &metrics)
{
  if (!enabled() || !frame_active_ || !metrics.valid || current_frame_.has_shared_draw_timing ||
      current_frame_.scope_nodes.is_empty())
  {
    return;
  }

  current_frame_.has_shared_draw_timing = true;
  current_frame_.has_last_evaluation = metrics.has_last_evaluation;
  current_frame_.last_evaluation_ms = metrics.last_evaluation_ms;
  current_frame_.depsgraph_eval_serial = metrics.depsgraph_eval_serial;
  current_frame_.draw_sync_ms = metrics.sync_total_ms;
  current_frame_.draw_submission_ms = metrics.submission_total_ms;
  current_frame_.total_cpu_ms = metrics.sync_total_ms + metrics.submission_total_ms;

  auto add_flat_sample = [&](const TelemetryStageId stage, const double cpu_ms) {
    if (cpu_ms < 0.0) {
      return;
    }
    TelemetryStageSample &sample = current_frame_.stages[int(stage)];
    sample.cpu_ms += std::max(0.0, cpu_ms);
    sample.call_count += 1;
  };
  add_flat_sample(TelemetryStageId::DrawSyncShared, metrics.sync_total_ms);
  add_flat_sample(TelemetryStageId::DrawSyncEngineSetup, metrics.sync_engine_setup_ms);
  add_flat_sample(TelemetryStageId::DrawSyncEngineInit, metrics.sync_engine_init_ms);
  add_flat_sample(TelemetryStageId::DrawSyncManagerBegin, metrics.sync_manager_begin_ms);
  add_flat_sample(TelemetryStageId::DrawSyncEngineBegin, metrics.sync_engine_begin_ms);
  add_flat_sample(TelemetryStageId::DrawSyncModulesBegin, metrics.sync_modules_begin_ms);
  add_flat_sample(TelemetryStageId::DrawSyncObjectIteration, metrics.sync_object_iteration_ms);
  add_flat_sample(TelemetryStageId::DrawSyncDupliExtraction, metrics.sync_dupli_extraction_ms);
  add_flat_sample(TelemetryStageId::DrawSyncDelayedExtraction, metrics.sync_delayed_extraction_ms);
  add_flat_sample(TelemetryStageId::DrawSyncExtractionWait, metrics.sync_extraction_wait_ms);
  add_flat_sample(TelemetryStageId::DrawSyncCurvesUpdate, metrics.sync_curves_update_ms);
  add_flat_sample(TelemetryStageId::DrawSyncEngineEnd, metrics.sync_engine_end_ms);
  add_flat_sample(TelemetryStageId::DrawSyncManagerEnd, metrics.sync_manager_end_ms);
  add_flat_sample(TelemetryStageId::DrawSubmissionShared, metrics.submission_total_ms);
  add_flat_sample(TelemetryStageId::DrawSubmissionFramebuffer, metrics.submission_framebuffer_ms);
  add_flat_sample(TelemetryStageId::DrawSubmissionCallbacksPre,
                  metrics.submission_callbacks_pre_ms);
  add_flat_sample(TelemetryStageId::DrawSubmissionEngineDraw,
                  metrics.submission_engine_draw_ms);
  add_flat_sample(TelemetryStageId::DrawSubmissionCallbacksPost,
                  metrics.submission_callbacks_post_ms);
  add_flat_sample(TelemetryStageId::DrawSubmissionFramebufferRestore,
                  metrics.submission_framebuffer_restore_ms);

  const int sync_root = scope_node_find_or_add(
      current_frame_, 0, TelemetryStageId::DrawSyncShared);
  const int submission_root = scope_node_find_or_add(
      current_frame_, 0, TelemetryStageId::DrawSubmissionShared);
  current_frame_.scope_nodes[sync_root].cpu_ms += metrics.sync_total_ms;
  current_frame_.scope_nodes[sync_root].call_count += 1;
  current_frame_.scope_nodes[submission_root].cpu_ms += metrics.submission_total_ms;
  current_frame_.scope_nodes[submission_root].call_count += 1;

  auto add_scope_sample = [&](const int parent_index,
                              const TelemetryStageId stage,
                              const double cpu_ms) {
    if (cpu_ms >= 0.0) {
      scope_node_add_sample(current_frame_, parent_index, stage, std::max(0.0, cpu_ms));
    }
  };
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncEngineSetup, metrics.sync_engine_setup_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncEngineInit, metrics.sync_engine_init_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncManagerBegin, metrics.sync_manager_begin_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncEngineBegin, metrics.sync_engine_begin_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncModulesBegin, metrics.sync_modules_begin_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncObjectIteration, metrics.sync_object_iteration_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncDupliExtraction, metrics.sync_dupli_extraction_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncDelayedExtraction, metrics.sync_delayed_extraction_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncExtractionWait, metrics.sync_extraction_wait_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncCurvesUpdate, metrics.sync_curves_update_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncEngineEnd, metrics.sync_engine_end_ms);
  add_scope_sample(sync_root, TelemetryStageId::DrawSyncManagerEnd, metrics.sync_manager_end_ms);
  add_scope_sample(submission_root,
                   TelemetryStageId::DrawSubmissionFramebuffer,
                   metrics.submission_framebuffer_ms);
  add_scope_sample(submission_root,
                   TelemetryStageId::DrawSubmissionCallbacksPre,
                   metrics.submission_callbacks_pre_ms);
  add_scope_sample(submission_root,
                   TelemetryStageId::DrawSubmissionEngineDraw,
                   metrics.submission_engine_draw_ms);
  add_scope_sample(submission_root,
                   TelemetryStageId::DrawSubmissionCallbacksPost,
                   metrics.submission_callbacks_post_ms);
  add_scope_sample(submission_root,
                   TelemetryStageId::DrawSubmissionFramebufferRestore,
                   metrics.submission_framebuffer_restore_ms);

  const int sync_engine_begin = scope_node_find_or_add(
      current_frame_, sync_root, TelemetryStageId::DrawSyncEngineBegin);
  const int sync_object_iteration = scope_node_find_or_add(
      current_frame_, sync_root, TelemetryStageId::DrawSyncObjectIteration);
  const int sync_engine_end = scope_node_find_or_add(
      current_frame_, sync_root, TelemetryStageId::DrawSyncEngineEnd);
  scope_node_reparent_from_root(current_frame_, TelemetryStageId::SyncBegin, sync_engine_begin);
  scope_node_reparent_from_root(
      current_frame_, TelemetryStageId::SyncObjects, sync_object_iteration);
  scope_node_reparent_from_root(current_frame_, TelemetryStageId::SyncEnd, sync_engine_end);

  /* Engine draw owns the Eevee render scopes. Attach all scopes that were recorded directly below
   * the temporary tree root while retaining their original pipeline order. */
  const int submission_engine_draw = scope_node_find_or_add(
      current_frame_, submission_root, TelemetryStageId::DrawSubmissionEngineDraw);
  Vector<int> engine_draw_children;
  for (const int child_index : current_frame_.scope_nodes[0].children) {
    const TelemetryStageId stage = current_frame_.scope_nodes[child_index].stage;
    if (stage != TelemetryStageId::DrawSyncShared &&
        stage != TelemetryStageId::DrawSubmissionShared)
    {
      engine_draw_children.append(child_index);
    }
  }
  for (int index = current_frame_.scope_nodes[0].children.size() - 1; index >= 0; index--) {
    const int child_index = current_frame_.scope_nodes[0].children[index];
    const TelemetryStageId stage = current_frame_.scope_nodes[child_index].stage;
    if (stage == TelemetryStageId::DrawSyncShared ||
        stage == TelemetryStageId::DrawSubmissionShared)
    {
      continue;
    }
    current_frame_.scope_nodes[0].children.remove(index);
  }
  for (const int child_index : engine_draw_children) {
    current_frame_.scope_nodes[submission_engine_draw].children.append(child_index);
  }
}

void TelemetryModule::draw_performance_end(const blender::DrawPerformanceMetrics &metrics)
{
  if (!enabled() || !frame_active_ || !metrics.valid) {
    return;
  }
  profiler_accounting_start_time_ = BLI_time_now_seconds();
  this->merge_draw_performance(metrics);
  this->frame_end();
}

int TelemetryModule::scope_begin(const TelemetryStageId stage)
{
  if (!enabled() || !frame_active_ || current_frame_.scope_nodes.is_empty()) {
    return -1;
  }

  const int parent_index = scope_stack_.is_empty() ? 0 : scope_stack_.last();
  int node_index = -1;
  for (const int child_index : current_frame_.scope_nodes[parent_index].children) {
    if (current_frame_.scope_nodes[child_index].stage == stage) {
      node_index = child_index;
      break;
    }
  }
  if (node_index == -1) {
    TelemetryScopeNode node;
    node.stage = stage;
    node_index = current_frame_.scope_nodes.size();
    current_frame_.scope_nodes.append(std::move(node));
    current_frame_.scope_nodes[parent_index].children.append(node_index);
  }
  scope_stack_.append(node_index);
  return node_index;
}

void TelemetryModule::scope_end(const int node_index, const double elapsed_seconds)
{
  if (node_index < 0 || !frame_active_ || scope_stack_.is_empty()) {
    return;
  }

  if (scope_stack_.last() != node_index) {
    /* A scope must be closed in LIFO order. Do not corrupt the rest of the capture if an
     * exceptional path violates that contract. */
    scope_stack_.clear();
    return;
  }

  TelemetryScopeNode &node = current_frame_.scope_nodes[node_index];
  node.cpu_ms += elapsed_seconds * 1000.0;
  node.call_count += 1;
  scope_stack_.pop_last();
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
  const auto add_sample = [&](auto &sample) {
    sample.request_count += 1;
    sample.shader_queued_count += int64_t(shader_queued);
    sample.optimize_queued_count += int64_t(optimize_queued);
    sample.fallback_count += int64_t(fallback);
    sample.failed_count += int64_t(failed);
  };
  add_sample(current_frame_.material_sync);

  /* Name only requests with a concrete compilation/fallback problem. Successful material lookups
   * remain aggregate-only so normal object iteration does not pay for per-material string work. */
  if (!(shader_queued || optimize_queued || fallback || failed) || material_name == nullptr ||
      material_name[0] == '\0')
  {
    return;
  }
  for (TelemetryMaterialHotspot &hotspot : current_frame_.material_hotspots) {
    if (hotspot.name == material_name) {
      add_sample(hotspot);
      return;
    }
  }
  if (current_frame_.material_hotspots.size() >= material_hotspot_tracking_limit) {
    current_frame_.material_hotspot_untracked_count++;
    return;
  }
  TelemetryMaterialHotspot hotspot;
  hotspot.name = material_name;
  add_sample(hotspot);
  current_frame_.material_hotspots.append(std::move(hotspot));
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
  return source_state_->has_last_record[mode_index] ?
             &source_state_->last_records[mode_index] :
             nullptr;
}

const TelemetryFrameRecord *TelemetryModule::last_viewport_record() const
{
  const TelemetryFrameRecord *viewport = last_record(TelemetryRuntimeMode::Viewport);
  const TelemetryFrameRecord *image_render = last_record(
      TelemetryRuntimeMode::ViewportImageRender);
  if (viewport == nullptr) {
    return image_render;
  }
  if (image_render == nullptr) {
    return viewport;
  }
  return (image_render->capture_seq > viewport->capture_seq) ? image_render : viewport;
}

double TelemetryModule::averaged_total_cpu_ms(const TelemetryRuntimeMode mode) const
{
  const Vector<TelemetryFrameRecord> &history = source_state_->history[int(mode)];
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
  const Vector<TelemetryFrameRecord> &history = source_state_->history[int(mode)];
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

std::shared_ptr<const bke::SceneEeveePerformanceSnapshot> TelemetryModule::build_snapshot(
    const TelemetryFrameRecord &record,
    const char *kind,
    const std::string &summary,
    const std::string &report,
    const uint64_t capture_seq) const
{
  auto snapshot = std::make_shared<bke::SceneEeveePerformanceSnapshot>();
  const auto &averages = record.average_stage_values;
  snapshot->id = fmt::format("{}-source-{}-epoch-{}", kind, record.source_id, record.epoch);
  if (std::strcmp(kind, "final_render") == 0) {
    snapshot->id = fmt::format("{}-scene-{}-run-{}-layer-{}-view-{}",
                               kind,
                               record.scene_session_uid,
                               record.render_run_id,
                               record.view_layer_name,
                               record.render_view_name.empty() ? "default" :
                                                                  record.render_view_name);
  }
  snapshot->kind = kind;
  snapshot->mode = telemetry_mode_label(record.runtime_mode);
  snapshot->status = record.is_playback ? "Playback Active" : sample_status_string(record);
  snapshot->timing_domain = "CPU wall time";
  snapshot->timing_scope = snapshot->kind.rfind("viewport", 0) == 0 ?
                               "Shared 3D draw cycle (Draw Sync + Draw/Submission; excludes "
                               "depsgraph evaluation, viewport unbind, and post-draw UI overlay)" :
                               (snapshot->kind == "final_render" ?
                                    "EEVEE render samples + sync/submission/readback (excludes "
                                    "Instance init and shader setup)" :
                                    "EEVEE frame");
  snapshot->source_label = snapshot->kind.rfind("viewport", 0) == 0 ?
                               fmt::format("Viewport {} ({}x{}, Samples {})",
                                           record.source_id,
                                           record.resolution_x,
                                           record.resolution_y,
                                           sample_progress_string(record)) :
                               (snapshot->kind == "final_render" ?
                                    fmt::format("Final Render scene {} run {} layer {} view {}",
                                                record.scene_session_uid,
                                                record.render_run_id,
                                                record.view_layer_name.empty() ? "<None>" :
                                                                                  record.view_layer_name,
                                                record.render_view_name.empty() ? "<default>" :
                                                                                   record.render_view_name) :
                                    fmt::format("{} scene {}",
                                                snapshot->mode,
                                                record.scene_session_uid));
  snapshot->source_id = record.source_id;
  snapshot->capture_seq = capture_seq;
  snapshot->epoch = record.epoch;
  snapshot->is_playback = record.is_playback;
  snapshot->scene_session_uid = record.scene_session_uid;
  snapshot->render_run_id = record.render_run_id;
  snapshot->view_layer_name = record.view_layer_name;
  snapshot->render_view_name = record.render_view_name;
  snapshot->frame = record.frame;
  snapshot->sample_index = record.sample_index;
  snapshot->sample_count = record.sample_count;
  snapshot->resolution_x = record.resolution_x;
  snapshot->resolution_y = record.resolution_y;
  snapshot->has_last_evaluation = record.has_last_evaluation;
  snapshot->last_evaluation_ms = record.last_evaluation_ms;
  snapshot->depsgraph_eval_serial = record.depsgraph_eval_serial;
  snapshot->total_cpu_ms = record.total_cpu_ms;
  snapshot->draw_sync_ms = record.draw_sync_ms;
  snapshot->draw_submission_ms = record.draw_submission_ms;
  snapshot->profiler_accounting_ms = record.profiler_accounting_ms;
  snapshot->summary = summary;
  snapshot->report = report;
  snapshot->root.id = snapshot->kind + "/root";
  snapshot->root.kind = "root";
  snapshot->root.label = (record.runtime_mode == TelemetryRuntimeMode::FinalRender) ?
                            "Final Render" :
                            (record.runtime_mode == TelemetryRuntimeMode::Bake ? "Bake" :
                                                                                 "Viewport");
  snapshot->root.current_ms = record.total_cpu_ms;
  snapshot->root.average_ms = record.average_total_cpu_ms;
  snapshot->root.self_ms = record.total_cpu_ms;
  snapshot->root.calls = 1;
  snapshot->root.active = true;

  if (!record.scope_nodes.is_empty()) {
    double child_ms = 0.0;
    for (const int child_index : record.scope_nodes[0].children) {
      bke::SceneEeveePerformanceNode child = snapshot_scope_node(
          record, child_index, snapshot->root.id);
      child_ms += child.current_ms;
      snapshot->root.children.push_back(std::move(child));
    }
    snapshot->root.self_ms = std::max(0.0, record.total_cpu_ms - child_ms);
  }
  else {
    for (int stage_index = 0; stage_index < stage_count; stage_index++) {
      const TelemetryStageSample &stage = record.stages[stage_index];
      if (stage.call_count == 0) {
        continue;
      }
      bke::SceneEeveePerformanceNode child;
      child.id = snapshot->root.id + "/stage-" + std::to_string(stage_index);
      child.kind = "stage";
      child.label = stage_label(TelemetryStageId(stage_index));
      child.current_ms = stage.cpu_ms;
      child.average_ms = averages[stage_index];
      child.self_ms = stage.cpu_ms;
      child.calls = stage.call_count;
      child.active = true;
      snapshot->root.children.push_back(std::move(child));
    }
  }
  return snapshot;
}

void TelemetryModule::publish_viewport_snapshot(const TelemetryFrameRecord &record,
                                                const std::string &summary,
                                                const std::string &report)
{
  const uint64_t sequence = record.capture_seq;
  const auto latest = build_snapshot(record, "viewport_latest", summary, report, sequence);
  Scene *scene_publish = DEG_get_original(inst_.scene);
  if (scene_publish == nullptr) {
    scene_publish = inst_.scene;
  }
  bke::SceneEeveePerformanceRuntime *runtime = scene_eevee_performance_runtime(scene_publish);
  if (runtime == nullptr) {
    return;
  }

  bke::SceneEeveePerformanceViewportSource source;
  source.source_id = record.source_id;
  source.label = latest->source_label;
  source.lifetime = source_state_->lifetime;
  source.closed = source_state_->closed;
  source.latest = latest;
  runtime->viewport_source_publish(std::move(source), summary, report);
  source_state_->last_published_viewport_summary = summary;
  source_state_->last_published_viewport_report = report;
}

void TelemetryModule::publish_render_snapshot(const TelemetryFrameRecord &record,
                                              const std::string &report)
{
  const auto snapshot = build_snapshot(record, "final_render", "", report, record.capture_seq);
  Scene *scene_publish = DEG_get_original(inst_.scene);
  if (scene_publish == nullptr) {
    scene_publish = inst_.scene;
  }
  if (bke::SceneEeveePerformanceRuntime *runtime =
          scene_eevee_performance_runtime(scene_publish))
  {
    runtime->final_render_publish(snapshot, report);
  }
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
  const TelemetryFrameRecord *record = last_viewport_record();
  if (record == nullptr) {
    return "";
  }

  const double total_cpu_ms = averaged_total_cpu_ms(record->runtime_mode);
  const auto averages = averaged_stage_values(record->runtime_mode);
  const double sync_ms = record->has_shared_draw_timing ?
                             averages[int(TelemetryStageId::DrawSyncShared)] :
                             (averages[int(TelemetryStageId::SyncBegin)] +
                              averages[int(TelemetryStageId::SyncObjects)] +
                              averages[int(TelemetryStageId::SyncEnd)]);
  const double main_ms = averages[int(TelemetryStageId::MainView)];
  const double deferred_ms = averages[int(TelemetryStageId::MainDeferred)];
  const double dof_ms = averages[int(TelemetryStageId::PostDepthOfField)];
  const double filter_ms = averages[int(TelemetryStageId::MainFilterBeforeVolumeFog)] +
                           averages[int(TelemetryStageId::MainFilterBeforePostFX)] +
                           averages[int(TelemetryStageId::PostFilterBeforeDepthOfField)] +
                           averages[int(TelemetryStageId::PostFilterBeforeComposite)];

  return fmt::format("Perf Draw CPU {:.2f} ms | Avg {:.2f} | Prof {:.2f} | Eval {} | Sample {} ({}) | Avg Sync {:.2f} | Main {:.2f} | Deferred {:.2f} | DOF {:.2f} | Filter {:.2f}",
                     record->total_cpu_ms,
                     total_cpu_ms,
                     record->profiler_accounting_ms,
                     record->has_last_evaluation ? fmt::format("{:.2f} ms", record->last_evaluation_ms) :
                                                   std::string("n/a"),
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

  const double frame_share = (record.total_cpu_ms > 0.0) ?
                                 (sample.cpu_ms / record.total_cpu_ms) * 100.0 :
                                 0.0;
  return fmt::format(
      "  Shader Waits:\n"
      "    - waits={} cpu={:.3f} ms frame_share={:.1f}% queued_shaders={} queued_textures={}\n",
      sample.wait_count,
      sample.cpu_ms,
      frame_share,
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
  if (record.material_hotspot_untracked_count > 0) {
    result += fmt::format("    - ... {} additional flagged requests not named (tracking limit {})\n",
                          record.material_hotspot_untracked_count,
                          material_hotspot_tracking_limit);
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
  if (viewport_publish_paused()) {
    const std::string &text = include_stage_list ?
                                  source_state_->last_published_viewport_report :
                                  source_state_->last_published_viewport_summary;
    return split_overlay_text(text);
  }

  const TelemetryFrameRecord *record = last_viewport_record();
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
  const TelemetryFrameRecord *record = last_viewport_record();
  if (record == nullptr) {
    return "";
  }

  const auto averages = averaged_stage_values(record->runtime_mode);
  std::string result = fmt::format(
      "Viewport Draw CPU: {:.3f} ms\n"
      "Average Draw CPU: {:.3f} ms\n"
      "Profiler Accounting CPU: {:.3f} ms (excluded from Draw CPU; report formatting and snapshot publication not included)\n"
      "Timing Domain: CPU wall time\n"
      "Timing Scope: Shared 3D draw cycle (Draw Sync + Draw/Submission; excludes depsgraph evaluation, viewport unbind, and post-draw UI overlay)\n"
      "Accounting: Inclusive scopes (parent rows include child time; do not sum nested rows)\n"
      "Last Evaluation: {}\n"
      "Depsgraph Eval Serial: {}\n"
      "Frame: {}\n"
      "Source ID: {}\n"
      "Capture Sequence: {}\n"
      "Sample Progress: {}\n"
      "Sampling: {}\n"
      "Features: AO={} DOF={} MB={} Volume={} Raytrace={} Filters={} RenderTextures={} Lights={} Probes={} NPR Mats={} Raycast Mats={} GLSL Mats={}\n",
      record->total_cpu_ms,
      record->average_total_cpu_ms,
      record->profiler_accounting_ms,
      record->has_last_evaluation ? fmt::format("{:.3f} ms (not included in Draw CPU)",
                                                record->last_evaluation_ms) :
                                    std::string("n/a"),
      record->depsgraph_eval_serial,
      record->frame,
      record->source_id,
      record->capture_seq,
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
    if (stage.call_count == 0 && avg_ms == 0.0) {
      continue;
    }
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
  result += format_material_sync_report(*record);
  result += format_shader_waits_report(*record);
  result += format_pass_readbacks_report(*record);
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
      "Timing Domain: CPU wall time\n"
      "Timing Scope: EEVEE render samples + sync/submission/readback (excludes Instance init and shader setup)\n"
      "Accounting: Inclusive scopes (parent rows include child time; do not sum nested rows)\n"
      "  Render Run: {}\n"
      "  View Layer: {}\n"
      "  Render View: {}\n"
      "  Frame: {}\n"
      "  Sample Progress: {}\n"
      "  Sampling: {}\n"
      "  Sample Count: {}\n"
      "  Total CPU: {:.3f} ms\n"
      "  Features: AO={} DOF={} MB={} Volume={} Raytrace={} Filters={} RenderTextures={} Lights={} Probes={} NPR Mats={} Raycast Mats={} GLSL Mats={}\n",
      record->render_run_id,
      record->view_layer_name.empty() ? "<None>" : record->view_layer_name,
      record->render_view_name.empty() ? "<default>" : record->render_view_name,
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
  result += format_material_sync_report(*record);
  result += format_shader_waits_report(*record);
  result += format_pass_readbacks_report(*record);
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
    scope_node_index_ = telemetry_->scope_begin(stage_);
    start_time_ = BLI_time_now_seconds();
  }
}

ScopedTelemetrySample::~ScopedTelemetrySample()
{
  if (telemetry_ != nullptr) {
    const double elapsed_seconds = BLI_time_now_seconds() - start_time_;
    telemetry_->stage_add(stage_, elapsed_seconds);
    telemetry_->scope_end(scope_node_index_, elapsed_seconds);
  }
}

}  // namespace blender::eevee
