/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "BKE_sound_types.hh"

#include "BLI_mutex.hh"
#include "BLI_set.hh"
#include "BLI_utility_mixins.hh"

namespace blender {

struct Depsgraph;

namespace nodes::eval_log {
class NodesEvalLog;
}  // namespace nodes::eval_log

namespace bke {

/* Runtime data specific to the compositing trees. */
class CompositorRuntime {
 public:
  CompositorRuntime();

  /* A nodes log of the last compositor evaluation. */
  std::unique_ptr<nodes::eval_log::NodesEvalLog> nodes_evaluation_log;
  /* A dependency graph used for interactive compositing. This is initialized the first time it is
   * needed, and then kept persistent for the lifetime of the scene. This is done to allow the
   * compositor to track changes to resources its uses as well as reduce the overhead of creating
   * the dependency graph every time it executes. */
  Depsgraph *preview_depsgraph = nullptr;

  ~CompositorRuntime();
};

/* Runtime data specific to the sequencer, e.g. when using scene strips. */
class SequencerRuntime {
 public:
  Depsgraph *depsgraph = nullptr;

  ~SequencerRuntime();
};

/* Audio runtime data. */
struct SceneAudioRuntime {
  AUD_Sequence sound_scene;
  AUD_Handle playback_handle;
  AUD_Handle sound_scrub_handle;
  Set<AUD_SequenceEntry> speaker_handles;
};

/* Immutable, machine-readable Eevee performance data shared by the Outliner and RNA clients.
 * Render threads build a complete value and publish it as one shared_ptr; readers keep the pointer
 * alive for the whole tree/report build and never observe partially written children. */
struct SceneEeveePerformanceNode {
  std::string id;
  std::string kind;
  std::string label;
  double current_ms = 0.0;
  double average_ms = 0.0;
  double self_ms = 0.0;
  int calls = 0;
  bool active = false;
  std::vector<SceneEeveePerformanceNode> children;
};

struct SceneEeveePerformanceSnapshot {
  std::string schema = "eevee.performance.v2";
  std::string id;
  std::string kind;
  std::string mode;
  std::string status;
  std::string timing_domain = "CPU wall time";
  std::string timing_scope;
  std::string source_label;
  uint64_t source_id = 0;
  uint64_t capture_seq = 0;
  uint64_t epoch = 0;
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
  /* Monotonic depsgraph evaluation serial, not a count of updated IDs. */
  uint64_t depsgraph_eval_serial = 0;
  double total_cpu_ms = 0.0;
  double draw_sync_ms = 0.0;
  double draw_submission_ms = 0.0;
  /* CPU spent finalizing telemetry history/scope accounting after the measured draw boundary.
   * Report formatting, snapshot publication, viewport unbind and later UI drawing are excluded. */
  double profiler_accounting_ms = 0.0;
  SceneEeveePerformanceNode root;
  std::string summary;
  std::string report;
};

struct SceneEeveePerformanceViewportSource {
  uint64_t source_id = 0;
  std::string label;
  std::weak_ptr<const void> lifetime;
  bool closed = false;
  std::shared_ptr<const SceneEeveePerformanceSnapshot> latest;
};

struct SceneEeveePerformanceSnapshotSet {
  std::vector<SceneEeveePerformanceViewportSource> viewport_sources;
  /* All layer/view results from one complete RE_engine_render() invocation. */
  uint64_t final_render_run_id = 0;
  std::vector<std::shared_ptr<const SceneEeveePerformanceSnapshot>> final_renders;
  /* Latest result retained for legacy single-report RNA consumers. */
  std::shared_ptr<const SceneEeveePerformanceSnapshot> final_render;
  std::shared_ptr<const SceneEeveePerformanceSnapshot> color_bake;
  std::shared_ptr<const SceneEeveePerformanceSnapshot> light_probe_bake;
};

struct SceneEeveePerformanceRuntime {
 private:
  mutable Mutex snapshot_mutex;
  std::shared_ptr<const SceneEeveePerformanceSnapshotSet> snapshot_set;

 public:
  std::string viewport_summary;
  std::string viewport_report;
  std::string render_report;
  std::string dlss5_viewport_status;
  std::string dlss5_render_status;

  void dlss5_status_publish(bool viewport, std::string status)
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    (viewport ? dlss5_viewport_status : dlss5_render_status) = std::move(status);
  }

  std::string dlss5_status_get(bool viewport) const
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    return viewport ? dlss5_viewport_status : dlss5_render_status;
  }


  std::shared_ptr<const SceneEeveePerformanceSnapshotSet> snapshot_set_get() const
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    return snapshot_set;
  }

  void snapshot_set_publish(std::shared_ptr<const SceneEeveePerformanceSnapshotSet> value)
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    snapshot_set = std::move(value);
  }

  void viewport_snapshot_publish(std::shared_ptr<const SceneEeveePerformanceSnapshotSet> value,
                                 const std::string &summary,
                                 const std::string &report)
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    snapshot_set = std::move(value);
    viewport_summary = summary;
    viewport_report = report;
  }

  void render_snapshot_publish(std::shared_ptr<const SceneEeveePerformanceSnapshotSet> value,
                               const std::string &report)
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    snapshot_set = std::move(value);
    render_report = report;
  }

  void viewport_source_publish(SceneEeveePerformanceViewportSource source,
                               const std::string &summary,
                               const std::string &report)
  {
    static constexpr size_t closed_source_limit = 8;
    std::lock_guard<Mutex> lock(snapshot_mutex);
    auto next = snapshot_set ? std::make_shared<SceneEeveePerformanceSnapshotSet>(*snapshot_set) :
                               std::make_shared<SceneEeveePerformanceSnapshotSet>();
    for (auto it = next->viewport_sources.begin(); it != next->viewport_sources.end();) {
      if (it->lifetime.expired() && !it->closed && it->source_id != source.source_id) {
        /* A destroyed viewport has no safe Scene pointer left for a final publish. Keep its last
         * immutable snapshot, but make the lifecycle state explicit so the Outliner can archive it
         * as closed rather than presenting it as a live source. */
        it->closed = true;
      }
      ++it;
    }
    bool replaced = false;
    for (SceneEeveePerformanceViewportSource &existing : next->viewport_sources) {
      if (existing.source_id != source.source_id) {
        continue;
      }
      if (existing.latest && source.latest &&
          (existing.latest->epoch > source.latest->epoch ||
           (existing.latest->epoch == source.latest->epoch &&
            existing.latest->capture_seq > source.latest->capture_seq)))
      {
        return;
      }
      existing = std::move(source);
      replaced = true;
      break;
    }
    if (!replaced) {
      next->viewport_sources.push_back(std::move(source));
    }

    size_t closed_count = 0;
    for (const SceneEeveePerformanceViewportSource &entry : next->viewport_sources) {
      closed_count += size_t(entry.closed);
    }
    while (closed_count > closed_source_limit) {
      auto it = std::find_if(next->viewport_sources.begin(),
                             next->viewport_sources.end(),
                             [&](const SceneEeveePerformanceViewportSource &entry) {
                               return entry.closed && entry.source_id != source.source_id;
                             });
      if (it == next->viewport_sources.end()) {
        break;
      }
      next->viewport_sources.erase(it);
      closed_count--;
    }
    snapshot_set = std::move(next);
    viewport_summary = summary;
    viewport_report = report;
  }

  void final_render_publish(std::shared_ptr<const SceneEeveePerformanceSnapshot> snapshot,
                            const std::string &report)
  {
    if (!snapshot) {
      return;
    }
    std::lock_guard<Mutex> lock(snapshot_mutex);
    auto next = snapshot_set ? std::make_shared<SceneEeveePerformanceSnapshotSet>(*snapshot_set) :
                               std::make_shared<SceneEeveePerformanceSnapshotSet>();
    if (snapshot->render_run_id < next->final_render_run_id) {
      return;
    }
    if (snapshot->render_run_id != next->final_render_run_id) {
      next->final_render_run_id = snapshot->render_run_id;
      next->final_renders.clear();
      next->final_render.reset();
    }
    const auto same_source = [&](const auto &existing) {
      return existing && existing->render_run_id == snapshot->render_run_id &&
             existing->scene_session_uid == snapshot->scene_session_uid &&
             existing->view_layer_name == snapshot->view_layer_name &&
             existing->render_view_name == snapshot->render_view_name;
    };
    const auto existing = std::find_if(
        next->final_renders.begin(), next->final_renders.end(), same_source);
    if (existing != next->final_renders.end()) {
      *existing = snapshot;
    }
    else {
      next->final_renders.push_back(snapshot);
    }
    next->final_render = std::move(snapshot);
    snapshot_set = std::move(next);
    render_report = report;
  }

  void final_render_session_begin(const uint64_t render_run_id)
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    auto next = snapshot_set ? std::make_shared<SceneEeveePerformanceSnapshotSet>(*snapshot_set) :
                               std::make_shared<SceneEeveePerformanceSnapshotSet>();
    if (render_run_id <= next->final_render_run_id) {
      return;
    }
    next->final_render_run_id = render_run_id;
    next->final_renders.clear();
    next->final_render.reset();
    snapshot_set = std::move(next);
    render_report.clear();
  }

  void viewport_strings_publish(const std::string &summary, const std::string &report)
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    viewport_summary = summary;
    viewport_report = report;
  }

  void render_string_publish(const std::string &report)
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    render_report = report;
  }

  std::string viewport_summary_get() const
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    return viewport_summary;
  }

  std::string viewport_report_get() const
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    return viewport_report;
  }

  std::string render_report_get() const
  {
    std::lock_guard<Mutex> lock(snapshot_mutex);
    return render_report;
  }
};

class SceneRuntime : NonCopyable, NonMovable {
 public:
  CompositorRuntime compositor;
  SequencerRuntime sequencer;
  SceneAudioRuntime audio;
  SceneEeveePerformanceRuntime eevee_performance;
};

}  // namespace bke
}  // namespace blender
