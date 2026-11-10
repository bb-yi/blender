/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_geometry_lines.hh"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <optional>
#include <thread>

#include "DNA_image_types.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "BLI_math_matrix.hh"
#include "BLI_hash.h"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"
#include "GPU_framebuffer.hh"
#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"
#include "DRW_gpu_wrapper.hh"
#include "draw_pass.hh"
#include "draw_view.hh"
#include "DRW_render.hh"
#include "NOD_npr_line.hh"
#include "eevee_geometry_lines_graph.hh"
#include "eevee_geometry_lines_mesh.hh"
#include "eevee_geometry_lines_shared.hh"
#include "eevee_instance.hh"

namespace blender::eevee {
namespace glines = geometry_lines;
namespace {

double elapsed_ms(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

uint64_t hash_bytes(uint64_t hash, const void *data, size_t length)
{
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (size_t i = 0; i < length; i++) { hash = (hash ^ bytes[i]) * 1099511628211ULL; }
  return hash;
}
template<class T> uint64_t hash_value(uint64_t hash, const T &value)
{
  return hash_bytes(hash, &value, sizeof(value));
}

size_t buffer_capacity(size_t count)
{
  size_t capacity = 1;
  while (capacity < count) { capacity *= 2; }
  return capacity;
}

template<class T>
void upload_buffer(gpu::StorageBuf *&buffer, size_t &capacity, std::vector<T> &data, const char *name)
{
  const size_t count = std::max<size_t>(1, data.size());
  const size_t desired = buffer_capacity(count);
  if (!buffer || capacity < desired || capacity > desired * 4) {
    if (buffer) { GPU_storagebuf_free(buffer); }
    capacity = desired;
    buffer = GPU_storagebuf_create_ex(capacity * sizeof(T), nullptr, GPU_USAGE_DYNAMIC, name);
  }
  data.resize(capacity);
  GPU_storagebuf_usage_size_set(buffer, count * sizeof(T));
  GPU_storagebuf_update(buffer, data.data());
}

GeometryLineBrushData pack_brush(const glines::Brush &brush)
{
  GeometryLineBrushData data{};
  data.field_program = int4(0, 0, -1, -1);
  data.field_outputs = int4(-1, -1, 0, 0);
  if (brush.fields) {
    const auto &fields = *brush.fields;
    data.field_program = int4(0, fields.instructions.size(), fields.position, fields.color);
    data.field_outputs = int4(fields.width, fields.opacity, fields.registers,
                             fields.project_parameters ? 2 : 1);
  }
  data.color = brush.color;
  data.tip = float4(brush.width, brush.opacity, brush.spacing, brush.stretch);
  data.variation = float4(brush.angle, brush.softness, brush.width_random, brush.angle_random);
  data.taper = float4(brush.taper_start, brush.taper_end, brush.alpha_start, brush.alpha_end);
  data.dash = float4(brush.dash_length, brush.dash_space, brush.dash_phase, brush.dashed ? 1 : 0);
  data.deformation = float4(brush.extend, brush.offset, brush.wave, brush.cycles);
  data.offset_noise = float4(brush.shift_x, brush.shift_y, brush.noise, brush.grain);
  data.options = int4(brush.mode, brush.copies, brush.seed,
                      (brush.image ? 1 : 0) | (brush.procedural_tip() ? 2 : 0));
  data.curve_options = int4(brush.width_curve_enabled, brush.opacity_curve_enabled, brush.raster_size_response, brush.stroke_mode);
  data.maps = float4(float(brush.width_map), brush.width_map_strength, float(brush.color_map), brush.color_map_strength);
  data.detail_maps = float4(float(brush.brush_map), brush.brush_map_opacity,
                            float(brush.distortion_map), brush.distortion_map_amount);
  data.wave_settings = float4(brush.wave_length, brush.wave_phase, brush.wave_random, brush.wave_period_random);
  data.wave_options = float4(brush.wave_phase_random, brush.wave_pixels, brush.offset_random, 0);
  return data;
}

struct DrawRange {
  int brush;
  int rule;
  int start;
  int count;
};

struct ExpandedChunk {
  gpu::StorageBuf *instances = nullptr;
  gpu::StorageBuf *parts = nullptr;
  std::vector<DrawRange> ranges;
  void free()
  {
    if (instances) { GPU_storagebuf_free(instances); }
    if (parts) { GPU_storagebuf_free(parts); }
    instances = parts = nullptr;
  }
};

struct BrushTip {
  draw::Texture texture = {"GeometryLines.Tip"};
  uint64_t signature = 0;
};

uint32_t tip_random_next(uint32_t state)
{
  return (2061u * state + 7u) & 65535u;
}

const std::array<uint32_t, 65536> &tip_random_table()
{
  static const auto table = []() {
    std::array<uint32_t, 65536> values;
    uint32_t state = 0;
    for (auto &value : values) { value = state = tip_random_next(state); }
    return values;
  }();
  return table;
}

float tip_spatial_noise(float2 a, float2 b, float cell_size, uint32_t state, uint32_t suffix)
{
  const auto &table = tip_random_table();
  const float inverse = 1.0f / std::max(cell_size, 0.001f);
  float value = -1;
  for (int footprint = 0; footprint < 2; footprint++) {
    const float2 p = (footprint == 0 ? a : b) * inverse;
    const int ix = int(p.x) - int(p.x < 0), iy = int(p.y) - int(p.y < 0);
    const float tx = p.x - float(ix), ty = p.y - float(iy);
    const float weights[] = {(1 - tx) * (1 - ty), (1 - ty) * tx, (1 - tx) * ty, ty * tx};
    for (int i = 0; i < 4; i++) {
      const int x = ix + (i & 1), y = iy + (i >> 1) + 32768;
      const int hx = x >> 16, hy = y >> 16;
      const uint32_t rx = table[uint32_t(x) & 65535u], ry = table[uint32_t(y) & 65535u];
      const uint32_t index = (ry + 1) * rx + ry + std::abs(hx) * (hx > 0 ? 255 : 13) +
                             std::abs(hy) * (hy > 0 ? 19 : 127) + footprint * 16384;
      uint32_t random = ((index + state + suffix) ^ suffix) & 65535u;
      for (int j = 0; j < 4; j++) { random = tip_random_next(random); }
      value += float(random) * (1.0f / 65536.0f) * weights[i];
    }
  }
  value = value <= 0 ? (value + 1) * (value + 1) - 1 : 1 - (1 - value) * (1 - value);
  return value;
}

float tip_direction_noise(float2 point, float cell_size, uint32_t suffix)
{
  const float2 shift(cell_size * 0.25f);
  return (tip_spatial_noise(point - shift, point + shift, cell_size, 0, suffix) + 1) * 0.5f;
}

struct StrokeTipParameters {
  uint32_t layer = 0;
  float rotation = 0;
};

StrokeTipParameters stroke_tip_parameters(const glines::Stroke &stroke, const glines::Brush &brush,
                                          int group, int copy, int height)
{
  uint32_t state = uint32_t(brush.seed) + uint32_t(group) * 1023u;
  /* One generator step per copy, followed by the spatial generator's nine warm-up steps. */
  for (int i = 0; i < copy + 10; i++) { state = tip_random_next(state); }
  const double3 &first = stroke.parts.front().segment.a;
  const double3 &last = stroke.parts.back().segment.b;
  float2 a(float(first.x), float(height - first.y));
  float2 b(float(last.x), float(height - last.y));
  if (stroke.closed) {
    b = a + float2(2);
    a -= float2(2);
  }
  const float unit = (tip_spatial_noise(a, b, 8.0f, state, 0x226d0u) + 1) * 0.5f;
  StrokeTipParameters parameters;
  if (brush.procedural_tip()) { parameters.layer = uint32_t(std::clamp(int(unit * 10), 0, 9)); }
  if (brush.angle_random != 0) {
    parameters.rotation = tip_spatial_noise(a, b, 8.0f, state, 0xc0eceu) *
                           (brush.angle_random * 0.5f);
  }
  return parameters;
}

/** Cached point coverage uses the measured 16-bit sequence and byte quantization. */
std::vector<float> make_tip(const glines::Brush &brush, int resolution, uint32_t phase = 0,
                          int border = 0)
{
  const int size = resolution + 2 * border;
  std::vector<float> pixels(size_t(size) * size);
  const float texel = 2.0f / resolution;
  const auto &table = tip_random_table();
  float rotation = 0;
  if (phase > 0 && brush.angle_random != 0) {
    const uint32_t state = tip_random_next(table[(phase + 0x309u) & 65535u]);
    rotation = ((float(state) / 32768.0f - 1) * brush.angle_random * 0.5f) * 0.017453292519943295f;
  }
  const float cosine = std::cos(rotation), sine = std::sin(rotation);
  float2 shift(0);
  if (phase > 0) {
    const uint32_t ry = tip_random_next(table[(phase + 0x24d83u) & 65535u]);
    const uint32_t rx = tip_random_next(ry);
    shift = float2((float(rx) / 32768.0f - 1) * 256,
                    (float(ry) / 32768.0f - 1) * 256);
  }
  const float radius = std::max(brush.dot_size, 0.1f) * 0.02f;
  const float step_x = 2.0f * (radius + brush.dot_space_x);
  const float step_y = 2.0f * (radius + brush.dot_space_y);
  float inner = 0.9f * (1.0f - brush.dot_antialiasing);
  if (inner < 0) { inner /= 1.0f - inner * 0.5f; }
  const float core = std::copysign(inner * inner, inner);
  const float outer = 1.0f + brush.dot_antialiasing * 0.75f;
  const float support = (brush.dot_size_random + 1) * radius * outer;
  const float end = std::clamp(brush.tip_reduction_end, 0.0f, 1.0f);
  const float start = std::clamp(brush.tip_reduction_start, 0.0f, 1.0f);
  const float start2 = start * start, end2 = end * end;
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      const float2 raw((x - border + 0.5f) * texel - 1.0f,
                       (y - border + 0.5f) * texel - 1.0f);
      const float2 p(cosine * raw.x - sine * raw.y, sine * raw.x + cosine * raw.y);
      float distance2 = p.x * p.x + p.y * p.y;
      float2 dot_point = p;
      if (brush.groove > 0) {
        const float length = std::sqrt(distance2);
        const float2 direction = length > 0 ? p / length : float2(1, 0);
        const float theta = std::atan2(p.x, p.y) + 3.1415927410125732f;
        const float angle = brush.groove_count *
                            (tip_direction_noise(direction, 0.125f, 0) * 0.25f + theta) *
                            0.15915493667125702f;
        const float wave = std::abs(2 * (angle - std::trunc(angle)) - 1);
        const float amount = (tip_direction_noise(direction, 0.05f, 100) * 0.5f + 1.25f) *
                             wave * wave * wave * brush.groove;
        distance2 *= 1 + amount * 1.25f;
        dot_point *= 1 - amount * 0.25f;
      }
      const float fade = start2 == end2 ? float(distance2 <= end2) :
                         std::clamp((end2 - distance2) / (end2 - start2), 0.0f, 1.0f);
      float alpha = distance2 < 1 ?
                        0.5f - (fade + 0.5f) * (fade - 0.5f) * (fade - 1.5f) * 1.3333333730697632f : 0;
      if (brush.softness > 0) {
        alpha *= std::clamp((1.0f - std::sqrt(distance2)) / brush.softness + 0.5f, 0.0f, 1.0f);
      }
      if (alpha > 0 && brush.mode == 2) {
        dot_point += shift;
        float dots = 0;
        const int start_x = int(((dot_point.x - step_x) - support) * (1 / step_x) + 0.5f);
        const int end_x = int((dot_point.x + step_x + support) * (1 / step_x) + 0.5f);
        const int start_y = int(((dot_point.y - step_y) - support) * (1 / step_y) + 0.5f);
        const int end_y = int((dot_point.y + step_y + support) * (1 / step_y) + 0.5f);
        for (int ix = start_x; ix < end_x && dots < 1.0f; ix++) {
          for (int iy = start_y; iy < end_y && dots < 1.0f; iy++) {
            uint32_t state = (table[(phase + 0x3d8545u + uint32_t(ix ^ iy)) & 65535u] +
                               table[(uint32_t(ix) + 0xb40u) & 65535u] +
                               table[(uint32_t(iy) + 0x438u) & 65535u]) & 65535u;
            state = tip_random_next(state);
            const float cx = (float(ix) + (float(state) / 65536.0f - 0.5f) * brush.dot_space_random_x) * step_x;
            state = tip_random_next(state);
            const float cy = (float(iy) + (float(state) / 65536.0f - 0.5f) * brush.dot_space_random_y) * step_y;
            state = tip_random_next(state);
            const float variation = (float(state) / 32768.0f - 1) * brush.dot_size_random;
            const float r = radius * (outer + variation);
            const float d2 = (dot_point.x - cx) * (dot_point.x - cx) + (dot_point.y - cy) * (dot_point.y - cy);
            if (r <= 0 || d2 >= r * r) { continue; }
            const float q = d2 / (r * r), b = core * (1 + variation);
            float coverage = 1;
            if (q > b) {
              const float t = std::sqrt((q - b) / (1 - b));
              coverage = 0.5f + 1.3333333730697632f * (t + 0.5f) * (t - 0.5f) * (t - 1.5f);
            }
            dots += coverage;
          }
        }
        alpha *= std::min(dots, 1.0f);
      }
      pixels[size_t(y) * size + x] = std::floor(std::clamp(alpha, 0.0f, 1.0f) * 255 + 0.5f) / 255;
    }
  }
  return pixels;
}

struct LineJob {
  std::shared_ptr<const glines::Geometry> geometry;
  glines::Camera camera;
  glines::Plan plan;
  std::vector<glines::Rule> rules;
  uint64_t signature = 0;
  uint64_t visibility_signature = 0;
};

/** Each evaluator owns its cache. No evaluated Blender ID or GPU object reaches a worker. */
struct LineEvaluator {
  std::vector<glines::IntersectionEdge> intersections;
  uint64_t intersection_revision = 0;
  uint64_t visibility_signature = 0;
  glines::Result visibility;

  std::shared_ptr<const glines::Result> compute(const LineJob &job, const std::atomic<bool> &cancel)
  {
    glines::Statistics stats;
    bool need_intersections = false;
    for (const auto &rule : job.rules) {
      need_intersections |= ((rule.visible | (rule.hidden_enabled ? rule.hidden : 0)) & glines::Intersection) != 0;
    }
    if (need_intersections && intersection_revision != job.geometry->intersection_revision) {
      auto next = glines::find_intersections(*job.geometry, stats, cancel);
      if (cancel.load()) { return {}; }
      intersections = std::move(next);
      intersection_revision = job.geometry->intersection_revision;
    }
    if (!need_intersections) { intersections.clear(); intersection_revision = 0; }
    const bool visibility_cache_hit = visibility_signature == job.visibility_signature;
    if (!visibility_cache_hit) {
      auto next = glines::evaluate(*job.geometry, job.camera, job.plan, job.rules, intersections, cancel, false);
      if (next.cancelled || cancel.load()) { return {}; }
      visibility = std::move(next);
      visibility_signature = job.visibility_signature;
    }
    auto result = std::make_shared<glines::Result>(visibility);
    if (visibility_cache_hit) {
      result->statistics.visibility_ms = 0;
      result->statistics.triangle_tests = 0;
      result->statistics.mask_tests = 0;
    }
    result->strokes.clear();
    for (auto &segment : result->segments) {
      const auto &rule = job.rules[segment.rule];
      segment.brush = segment.hidden ? rule.hidden_brush[segment.type] : rule.visible_brush[segment.type];
    }
    glines::connect_strokes(*result, job.rules, cancel);
    if (cancel.load()) { return {}; }
    result->statistics.intersection_ms = stats.intersection_ms;
    result->statistics.intersection_tests = stats.intersection_tests;
    return result;
  }
};

/** At most one running and one latest pending snapshot; obsolete results are never published. */
class LineWorker {
  std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<LineJob> pending_;
  std::shared_ptr<const glines::Result> completed_;
  uint64_t requested_ = 0, completed_signature_ = 0;
  bool stopping_ = false;
  std::atomic<bool> cancel_{false};
  std::thread thread_;

 public:
  ~LineWorker()
  {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      cancel_.store(true);
    }
    wake_.notify_one();
    if (thread_.joinable()) { thread_.join(); }
  }
  void cancel()
  {
    std::lock_guard lock(mutex_);
    requested_ = 0;
    pending_.reset();
    completed_.reset();
    completed_signature_ = 0;
    cancel_.store(true);
  }
  void request(LineJob job)
  {
    std::lock_guard lock(mutex_);
    if (requested_ == job.signature) { return; }
    requested_ = job.signature;
    pending_ = std::move(job);
    completed_.reset();
    cancel_.store(true);
    if (!thread_.joinable()) {
      thread_ = std::thread([this]() {
        LineEvaluator evaluator;
        for (;;) {
          LineJob job;
          {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&]() { return stopping_ || pending_.has_value(); });
            if (stopping_) { return; }
            job = std::move(*pending_);
            pending_.reset();
            cancel_.store(false);
          }
          auto result = evaluator.compute(job, cancel_);
          std::lock_guard lock(mutex_);
          if (result && !cancel_.load() && requested_ == job.signature) {
            completed_ = std::move(result);
            completed_signature_ = job.signature;
          }
        }
      });
    }
    wake_.notify_one();
  }
  std::shared_ptr<const glines::Result> take(uint64_t signature)
  {
    std::lock_guard lock(mutex_);
    return completed_signature_ == signature ? completed_ : nullptr;
  }
};

}  // namespace

struct GeometryLineModule::Impl {
  Instance &inst;
  bool enabled = false;
  glines::Graph graph;
  std::optional<glines::Graph> compiled_graph;
  const bNodeTree *compiled_tree = nullptr;
  uint64_t compiled_update = ~uint64_t(0);
  int compiled_solo = 0;
  glines::GeometryBuilder builder;
  std::shared_ptr<const glines::Geometry> geometry;
  std::shared_ptr<const glines::Geometry> reference_geometry;
  std::optional<glines::Graph> reference_graph;
  std::optional<CameraData> reference_camera;
  /** Only needed when the beauty line is blurred: preserve one clear canonical raw pass. */
  std::unique_ptr<Impl> raw_reference;
  std::shared_ptr<const glines::Result> result;
  LineEvaluator evaluator;
  LineWorker worker;
  uint64_t result_signature = 0;
  uint64_t paint_signature = 0;
  uint64_t expansion_signature = 0;
  std::vector<ExpandedChunk> expanded_chunks;
  bool expansion_cached = false;
  uint64_t requested_signature = 0;
  std::optional<LineJob> current_job;
  std::chrono::steady_clock::time_point changed_at;
  bool quick = false;
  bool timing_enabled = false;
  bool timing_quick = false;
  uint64_t timing_result_signature = 0;
  glines::Statistics timing_statistics;
  double prepare_ms = 0, measured_prepare_ms = 0, drawing_ms = 0;
  uint64_t drawing_instances = 0;
  draw::View *render_view = nullptr;
  uint64_t last_depsgraph_update = 0;
  uint64_t last_geometry_revision = 0;
  uint64_t image_revision = 0;
  struct ImageUpdate {
    blender::Image *image;
    uint32_t session_uid;
    PartialUpdateUser *user;
  };
  std::vector<ImageUpdate> image_updates;
  uint64_t accumulated_samples = 0;
  uint64_t last_accumulated_sample = 0;
  uint64_t last_accumulated_paint_signature = 0;
  bool raw_accumulation_static = true;
  double4x4 previous_projection = double4x4::identity();
  bool has_previous_projection = false;
  std::atomic<bool> cancelled{false};
  int2 extent = int2(1);

  draw::Texture color_tx = {"GeometryLines.Color"};
  draw::Texture depth_tx = {"GeometryLines.Depth"};
  draw::Texture velocity_tx = {"GeometryLines.Velocity"};
  draw::Texture composite_tx = {"GeometryLines.Composite"};
  draw::Texture processed_tx = {"GeometryLines.CameraEffects"};
  draw::Texture raw_accum_tx = {"GeometryLines.RawAccumulation"};
  draw::Texture processed_accum_tx = {"GeometryLines.ProcessedAccumulation"};
  draw::Texture white_tx = {"GeometryLines.White"};
  draw::Texture dummy_tip_tx = {"GeometryLines.DummyTip"};
  draw::Texture transparent_tx = {"GeometryLines.Transparent"};
  draw::Texture preview_depth_tx = {"GeometryLines.PreviewDepth"};
  draw::Texture coverage_mask_tx = {"GeometryLines.Coverage"};
  draw::Texture coverage_1_tx = {"GeometryLines.Coverage1"};
  draw::Texture coverage_2_tx = {"GeometryLines.Coverage2"};
  draw::Texture coverage_3_tx = {"GeometryLines.Coverage3"};
  draw::Texture mapped_color_tx = {"GeometryLines.MappedColor"};
  std::vector<std::unique_ptr<BrushTip>> brush_tips;
  draw::Framebuffer color_fb = {"GeometryLines.Color.FB"};
  draw::Framebuffer guides_fb = {"GeometryLines.Guides.FB"};
  draw::Framebuffer coverage_mask_fb = {"GeometryLines.Coverage.FB"};
  draw::Framebuffer mapped_color_fb = {"GeometryLines.MappedColor.FB"};
  gpu::StorageBuf *parts_buffer = nullptr;
  gpu::StorageBuf *brushes_buffer = nullptr;
  gpu::StorageBuf *curves_buffer = nullptr;
  gpu::StorageBuf *reductions_buffer = nullptr;
  gpu::StorageBuf *maps_buffer = nullptr;
  gpu::StorageBuf *instances_buffer = nullptr;
  gpu::StorageBuf *fields_buffer = nullptr, *ramps_buffer = nullptr, *original_buffer = nullptr;
  size_t parts_capacity = 0, brushes_capacity = 0, curves_capacity = 0, instances_capacity = 0;
  size_t reductions_capacity = 0;
  size_t maps_capacity = 0;
  size_t fields_capacity = 0, ramps_capacity = 0, original_capacity = 0;
  float4x4 world_from_clip = float4x4::identity();
  double3 reduction_camera_position = double3(0);
  /* Borrowed from the shared EEVEE shader cache, including across local previews. */
  gpu::Shader *expand_shader = nullptr;
  gpu::Shader *draw_shader = nullptr;
  gpu::Shader *composite_shader = nullptr;
  gpu::Shader *present_shader = nullptr;
  gpu::Shader *accumulate_shader = nullptr;
  gpu::Shader *mask_resolve_shader = nullptr;
  gpu::Shader *draw_samples_shader = nullptr;
  gpu::Shader *mask_samples_resolve_shader = nullptr;

  explicit Impl(Instance &instance) : inst(instance) {}
  ~Impl()
  {
    for (auto &image : image_updates) { BKE_image_partial_update_free(image.user); }
    for (auto &chunk : expanded_chunks) { chunk.free(); }
    for (auto *buffer : {parts_buffer, brushes_buffer, curves_buffer, reductions_buffer, maps_buffer, instances_buffer,
                        fields_buffer, ramps_buffer, original_buffer}) {
      if (buffer) { GPU_storagebuf_free(buffer); }
    }
  }

  bool refresh_images()
  {
    using namespace bke::image::partial_update;
    std::vector<blender::Image *> referenced;
    auto add = [&](blender::Image *image) {
      if (image && std::find(referenced.begin(), referenced.end(), image) == referenced.end()) {
        referenced.push_back(image);
      }
    };
    for (const auto &map : graph.maps) { add(map.image); }
    for (const auto &brush : graph.brushes) {
      add(brush.image);
      if (brush.fields) { for (blender::Image *image : brush.fields->images) { add(image); } }
    }
    for (const auto &rule : graph.occlusion) { if (rule.mode == 2) { add(rule.image); } }
    for (auto it = image_updates.begin(); it != image_updates.end();) {
      const auto found = std::find(referenced.begin(), referenced.end(), it->image);
      if (found == referenced.end() || (*found)->id.session_uid != it->session_uid) {
        BKE_image_partial_update_free(it->user);
        it = image_updates.erase(it);
      }
      else { ++it; }
    }
    bool changed = false;
    for (blender::Image *image : referenced) {
      auto it = std::find_if(image_updates.begin(), image_updates.end(),
                             [&](const ImageUpdate &item) { return item.image == image; });
      if (it == image_updates.end()) {
        image_updates.push_back({image, image->id.session_uid, BKE_image_partial_update_create(image)});
        it = image_updates.end() - 1;
      }
      changed |= BKE_image_partial_update_collect_changes(image, it->user) !=
                 ePartialUpdateCollectResult::NoChangesDetected;
    }
    return changed;
  }

  gpu::Texture *tip_texture(const glines::Brush &brush, int index)
  {
    if (!brush.procedural_tip()) { return dummy_tip_tx; }
    if (!brush_tips[index]) { brush_tips[index] = std::make_unique<BrushTip>(); }
    BrushTip &tip = *brush_tips[index];
    uint64_t signature = hash_value(1469598103934665603ULL, brush.mode);
    const float values[] = {brush.dot_size, brush.dot_size_random, brush.dot_antialiasing,
                            brush.dot_space_x, brush.dot_space_y, brush.dot_space_random_x,
                            brush.dot_space_random_y, brush.tip_reduction_start, brush.tip_reduction_end,
                            brush.groove, brush.softness, brush.angle_random};
    signature = hash_bytes(signature, values, sizeof(values));
    signature = hash_value(signature, brush.groove_count);
    signature = hash_value(signature, brush.seed);
    if (!tip.texture.is_valid() || tip.signature != signature) {
      constexpr int resolution = 64, size = resolution + 2, variants = 10;
      std::vector<float> pixels;
      pixels.reserve(size * size * variants);
      uint32_t state = uint32_t(brush.seed) + uint32_t(index) * 4095u + 127u;
      std::vector<float> mask;
      for (int i = 0; i < variants; i++) {
        state = tip_random_next(state);
        if (i == 0 || brush.mode == 2 || brush.angle_random != 0) {
          mask = make_tip(brush, resolution, state, 1);
        }
        pixels.insert(pixels.end(), mask.begin(), mask.end());
      }
      tip.texture.ensure_2d_array(gpu::TextureFormat::SFLOAT_16, int2(size), variants,
                                  GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT, nullptr, 7);
      GPU_texture_update(tip.texture, GPU_DATA_FLOAT, pixels.data());
      GPU_texture_update_mipmap_chain(tip.texture);
      GPU_texture_mipmap_mode(tip.texture, true, true);
      tip.signature = signature;
    }
    return tip.texture;
  }

  bool ensure_shaders()
  {
    if (!expand_shader) { expand_shader = inst.shaders.static_shader_get(GEOMETRY_LINES_EXPAND); }
    if (!draw_shader) { draw_shader = inst.shaders.static_shader_get(GEOMETRY_LINES_DRAW); }
    if (!composite_shader) { composite_shader = inst.shaders.static_shader_get(GEOMETRY_LINES_COMPOSITE); }
    if (!present_shader) { present_shader = inst.shaders.static_shader_get(GEOMETRY_LINES_PRESENT); }
    if (!mask_resolve_shader) { mask_resolve_shader = inst.shaders.static_shader_get(GEOMETRY_LINES_MASK_RESOLVE); }
    if (!expand_shader || !draw_shader || !composite_shader || !present_shader || !mask_resolve_shader) {
      inst.info_append("Geometry Lines: GPU shader compilation failed");
      return false;
    }
    if (!white_tx.is_valid()) {
      const float white[4] = {1, 1, 1, 1};
      white_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), GPU_TEXTURE_USAGE_SHADER_READ, white);
    }
    if (!dummy_tip_tx.is_valid()) {
      const float white = 1;
      dummy_tip_tx.ensure_2d_array(gpu::TextureFormat::SFLOAT_16, int2(1), 1,
                                   GPU_TEXTURE_USAGE_SHADER_READ, &white);
    }
    if (!transparent_tx.is_valid()) {
      const float clear[4] = {0, 0, 0, 0};
      transparent_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), GPU_TEXTURE_USAGE_SHADER_READ, clear);
    }
    return true;
  }

  void begin_sync()
  {
    const auto timing_start = std::chrono::steady_clock::now();
    timing_enabled = inst.scene->eevee.geometry_line_tree &&
                     !inst.is_dlss_guide_render &&
                     nodes::npr_line_timing_enabled(*inst.scene->eevee.geometry_line_tree);
    prepare_ms = 0;
    const bool previous_enabled = enabled;
    const uint64_t previous_connection = graph.connection_signature;
    const int previous_preview = graph.preview;
    enabled = inst.scene->eevee.use_geometry_lines && inst.scene->eevee.geometry_line_tree &&
              !inst.view_layer->eevee.geometry_lines_disabled &&
              !inst.is_dlss_guide_render;
    if (enabled) {
      const bNodeTree *tree = inst.scene->eevee.geometry_line_tree;
      const uint64_t update = DEG_get_update_count(inst.depsgraph);
      const int solo = inst.is_viewport() ? nodes::npr_line_solo_node(*tree) : 0;
      if (reference_graph) { graph = *reference_graph; }
      else {
        if (!compiled_graph || compiled_tree != tree || compiled_update != update || compiled_solo != solo) {
          compiled_graph = glines::compile_graph(*tree, solo);
          if (std::getenv("BLENDER_NPR_LINE_STATS")) {
            std::printf("NPR_BRUSH_FIELDS event=compile\n"); std::fflush(stdout);
          }
          compiled_tree = tree;
          compiled_update = update;
          compiled_solo = solo;
        }
        graph = *compiled_graph;
      }
      if (!graph.error.empty()) { inst.info_append(graph.error.c_str()); }
      for (const auto &brush : graph.brushes) {
        if (!brush.fields) { continue; }
        for (const auto &source : brush.fields->sources) {
          if (source.type != 2) { continue; }
          bool registered = false;
          const auto &aovs = inst.film.aovs_info;
          const uint32_t hash = BLI_hash_string(source.attribute.name.c_str());
          const int first = source.aov_type ? aovs.color_len : 0;
          const int last = source.aov_type ? aovs.color_len + aovs.value_len : aovs.color_len;
          for (int i = first; i < last; i++) { registered |= aovs.hash[i / 4][i % 4] == hash; }
          if (!registered) { inst.info_append("Brush Material AOV: name/type is not registered; using fallback"); }
        }
      }
      for (const auto &map : graph.maps) {
        if (map.image && ELEM(map.image->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE)) {
          graph.paint_signature = hash_value(graph.paint_signature, inst.scene->r.cfra);
        }
      }
      for (const auto &item : graph.reductions) {
        if (item.use_object && item.object) {
          const Object *object = DEG_get_evaluated(inst.depsgraph, item.object);
          if (object) { graph.paint_signature = hash_value(graph.paint_signature, object->object_to_world().location()); }
        }
      }
      enabled = !graph.rules.empty();
    }
    if (!enabled) {
      if (timing_enabled) { nodes::npr_line_set_timing(*inst.scene->eevee.geometry_line_tree, IFACE_("Geometry Lines inactive")); }
      if (previous_enabled) {
        color_tx.free();
        depth_tx.free();
        velocity_tx.free();
        composite_tx.free();
        processed_tx.free();
        preview_depth_tx.free();
        coverage_mask_fb.ensure();
        coverage_mask_tx.free();
        coverage_1_tx.free();
        coverage_2_tx.free();
        coverage_3_tx.free();
        if (mapped_color_tx.is_valid()) { mapped_color_fb.ensure(); mapped_color_tx.free(); }
        reset_accumulation();
        for (auto &chunk : expanded_chunks) { chunk.free(); }
        expanded_chunks.clear();
        expansion_cached = false;
        expansion_signature = 0;
        inst.native_postfx_outputs.release_geometry_lines_history();
        builder.clear();
        geometry.reset();
        result.reset();
        evaluator = {};
        worker.cancel();
        current_job.reset();
        result_signature = paint_signature = requested_signature = 0;
      }
      return;
    }
    if (previous_preview != graph.preview) { worker.cancel(); }
    if (previous_connection != graph.connection_signature || previous_enabled != enabled ||
        previous_preview != graph.preview) {
      inst.sampling.reset();
    }
    const uint64_t update = DEG_get_update_count(inst.depsgraph);
    const bool images_changed = refresh_images() ||
        (update != last_depsgraph_update && DEG_id_type_updated(inst.depsgraph, ID_IM));
    last_depsgraph_update = update;
    if (images_changed) { paint_signature = 0; image_revision++; }
    bool masks_changed = false;
    if (images_changed && !graph.occlusion.empty()) {
      DEGIDIterData data{};
      data.graph = inst.depsgraph;
      data.only_updated = true;
      ITER_BEGIN (DEG_iterator_ids_begin, DEG_iterator_ids_next, DEG_iterator_ids_end, &data, ID *, id) {
        if (GS(id->name) == ID_IM) {
          const ID *original = DEG_get_original_id(id);
          for (const auto &rule : graph.occlusion) {
            masks_changed |= rule.mode == 2 && rule.image && original == &rule.image->id;
          }
        }
      }
      ITER_END;
    }
    if (reference_geometry) { return; }
    builder.begin(graph, masks_changed,
                  !inst.is_viewport() && graph.motion_blur && inst.motion_blur.postfx_enabled() ?
                      &inst.velocity : nullptr);
    if (timing_enabled) { prepare_ms += elapsed_ms(timing_start); }
  }

  void end_sync()
  {
    const auto timing_start = std::chrono::steady_clock::now();
    if (!enabled) { return; }
    if (reference_geometry) {
      geometry = reference_geometry;
      graph.paint_signature = hash_value(hash_value(graph.paint_signature, geometry->attributes_revision), image_revision);
      return;
    }
    auto next_geometry = builder.finish();
    if (!geometry || geometry->revision != next_geometry->revision ||
        geometry->attributes_revision != next_geometry->attributes_revision) { inst.sampling.reset(); }
    geometry = std::move(next_geometry);
    graph.paint_signature = hash_value(hash_value(graph.paint_signature, geometry->attributes_revision), image_revision);
    if (!builder.error().empty()) { inst.info_append(builder.error().c_str()); }
    if (timing_enabled) { prepare_ms += elapsed_ms(timing_start); }
  }

  void capture_reference_geometry()
  {
    if (inst.is_viewport() || inst.is_baking() ||
        inst.is_dlss_guide_render || !(inst.scene->r.mode & R_MBLUR) ||
        !(inst.view_layer->layflag & SCE_LAY_MOTION_BLUR) ||
        !inst.scene->eevee.use_geometry_lines || !inst.scene->eevee.geometry_line_tree ||
        inst.view_layer->eevee.geometry_lines_disabled) {
      return;
    }
    auto frame_graph = glines::compile_graph(*inst.scene->eevee.geometry_line_tree);
    if (frame_graph.rules.empty()) { return; }
    builder.begin(frame_graph, DEG_id_type_updated(inst.depsgraph, ID_IM));
    DRW_render_object_iter(inst.render, inst.depsgraph,
        [&](draw::ObjectRef &object, RenderEngine *, Depsgraph *) {
          const int visibility = DRW_object_visibility_in_active_context(object.object);
          if (DRW_object_is_renderable(object.object) && (visibility & OB_VISIBLE_SELF)) {
            builder.add(object);
          }
        });
    auto frame_geometry = builder.finish();
    if (!builder.error().empty()) { inst.info_append(builder.error().c_str()); }
    inst.camera.sync();
    if (frame_graph.motion_blur) {
      raw_reference = std::make_unique<Impl>(inst);
      raw_reference->enabled = true;
      raw_reference->graph = std::move(frame_graph);
      raw_reference->graph.depth_of_field = raw_reference->graph.motion_blur = false;
      raw_reference->reference_geometry = frame_geometry;
      raw_reference->geometry = std::move(frame_geometry);
      raw_reference->reference_camera = inst.camera.data_get();
    }
    else {
      reference_geometry = std::move(frame_geometry);
      reference_graph = std::move(frame_graph);
      reference_camera = inst.camera.data_get();
    }
  }

  void paint(draw::View &view)
  {
    drawing_instances = 0;
    processed_tx.free();
    if (!ensure_shaders()) { return; }
    constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT |
                                      GPU_TEXTURE_USAGE_SHADER_WRITE;
    color_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, extent, usage);
    color_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_tx));
    GPU_framebuffer_bind(color_fb);
    GPU_framebuffer_clear_color(color_fb, double4(0));
    const bool guides = graph.depth_of_field || graph.motion_blur;
    if (guides) {
      depth_tx.ensure_2d(gpu::TextureFormat::SFLOAT_32, extent, usage);
      velocity_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, extent, usage);
      guides_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_NONE,
                        GPU_ATTACHMENT_TEXTURE(depth_tx), GPU_ATTACHMENT_TEXTURE(velocity_tx));
      GPU_framebuffer_bind(guides_fb);
      GPU_framebuffer_clear_color(guides_fb, double4(0));
    }
    else {
      depth_tx.free();
      velocity_tx.free();
    }
    if (!result || result->strokes.empty() || graph.brushes.empty() || graph.width_scale <= 0) {
      GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
      return;
    }
    const float scale = graph.width_scale * (graph.relative_width ? extent.x / std::max(graph.reference_width, 1.0f) : 1.0f);
    const int samples = quick ? 1 : std::clamp(graph.samples, 1, 4);
    const bool has_sample_masks = samples > 1 && std::any_of(
        result->strokes.begin(), result->strokes.end(), [&](const auto &stroke) {
          const auto &brush = graph.brushes[stroke.brush];
          return brush.mode != 0 && brush.raster_size_response && brush.color_map < 0;
        });
    const int mask_count = has_sample_masks ? (samples * samples + 3) / 4 : 1;
    if (has_sample_masks) {
      if (!draw_samples_shader) {
        draw_samples_shader = inst.shaders.static_shader_get(GEOMETRY_LINES_DRAW_SAMPLES);
      }
      if (!mask_samples_resolve_shader) {
        mask_samples_resolve_shader = inst.shaders.static_shader_get(GEOMETRY_LINES_MASK_RESOLVE_SAMPLES);
      }
      if (!draw_samples_shader || !mask_samples_resolve_shader) {
        inst.info_append("Geometry Lines: multisample coverage shader compilation failed");
        return;
      }
    }
    const auto mask_format = has_sample_masks ? gpu::TextureFormat::SFLOAT_16_16_16_16 :
                                               gpu::TextureFormat::SFLOAT_16;
    draw::Texture *extra_masks[] = {std::addressof(coverage_1_tx),
                                   std::addressof(coverage_2_tx),
                                   std::addressof(coverage_3_tx)};
    for (int i = 0; i < 3; i++) {
      if (i + 1 < mask_count) {
        extra_masks[i]->ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, extent, usage);
      }
      else if (extra_masks[i]->is_valid()) {
        coverage_mask_fb.ensure();
        extra_masks[i]->free();
      }
    }
    std::vector<GeometryLineBrushData> brush_data;
    std::vector<GeometryLineFieldInstruction> field_data;
    std::vector<float4> ramp_data;
    brush_tips.resize(graph.brushes.size());
    brush_data.reserve(graph.brushes.size());
    for (const auto &brush : graph.brushes) {
      auto packed = pack_brush(brush);
      packed.field_program.x = int(field_data.size());
      if (brush.fields) {
        for (auto instruction : brush.fields->instructions) {
          if (ELEM(instruction.op.x, LINE_FIELD_RAMP, LINE_FIELD_CURVE)) {
            instruction.meta.y += int(ramp_data.size());
          }
          field_data.push_back(instruction);
        }
        ramp_data.insert(ramp_data.end(), brush.fields->ramps.begin(), brush.fields->ramps.end());
      }
      brush_data.push_back(packed);
    }
    upload_buffer(fields_buffer, fields_capacity, field_data, "GeometryLines.Fields");
    upload_buffer(ramps_buffer, ramps_capacity, ramp_data, "GeometryLines.FieldRamps");
    upload_buffer(brushes_buffer, brushes_capacity, brush_data, "GeometryLines.Brushes");
    std::vector<GeometryLineCurveData> curve_data(graph.brushes.size());
    for (int i = 0; i < int(graph.brushes.size()); i++) {
      std::copy(graph.brushes[i].curve.begin(), graph.brushes[i].curve.end(), curve_data[i].values);
    }
    upload_buffer(curves_buffer, curves_capacity, curve_data, "GeometryLines.Curves");
    std::vector<GeometryLineReductionData> reduction_data;
    reduction_data.reserve(graph.reductions.size());
    for (const auto &item : graph.reductions) {
      GeometryLineReductionData data{};
      double3 position = reduction_camera_position;
      bool active = true;
      if (item.use_object) {
        const Object *object = item.object ? DEG_get_evaluated(inst.depsgraph, item.object) : nullptr;
        active = object != nullptr;
        if (object) { position = double3(object->object_to_world().location()); }
      }
      data.origin = float4(float3(position), item.start);
      const float span = item.end - item.start;
      data.domain = float4(std::abs(span) > 1e-8f ? 1 / span : 0, item.end, active, 0);
      std::copy(item.curve.begin(), item.curve.end(), data.values);
      reduction_data.push_back(data);
    }
    /* Include the evaluated pose as well as the curve. No Blender object reaches a worker. */
    uint64_t reduction_key = 1469598103934665603ULL;
    for (const auto &item : reduction_data) { reduction_key = hash_value(reduction_key, item); }
    upload_buffer(reductions_buffer, reductions_capacity, reduction_data, "GeometryLines.Reductions");
    std::vector<GeometryLineMapData> map_data;
    for (const auto &map : graph.maps) {
      GeometryLineMapData data;
      data.transform = float4(map.tiling.x, map.tiling.y, map.offset.x, map.offset.y);
      const float angle = map.rotation * (float(M_PI) / 180);
      data.rotation = float4(std::cos(angle), std::sin(angle), float(map.coordinates), 0);
      data.sampling = int4(map.wrap_u, map.wrap_v, map.filter, 0);
      map_data.push_back(data);
    }
    uint64_t map_key = geometry->attributes_revision;
    for (const auto &brush : graph.brushes) {
      for (const int index : {brush.width_map, brush.distortion_map}) {
        if (index >= 0) {
          map_key = hash_value(map_key, map_data[index]);
          map_key = hash_value(map_key, image_revision);
          const auto &map = graph.maps[index];
          map_key = hash_value(map_key, map.image);
          map_key = hash_value(map_key, map.coordinates);
          map_key = hash_value(map_key, map.uv.index);
          map_key = hash_bytes(map_key, map.uv.name.data(), map.uv.name.size());
          if (map.image && ELEM(map.image->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE)) {
            map_key = hash_value(map_key, inst.scene->r.cfra);
          }
        }
      }
      if (brush.color_map >= 0) {
        const auto &map = graph.maps[brush.color_map];
        map_key = hash_value(map_key, map.coordinates);
        map_key = hash_value(map_key, map.uv.index);
        map_key = hash_bytes(map_key, map.uv.name.data(), map.uv.name.size());
      }
      if (brush.brush_map >= 0 && graph.maps[brush.brush_map].coordinates == 2) {
        const auto &map = graph.maps[brush.brush_map];
        map_key = hash_value(map_key, map.coordinates);
        map_key = hash_value(map_key, map.uv.index);
        map_key = hash_bytes(map_key, map.uv.name.data(), map.uv.name.size());
      }
    }
    upload_buffer(maps_buffer, maps_capacity, map_data, "GeometryLines.Maps");
    auto coordinates = [&](const glines::AttributePoint &point, const glines::TextureMap &map) {
      float4 result(0, 0, point.inverse_w, 0);
      if (map.coordinates == 1) { result.w = 1; return result; }
      if (map.coordinates == 2) {
        float3 value(0);
        bool found_value = false;
        if (point.triangle >= 0) {
          const auto found = geometry->style_colors.find(map.uv);
          if (found != geometry->style_colors.end() && size_t(point.triangle) * 3 + 2 < found->second.size()) {
            const auto *colors = found->second.data() + size_t(point.triangle) * 3;
            value = float3(colors[0] + (colors[1] - colors[0]) * point.barycentric.x +
                               (colors[2] - colors[0]) * point.barycentric.y);
            found_value = true;
          }
        }
        else if (point.triangle < -1) {
          const int64_t edge_index = -int64_t(point.triangle) - 2;
          const auto found = geometry->point_colors.find(map.uv);
          if (size_t(edge_index) < geometry->edges.size() && found != geometry->point_colors.end()) {
            const int2 vertices = geometry->edges[edge_index].vertices;
            if (size_t(std::max(vertices.x, vertices.y)) < found->second.size()) {
              value = float3(found->second[vertices.x] + (found->second[vertices.y] - found->second[vertices.x]) * point.barycentric.x);
              found_value = true;
            }
          }
        }
        return found_value && std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) ?
                   float4(value, point.inverse_w) : float4(0);
      }
      const auto found = geometry->style_uvs.find(map.uv);
      if (point.triangle < 0 || found == geometry->style_uvs.end() ||
          size_t(point.triangle) * 3 + 2 >= found->second.size()) { return result; }
      const auto *uv = found->second.data() + size_t(point.triangle) * 3;
      const float2 value = uv[0] + (uv[1] - uv[0]) * point.barycentric.x + (uv[2] - uv[0]) * point.barycentric.y;
      if (std::isfinite(value.x) && std::isfinite(value.y)) { result = float4(value.x, value.y, point.inverse_w, 1); }
      return result;
    };
    bool missing_width_uv = false, missing_color_uv = false, missing_distortion_uv = false, missing_attribute = false;
    auto dynamic_color = [](const glines::Brush &brush) {
      return brush.color_map >= 0 || (brush.fields && brush.fields->color >= 0);
    };
    auto bind_fields = [&](draw::PassSimple &pass, const glines::Brush &brush) {
      pass.bind_ssbo("field_instructions", fields_buffer);
      pass.bind_ssbo("field_ramps", ramps_buffer);
      pass.bind_ssbo("original_parts", original_buffer);
      ImageUser image_user{};
      BKE_imageuser_default(&image_user);
      image_user.framenr = inst.scene->r.cfra;
      const char *names[] = {"field_image_0_tx", "field_image_1_tx", "field_image_2_tx", "field_image_3_tx"};
      for (int i = 0; i < LINE_FIELD_MAX_IMAGES; i++) {
        blender::Image *image = brush.fields && i < int(brush.fields->images.size()) ? brush.fields->images[i] : nullptr;
        gpu::Texture *texture = image ? BKE_image_get_gpu_texture(image, &image_user) : nullptr;
        pass.bind_texture(names[i], texture ? texture : transparent_tx.gpu_texture(),
                          GPUSamplerState(GPU_SAMPLER_FILTERING_LINEAR));
      }
      pass.bind_texture("field_aov_color_tx", inst.render_buffers.rp_color_tx);
      pass.bind_texture("field_aov_value_tx", inst.render_buffers.rp_value_tx);
      pass.bind_texture("field_depth_tx", inst.render_buffers.depth_tx);
    };
    if (std::none_of(graph.brushes.begin(), graph.brushes.end(), dynamic_color)) {
      if (mapped_color_tx.is_valid()) { mapped_color_fb.ensure(); mapped_color_tx.free(); }
    }
    int coverage_brush = -1, coverage_rule = -1, coverage_sample_count = 1;
    auto resolve_coverage = [&]() {
      if (coverage_brush < 0) { return; }
      GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
      draw::PassSimple resolve("GeometryLines.CoverageResolve");
      resolve.init();
      resolve.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA_PREMUL);
      const bool packed = coverage_sample_count > 1;
      resolve.shader_set(packed ? mask_samples_resolve_shader : mask_resolve_shader);
      resolve.framebuffer_set(&color_fb);
      resolve.bind_texture("mask_tx", coverage_mask_tx);
      if (packed) {
        resolve.bind_texture("mask_1_tx", coverage_1_tx.is_valid() ? coverage_1_tx.gpu_texture() : transparent_tx.gpu_texture());
        resolve.bind_texture("mask_2_tx", coverage_2_tx.is_valid() ? coverage_2_tx.gpu_texture() : transparent_tx.gpu_texture());
        resolve.bind_texture("mask_3_tx", coverage_3_tx.is_valid() ? coverage_3_tx.gpu_texture() : transparent_tx.gpu_texture());
        resolve.push_constant("sample_count", coverage_sample_count);
      }
      const bool mapped = dynamic_color(graph.brushes[coverage_brush]);
      resolve.bind_texture("mapped_color_tx", mapped ? mapped_color_tx.gpu_texture() : white_tx.gpu_texture());
      resolve.push_constant("mapped_color", mapped);
      resolve.push_constant("brush_color", graph.brushes[coverage_brush].color);
      resolve.draw_procedural(GPU_PRIM_TRIS, 1, 3);
      inst.manager->submit(resolve, view);
      coverage_brush = coverage_rule = -1;
    };
    auto draw_ranges = [&](gpu::StorageBuf *instances, gpu::StorageBuf *source_parts, const std::vector<DrawRange> &ranges) {
      for (const DrawRange &range : ranges) {
        drawing_instances += range.count;
        const auto &brush = graph.brushes[range.brush];
        const bool mapped = dynamic_color(brush);
        const bool packed = samples > 1 && brush.mode != 0 && brush.raster_size_response && !mapped;
        if (coverage_brush != range.brush || coverage_rule != range.rule) {
          resolve_coverage();
          coverage_mask_tx.ensure_2d(mask_format, extent, usage);
          coverage_mask_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(coverage_mask_tx),
              packed && mask_count > 1 ? GPU_ATTACHMENT_TEXTURE(coverage_1_tx) : GPU_ATTACHMENT_NONE,
              packed && mask_count > 2 ? GPU_ATTACHMENT_TEXTURE(coverage_2_tx) : GPU_ATTACHMENT_NONE,
              packed && mask_count > 3 ? GPU_ATTACHMENT_TEXTURE(coverage_3_tx) : GPU_ATTACHMENT_NONE);
          GPU_framebuffer_bind(coverage_mask_fb);
          GPU_framebuffer_clear_color(coverage_mask_fb, double4(0));
          coverage_brush = range.brush;
          coverage_rule = range.rule;
          coverage_sample_count = packed ? samples * samples : 1;
          if (mapped) {
            mapped_color_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, extent, usage);
            if (guides) {
              mapped_color_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(mapped_color_tx),
                                     GPU_ATTACHMENT_TEXTURE(depth_tx), GPU_ATTACHMENT_TEXTURE(velocity_tx));
            }
            else { mapped_color_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(mapped_color_tx)); }
            GPU_framebuffer_bind(mapped_color_fb);
            /* Guides are shared by all styles; clear only the new style's color. */
            const float clear[4] = {0, 0, 0, 0};
            GPU_texture_clear(mapped_color_tx, GPU_DATA_FLOAT, clear);
          }
        }
        ImageUser image_user = {};
        image_user.framenr = 1;
        gpu::Texture *brush_texture = brush.image ? BKE_image_get_gpu_texture(brush.image, &image_user) : nullptr;
        if (!brush_texture) { brush_texture = white_tx; }
        gpu::Texture *tip = tip_texture(brush, range.brush);
        ImageUser color_user{};
        BKE_imageuser_default(&color_user);color_user.framenr=inst.scene->r.cfra;
        gpu::Texture *color_texture = brush.color_map >= 0 && graph.maps[brush.color_map].image ?
            BKE_image_get_gpu_texture(graph.maps[brush.color_map].image, &color_user) : nullptr;
        if (!color_texture) { color_texture = transparent_tx; }
        gpu::Texture *brush_map_texture = brush.brush_map >= 0 && graph.maps[brush.brush_map].image ?
            BKE_image_get_gpu_texture(graph.maps[brush.brush_map].image, &color_user) : nullptr;
        if (!brush_map_texture) { brush_map_texture = white_tx; }
        for (int guide_pass = 0; guide_pass <= int(guides || mapped); guide_pass++) {
          if (guide_pass) { GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH); }
          draw::PassSimple draw("GeometryLines.Brush");
          draw.init();
          draw.state_set(guide_pass ? DRW_STATE_WRITE_COLOR : DRW_STATE_WRITE_COLOR |
                         DRW_STATE_BLEND_MAX);
          draw.shader_set(!guide_pass && packed ? draw_samples_shader : draw_shader);
          draw.framebuffer_set(guide_pass ? (mapped ? &mapped_color_fb : &guides_fb) : &coverage_mask_fb);
          draw.bind_ssbo("instances", instances);
          draw.bind_ssbo("brushes", brushes_buffer);
          draw.bind_ssbo("brush_curves", curves_buffer);
          draw.bind_ssbo("line_maps", maps_buffer);
          draw.bind_ssbo("source_parts", source_parts);
          bind_fields(draw, brush);
          draw.bind_texture("brush_tx", brush_texture);
          draw.bind_texture("tip_tx", tip);
          draw.bind_texture("color_map_tx", color_texture);
          draw.bind_texture("brush_map_tx", brush_map_texture);
          draw.bind_texture("coverage_tx", guide_pass && mapped ? coverage_mask_tx.gpu_texture() : white_tx.gpu_texture());
          draw.bind_texture("preview_depth_tx", preview_depth_tx.is_valid() ? preview_depth_tx.gpu_texture() : white_tx.gpu_texture());
          draw.push_constant("line_extent", extent);
          draw.push_constant("instance_start", range.start);
          draw.push_constant("antialiasing", graph.antialiasing);
          draw.push_constant("quick_preview", quick && preview_depth_tx.is_valid());
          draw.push_constant("coverage_samples", samples);
          draw.push_constant("coverage_mask", !guide_pass);
          draw.push_constant("color_pass", bool(guide_pass) && mapped);
          draw.draw_procedural(GPU_PRIM_TRIS, 1, uint(range.count) * 6);
          inst.manager->submit(draw, view);
        }
      }
    };
    uint64_t expansion_key = hash_value(result_signature, quick);
    expansion_key = hash_value(expansion_key, scale);
    expansion_key = hash_value(expansion_key, graph.antialiasing);
    expansion_key = hash_value(expansion_key, reduction_key);
    expansion_key = hash_value(expansion_key, map_key);
    for (const auto &rule : graph.rules) {
      expansion_key = hash_value(expansion_key, rule.width_reduction);
      expansion_key = hash_value(expansion_key, rule.opacity_reduction);
      expansion_key = hash_value(expansion_key, rule.hidden_width_reduction);
      expansion_key = hash_value(expansion_key, rule.hidden_opacity_reduction);
    }
    for (const auto &brush : graph.brushes) {
      if (brush.fields) {
        expansion_key = hash_value(expansion_key, brush.fields->shape_signature);
        if (brush.fields->shape_uses_image) { expansion_key = hash_value(expansion_key, image_revision); }
        for (const auto &source : brush.fields->sources) {
          if (source.type == 2) {
            expansion_key = hash_value(expansion_key, inst.render_buffers.data.aovs);
            expansion_key = hash_value(expansion_key, inst.render_buffers.data.line_aov_marker_layers);
          }
        }
        /* Source AOVs belong to the current sample, never to the Film history. */
        if (brush.fields->shape_uses_aov) {
          expansion_key = hash_value(expansion_key, inst.sampling.sample_index());
          expansion_key = hash_value(expansion_key, DEG_get_update_count(inst.depsgraph));
        }
      }
      auto data = pack_brush(brush);
      data.field_program = int4(0);
      data.field_outputs = int4(0);
      data.color = float4(0);
      data.tip.y = 0;
      data.variation.y = 0;
      data.taper.z = data.taper.w = 0;
      data.dash = float4(0);
      data.offset_noise.w = 0;
      data.options.w = 0;
      data.curve_options.y = 0;
      data.maps.z = data.maps.w = 0;
      data.detail_maps.y = 0;
      if (brush.brush_map < 0 || graph.maps[brush.brush_map].coordinates != 2) {
        data.detail_maps.x = 0;
      }
      expansion_key = hash_value(expansion_key, data);
      for (const float2 &sample : brush.curve) { expansion_key = hash_value(expansion_key, sample.x); }
    }
    for (const auto &rule : graph.rules) { expansion_key = hash_value(expansion_key, rule.width_scale); }
    if (expansion_cached && expansion_signature == expansion_key) {
      for (const auto &chunk : expanded_chunks) { draw_ranges(chunk.instances, chunk.parts, chunk.ranges); }
      resolve_coverage();
      GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
      return;
    }
    for (auto &chunk : expanded_chunks) { chunk.free(); }
    expanded_chunks.clear();
    if (std::getenv("BLENDER_NPR_LINE_STATS")) {
      std::printf("NPR_BRUSH_FIELDS event=expand\n"); std::fflush(stdout);
    }
    expansion_signature = expansion_key;
    expansion_cached = true;
    size_t cached_bytes = 0;
    constexpr size_t cache_budget = 64 * 1024 * 1024;
    std::vector<const glines::Stroke *> ordered;
    ordered.reserve(result->strokes.size());
    for (const auto &stroke : result->strokes) { ordered.push_back(&stroke); }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto *a, const auto *b) {
      const int ra = a->parts.front().segment.rule, rb = b->parts.front().segment.rule;
      return ra != rb ? ra > rb : a->brush < b->brush;
    });

    /* Original path geometry is independent of draw chunking. X remapping can cross
     * part boundaries while preserving source attributes and non-drawable gaps. */
    std::vector<GeometryLinePartData> original_parts;
    std::map<const glines::Stroke *, uint2> source_ranges;
    auto source_color = [&](const glines::AttributePoint &point, const glines::UVMapKey &key, bool &valid) {
      float4 result(0);
      valid = false;
      if (point.triangle >= 0) {
        const auto found = geometry->style_colors.find(key);
        if (found != geometry->style_colors.end() && size_t(point.triangle) * 3 + 2 < found->second.size()) {
          const auto *colors = found->second.data() + size_t(point.triangle) * 3;
          result = colors[0] + (colors[1] - colors[0]) * point.barycentric.x + (colors[2] - colors[0]) * point.barycentric.y;
          valid = true;
        }
      }
      else if (point.triangle < -1) {
        const size_t edge = size_t(-int64_t(point.triangle) - 2);
        const auto found = geometry->point_colors.find(key);
        if (edge < geometry->edges.size() && found != geometry->point_colors.end()) {
          const int2 vertices = geometry->edges[edge].vertices;
          result = found->second[vertices.x] + (found->second[vertices.y] - found->second[vertices.x]) * point.barycentric.x;
          valid = true;
        }
      }
      for (int i = 0; i < 4; i++) { valid &= std::isfinite(result[i]); }
      return valid ? result : float4(0);
    };
    for (const glines::Stroke *stroke : ordered) {
      const uint32_t first = uint32_t(original_parts.size());
      const auto &brush = graph.brushes[stroke->brush];
      if (!brush.fields) { continue; }
      for (const auto &part : stroke->parts) {
        const auto &segment = part.segment;
        GeometryLinePartData packed{};
        packed.a = float4(float3(segment.a), 0);
        packed.b = float4(float3(segment.b), 0);
        packed.arc = float4(float(part.arc), float(part.length), float(stroke->length), segment.width_scale);
        packed.meta = uint4(uint32_t(stroke->identity ^ (stroke->identity >> 32)), stroke->closed ? 1 : 0, 0, 0);
        packed.offset_normals = float4(part.offset_normal_a.x, part.offset_normal_a.y, part.offset_normal_b.x, part.offset_normal_b.y);
        packed.field_weights = float4(segment.attribute_a.inverse_w, segment.attribute_b.inverse_w, 0, 0);
        packed.field_meta.w = segment.hidden ? 1 : 0;
        const glines::Surface *surface = nullptr;
        const int triangle = segment.attribute_a.triangle;
        if (triangle >= 0 && triangle < int(geometry->triangles.size())) {
          surface = &geometry->surfaces[geometry->faces[geometry->triangles[triangle].face].surface];
          packed.field_meta.z = surface->resource_id;
        }
        if (brush.fields) {
          for (int i = 0; i < std::min<int>(LINE_FIELD_MAX_SOURCES, brush.fields->sources.size()); i++) {
            const auto &source = brush.fields->sources[i];
            bool valid_a = false, valid_b = false;
            if (source.type == 0) {
              glines::TextureMap map;
              map.uv = source.attribute;
              float4 a = coordinates(segment.attribute_a, map), b = coordinates(segment.attribute_b, map);
              packed.field_source_a[i] = float4(a.x, a.y, 0, 0);
              packed.field_source_b[i] = float4(b.x, b.y, 0, 0);
              valid_a = a.w != 0; valid_b = b.w != 0;
            }
            else if (source.type == 1) {
              packed.field_source_a[i] = source_color(segment.attribute_a, source.attribute, valid_a);
              packed.field_source_b[i] = source_color(segment.attribute_b, source.attribute, valid_b);
            }
            else {
              int layer = -1, marker = -1;
              const auto &buffers = inst.render_buffers.data;
              const auto &aovs = buffers.aovs;
              const uint32_t hash = BLI_hash_string(source.attribute.name.c_str());
              const int begin = source.aov_type ? aovs.color_len : 0;
              const int end = source.aov_type ? aovs.color_len + aovs.value_len : aovs.color_len;
              for (int index = begin; index < end; index++) {
                if (aovs.hash[index / 4][index % 4] == hash && buffers.line_aov_marker_info.x >= 0) {
                  layer = (source.aov_type ? buffers.value_len - aovs.color_len : buffers.color_len) + index;
                  marker = buffers.line_aov_marker_layers[index / 4][index % 4];
                }
              }
              const uint32_t material = surface ? surface->material_tag : 0;
              packed.field_source_a[i] = packed.field_source_b[i] = float4(layer, marker, material & 65535u, material >> 16u);
            }
            if (valid_a) { packed.field_meta.x |= 1u << i; }
            if (valid_b) { packed.field_meta.y |= 1u << i; }
          }
        }
        original_parts.push_back(packed);
      }
      const uint32_t count = uint32_t(original_parts.size()) - first;
      source_ranges[stroke] = uint2(first, count);
      for (uint32_t i = first; i < first + count; i++) { original_parts[i].source_range = uint4(first, count, i, 0); }
    }
    upload_buffer(original_buffer, original_capacity, original_parts, "GeometryLines.OriginalParts");

    /* Bound each dispatch/storage allocation without dropping strokes. A single task is
     * further divided below when an unusually long stroke exceeds the chunk budget. */
    constexpr uint32_t chunk_budget = 262144;
    std::vector<GeometryLinePartData> parts;
    std::vector<DrawRange> ranges;
    uint32_t instance_count = 0;
    auto flush = [&]() {
      if (!instance_count) { return; }
      const int part_count = int(parts.size());
      upload_buffer(parts_buffer, parts_capacity, parts, "GeometryLines.Parts");
      const size_t desired = buffer_capacity(instance_count);
      if (!instances_buffer || instances_capacity < desired) {
        if (instances_buffer) { GPU_storagebuf_free(instances_buffer); }
        instances_capacity = desired;
        instances_buffer = GPU_storagebuf_create_ex(desired * sizeof(GeometryLineInstanceData), nullptr,
                                                     GPU_USAGE_DEVICE_ONLY, "GeometryLines.Instances");
      }
      draw::PassSimple expand("GeometryLines.Expand");
      expand.init();
      expand.shader_set(expand_shader);
      expand.bind_ssbo("parts", parts_buffer);
      expand.bind_ssbo("brushes", brushes_buffer);
      expand.bind_ssbo("brush_curves", curves_buffer);
      expand.bind_ssbo("reductions", reductions_buffer);
      expand.bind_ssbo("line_maps", maps_buffer);
      const auto &brush = graph.brushes[parts.front().range.z];
      bind_fields(expand, brush);
      ImageUser image_user{};
      BKE_imageuser_default(&image_user);
      image_user.framenr = inst.scene->r.cfra;
      gpu::Texture *width_map_texture = brush.width_map >= 0 && graph.maps[brush.width_map].image ?
          BKE_image_get_gpu_texture(graph.maps[brush.width_map].image, &image_user) : nullptr;
      expand.bind_texture("width_map_tx", width_map_texture ? width_map_texture : white_tx.gpu_texture());
      gpu::Texture *distortion_texture = brush.distortion_map >= 0 && graph.maps[brush.distortion_map].image ?
          BKE_image_get_gpu_texture(graph.maps[brush.distortion_map].image, &image_user) : nullptr;
      expand.bind_texture("distortion_map_tx", distortion_texture ? distortion_texture : transparent_tx.gpu_texture());
      expand.bind_ssbo("instances", instances_buffer);
      expand.push_constant("part_count", part_count);
      expand.push_constant("instance_count", int(instance_count));
      expand.push_constant("width_scale", scale);
      expand.push_constant("world_from_clip", world_from_clip);
      expand.push_constant("line_extent", extent);
      expand.push_constant("raster_response", graph.antialiasing > 0);
      expand.push_constant("raster_antialiasing", graph.antialiasing);
      expand.dispatch(int((instance_count + 63) / 64));
      inst.manager->submit(expand, view);
      GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
      draw_ranges(instances_buffer, parts_buffer, ranges);
      const size_t bytes = instances_capacity * sizeof(GeometryLineInstanceData) + parts_capacity * sizeof(GeometryLinePartData);
      if (expansion_cached && cached_bytes + bytes <= cache_budget) {
        cached_bytes += bytes;
        expanded_chunks.push_back({instances_buffer, parts_buffer, ranges});
        instances_buffer = nullptr;
        instances_capacity = 0;
        parts_buffer = nullptr;
        parts_capacity = 0;
      }
      else {
        expansion_cached = false;
        for (auto &chunk : expanded_chunks) { chunk.free(); }
        expanded_chunks.clear();
      }

      parts.clear();
      ranges.clear();
      instance_count = 0;
    };
    for (const glines::Stroke *stroke : ordered) {
      if (!parts.empty() && parts.back().range.z != uint32_t(stroke->brush)) { flush(); }
      const auto &brush = graph.brushes[stroke->brush];
      for (int copy = 0; copy < brush.copies; copy++) {
        const StrokeTipParameters tip = brush.mode != 0 &&
                                               (brush.procedural_tip() || brush.angle_random != 0) ?
            stroke_tip_parameters(*stroke, brush, stroke->brush, copy, extent.y) : StrokeTipParameters{};
        for (int i = 0; i < int(stroke->parts.size()); i++) {
          const glines::StrokePart &part = stroke->parts[i];
          const auto &segment = part.segment;
          if (part.length < 1e-8) { continue; }
          const float type_scale = graph.rules[segment.rule].width_scale[segment.type];
          if (type_scale <= 0 || brush.width <= 0) { continue; }
          /* Simple follows the visibility path lattice. Textured strokes use the
           * measured half-pixel raster lattice and include both terminal stamps. */
          const uint64_t grid = brush.mode == 0 ?
              std::max<uint64_t>(1, uint64_t(std::ceil(part.length / (brush.fields ? brush.fields->resample : 0.9f)))) :
              uint64_t(float(part.length) * 2.0f) + 1;
          const double spacing = brush.spacing <= 0 ? 0 :
                                     std::max(0.35, double(brush.width * scale * type_scale * brush.spacing));
          const uint64_t stride = brush.mode == 0 ? 1 :
                                      std::max<uint64_t>(1, uint64_t(std::ceil(spacing / (part.length / grid))));
          const uint64_t samples = brush.mode == 0 ? grid : (grid + stride - 1) / stride + 1;
          const uint64_t batch_samples = chunk_budget;
          for (uint64_t first = 0; first < samples; first += batch_samples) {
            const uint32_t count = uint32_t(std::min(samples - first, batch_samples));
            if (instance_count + count > chunk_budget) { flush(); }
            GeometryLinePartData packed{};
            if (brush.fields) { packed = original_parts[source_ranges[stroke].x + i]; }
            packed.a = float4(float3(segment.a), tip.rotation);
            packed.b = float4(float3(segment.b), 0);
            packed.previous_a = float4(float3(segment.previous_a), 0);
            packed.previous_b = float4(float3(segment.previous_b), 0);
            packed.next_a = float4(float3(segment.next_a), 0);
            packed.next_b = float4(float3(segment.next_b), 0);
            packed.arc = float4(float(part.arc), float(part.length), float(stroke->length), type_scale);
            packed.offset_normals = float4(part.offset_normal_a.x, part.offset_normal_a.y,
                                            part.offset_normal_b.x, part.offset_normal_b.y);
            packed.range = uint4(instance_count, count, stroke->brush, copy);
            const auto &rule = graph.rules[segment.rule];
            const uint32_t flags = uint32_t(stroke->closed) | (uint32_t(i == 0) << 1) |
                                   (uint32_t(i + 1 == int(stroke->parts.size())) << 2);
            packed.meta = uint4(uint32_t(stroke->identity ^ (stroke->identity >> 32)), flags | (tip.layer << 8),
                                uint32_t(segment.hidden ? rule.hidden_width_reduction : rule.width_reduction),
                                uint32_t(segment.hidden ? rule.hidden_opacity_reduction : rule.opacity_reduction));
            packed.sampling = uint4(uint32_t(first * stride), uint32_t(stride), uint32_t(grid),
                                    uint32_t(segment.instance ^ (segment.instance >> 32)));
            packed.width_uv_a = packed.width_uv_b = float4(0);
            if (brush.width_map >= 0) {
              const auto &map = graph.maps[brush.width_map];
              packed.width_uv_a = coordinates(segment.attribute_a, map);
              packed.width_uv_b = coordinates(segment.attribute_b, map);
              if (map.coordinates == 0 && (packed.width_uv_a.w == 0 || packed.width_uv_b.w == 0)) {
                missing_width_uv = true;
              }
              missing_attribute |= map.coordinates == 2 && (packed.width_uv_a.w == 0 || packed.width_uv_b.w == 0);
            }
            packed.color_uv_a = packed.color_uv_b = float4(0);
            if (brush.color_map >= 0) {
              const auto &map = graph.maps[brush.color_map];
              packed.color_uv_a = coordinates(segment.attribute_a, map);
              packed.color_uv_b = coordinates(segment.attribute_b, map);
              missing_color_uv |= map.coordinates == 0 && (packed.color_uv_a.w == 0 || packed.color_uv_b.w == 0);
              missing_attribute |= map.coordinates == 2 && (packed.color_uv_a.w == 0 || packed.color_uv_b.w == 0);
            }
            packed.distortion_uv_a = packed.distortion_uv_b = float4(0);
            if (brush.distortion_map >= 0) {
              const auto &map = graph.maps[brush.distortion_map];
              packed.distortion_uv_a = coordinates(segment.attribute_a, map);
              packed.distortion_uv_b = coordinates(segment.attribute_b, map);
              missing_distortion_uv |= map.coordinates == 0 && (packed.distortion_uv_a.w == 0 || packed.distortion_uv_b.w == 0);
              missing_attribute |= map.coordinates == 2 && (packed.distortion_uv_a.w == 0 || packed.distortion_uv_b.w == 0);
            }
            packed.brush_color_a = packed.brush_color_b = float4(0);
            if (brush.brush_map >= 0 && graph.maps[brush.brush_map].coordinates == 2) {
              const auto &map = graph.maps[brush.brush_map];
              packed.brush_color_a = coordinates(segment.attribute_a, map);
              packed.brush_color_b = coordinates(segment.attribute_b, map);
              missing_attribute |= packed.brush_color_a.w == 0 || packed.brush_color_b.w == 0;
            }
            parts.push_back(packed);
            if (!ranges.empty() && ranges.back().brush == stroke->brush && ranges.back().rule == segment.rule &&
                ranges.back().start + ranges.back().count == int(instance_count)) {
              ranges.back().count += int(count);
            }
            else { ranges.push_back({stroke->brush, segment.rule, int(instance_count), int(count)}); }
            instance_count += count;
          }
        }
      }
    }
    flush();
    resolve_coverage();
    if (missing_width_uv) { inst.info_append("Geometry Lines: width map has no valid source UV; using the base width"); }
    if (missing_color_uv) { inst.info_append("Geometry Lines: color map has no valid source UV; using the base color"); }
    if (missing_distortion_uv) { inst.info_append("Geometry Lines: distortion map has no valid source UV; using the base path"); }
    if (missing_attribute) { inst.info_append("Geometry Lines: a requested color attribute is missing; using black"); }
    GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
  }

  gpu::Texture *display_texture()
  {
    if (!inst.is_viewport() && accumulated_samples) {
      return processed_accum_tx.is_valid() ? processed_accum_tx.gpu_texture() :
             raw_accum_tx.is_valid() ? raw_accum_tx.gpu_texture() : nullptr;
    }
    return current_display_texture();
  }

  gpu::Texture *current_display_texture()
  {
    return processed_tx.is_valid() ? processed_tx.gpu_texture() :
           color_tx.is_valid() ? color_tx.gpu_texture() : nullptr;
  }

  void reset_accumulation()
  {
    if (!inst.is_viewport()) { inst.native_postfx_outputs.release_geometry_lines_history(); }
    raw_reference.reset();
    reference_geometry.reset();
    reference_graph.reset();
    reference_camera.reset();
    accumulated_samples = 0;
    last_accumulated_sample = 0;
    last_accumulated_paint_signature = 0;
    raw_accumulation_static = true;
    raw_accum_tx.free();
    processed_accum_tx.free();
  }

  void accumulate(draw::View &view)
  {
    if (inst.is_viewport() || !enabled || !color_tx.is_valid()) { return; }
    const uint64_t sample = inst.sampling.sample_index();
    if (accumulated_samples && sample == last_accumulated_sample) { return; }
    if (sample < last_accumulated_sample) { reset_accumulation(); }
    if (!accumulate_shader) {
      accumulate_shader = inst.shaders.static_shader_get(GEOMETRY_LINES_ACCUMULATE);
    }
    if (!accumulate_shader) {
      inst.info_append("Geometry Lines: GPU accumulation shader compilation failed");
      return;
    }
    const float weight = 1.0f / float(accumulated_samples + 1);
    auto append = [&](gpu::Texture *input, draw::Texture &output) {
      const bool resized = output.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, extent,
          GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE);
      draw::PassSimple pass("GeometryLines.Accumulate");
      pass.init();
      pass.shader_set(accumulate_shader);
      pass.bind_texture("input_tx", input);
      pass.bind_image("accumulation_img", output);
      pass.push_constant("sample_weight", resized ? 1.0f : weight);
      pass.dispatch(int2((extent.x + 15) / 16, (extent.y + 15) / 16));
      inst.manager->submit(pass, view);
    };
    /* The central projection gives crisp, deterministic default line coverage. Average only
     * the independent premultiplied images over the same accepted Film samples; shutter times
     * already follow the renderer's shutter-curve distribution. No line data enters scene history. */
    if (processed_tx.is_valid() && !processed_accum_tx.is_valid() && accumulated_samples &&
        raw_accum_tx.is_valid()) {
      processed_accum_tx.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, extent,
          GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE);
      GPU_texture_copy(processed_accum_tx, raw_accum_tx);
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_SHADER_IMAGE_ACCESS);
    }
    if (accumulated_samples && last_accumulated_paint_signature != paint_signature) {
      raw_accumulation_static = false;
    }
    if (!raw_accum_tx.is_valid() || !raw_accumulation_static) {
      append(color_tx, raw_accum_tx);
    }
    if (processed_tx.is_valid() || processed_accum_tx.is_valid()) {
      append(current_display_texture(), processed_accum_tx);
    }
    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH |
                       GPU_BARRIER_TEXTURE_UPDATE);
    accumulated_samples++;
    last_accumulated_sample = sample;
    last_accumulated_paint_signature = paint_signature;
  }

  void process_effects(draw::View &view)
  {
    if (quick || !color_tx.is_valid() ||
        ((!graph.depth_of_field || !inst.depth_of_field.postfx_enabled()) &&
         (!graph.motion_blur || !inst.motion_blur.postfx_enabled()))) {
      processed_tx.free();
      inst.native_postfx_outputs.release_geometry_lines_history();
      return;
    }
    if (!quick && color_tx.is_valid() && (graph.depth_of_field || graph.motion_blur)) {
      auto *processed = inst.native_postfx_outputs.render_geometry_lines_for_combined(
          view, color_tx, depth_tx, velocity_tx, graph.depth_of_field, graph.motion_blur,
          hash_value(graph.paint_signature, image_revision));
      if (processed && processed != color_tx.gpu_texture()) {
        processed_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, extent,
                              GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE);
        GPU_texture_copy(processed_tx, processed);
        GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
      }
    }
    else { inst.native_postfx_outputs.release_geometry_lines_history(); }
  }

  void report_result()
  {
    if (timing_enabled && result &&
        (timing_result_signature != result_signature || timing_quick != quick)) {
      timing_result_signature = result_signature;
      timing_quick = quick;
      timing_statistics = result->statistics;
      measured_prepare_ms = prepare_ms;
    }
    if (result && std::getenv("BLENDER_NPR_LINE_STATS")) {
      std::printf("NPR_GEOMETRY_LINES preview=%s candidates=%llu triangles=%llu segments=%zu strokes=%zu visibility_ms=%.3f connection_ms=%.3f intersection_ms=%.3f\n",
                  quick ? "quick" : "full", static_cast<unsigned long long>(result->statistics.candidates),
                  static_cast<unsigned long long>(result->statistics.triangle_tests),
                  result->segments.size(), result->strokes.size(), result->statistics.visibility_ms,
                  result->statistics.connection_ms, result->statistics.intersection_ms);
      std::fflush(stdout);
    }
    if (result && std::getenv("BLENDER_NPR_LINE_DUMP_STROKES")) {
      for (size_t i = 0; i < result->strokes.size(); i++) {
        const auto &stroke = result->strokes[i];
        std::printf("NPR_LINE_STROKE {\"stroke\":%zu,\"length\":%.12g,\"closed\":%d,\"parts\":%zu}\n",
                    i, stroke.length, int(stroke.closed), stroke.parts.size());
        for (const auto &part : stroke.parts) {
          const auto &s = part.segment;
          std::printf("NPR_LINE_PART {\"stroke\":%zu,\"a\":[%.12g,%.12g,%.12g],\"b\":[%.12g,%.12g,%.12g],\"length\":%.12g,\"arc\":%.12g,\"type\":%d,\"hidden\":%d,\"identity\":%llu,\"va\":%llu,\"vb\":%llu,\"range\":[%.12g,%.12g]}\n",
                      i, s.a.x, s.a.y, s.join_depth_a, s.b.x, s.b.y, s.join_depth_b,
                      part.length, part.arc, s.type, int(s.hidden),
                      static_cast<unsigned long long>(s.identity),
                      static_cast<unsigned long long>(s.vertex_a), static_cast<unsigned long long>(s.vertex_b),
                      s.source_start, s.source_end);
        }
      }
      std::fflush(stdout);
    }
  }

  void publish_timing(bool repainted, double submission_ms)
  {
    if (!timing_enabled || !result || !inst.scene->eevee.geometry_line_tree) { return; }
    if (timing_result_signature == 0) { report_result(); }
    std::ostringstream text;
    const auto &s = timing_statistics;
    text << std::fixed << std::setprecision(2);
    text << IFACE_("Geometry Lines CPU") << " · " << (quick ? IFACE_("Quick") : IFACE_("Full"));
    text << "\n" << IFACE_("Geometry computation") << ": "
         << measured_prepare_ms + s.visibility_ms + s.connection_ms + s.intersection_ms << " ms";
    text << "\n" << IFACE_("Preparation") << " " << measured_prepare_ms << " · "
         << IFACE_("Visibility") << " " << s.visibility_ms << " ms";
    text << "\n" << IFACE_("Intersection lines") << " " << s.intersection_ms << " · "
         << IFACE_("Connection") << " " << s.connection_ms << " ms";
    if (repainted) { drawing_ms = submission_ms; }
    text << "\n" << IFACE_("Drawing submission") << ": " << drawing_ms << " ms";
    text << "\n" << IFACE_("Display submission") << ": " << submission_ms << " ms · "
         << (repainted ? IFACE_("Updated") : IFACE_("Cached"));
    text << "\n" << result->segments.size() << " " << IFACE_("segments") << " · "
         << result->strokes.size() << " " << IFACE_("strokes") << " · " << drawing_instances << " " << IFACE_("instances");
    nodes::npr_line_set_timing(*inst.scene->eevee.geometry_line_tree, text.str());
  }

  void render(draw::View &view)
  {
    if (!enabled || !geometry) { return; }
    if (raw_reference && !raw_reference->color_tx.is_valid()) { raw_reference->render(view); }
    render_view = &view;
    extent = inst.film.film_extent_get();
    if (extent.x <= 0 || extent.y <= 0) { return; }
    glines::Camera camera;
    camera.extent = extent;
    camera.position = double3(view.location());
    camera.forward = -double3(view.forward());
    camera.projection = double4x4(view.persmat());
    if (reference_camera) {
      camera.position = double3(reference_camera->viewinv.location());
      camera.forward = -double3(reference_camera->viewinv.z_axis());
      camera.projection = double4x4(reference_camera->persmat);
    }
    camera.previous_projection = has_previous_projection ? previous_projection : camera.projection;
    camera.next_projection = camera.projection;
    if (reference_camera) { camera.previous_projection = camera.projection; }
    if (geometry->has_motion_steps) {
      camera.previous_projection = double4x4(inst.velocity.camera_steps[STEP_PREVIOUS]->persmat);
      camera.next_projection = double4x4(inst.velocity.camera_steps[STEP_NEXT]->persmat);
    }
    camera.perspective = view.is_persp();
    if (reference_camera) { camera.perspective = reference_camera->type == CAMERA_PERSP; }
    reduction_camera_position = camera.position;
    world_from_clip = float4x4(math::invert(camera.projection));
    camera.use_previous_geometry = !reference_camera && geometry->revision != last_geometry_revision;
    double margin = 0;
    const double width_scale = graph.width_scale * (graph.relative_width ? extent.x / std::max(graph.reference_width, 1.0f) : 1.0f);
    float maximum_type_scale = 1;
    for (const auto &rule : graph.rules) {
      for (float scale : rule.width_scale) { maximum_type_scale = std::max(maximum_type_scale, scale); }
    }
    for (const auto &brush : graph.brushes) {
      margin = std::max(margin, double(brush.width * std::max(brush.stretch, 1.0f) * 0.5 * width_scale * maximum_type_scale +
                                       std::abs(brush.offset) + brush.offset_random + brush.wave * (1 + brush.wave_random) + brush.noise + brush.extend +
                                       (brush.distortion_map >= 0 ? brush.distortion_map_amount : 0) +
                                       std::max(std::abs(brush.shift_x), std::abs(brush.shift_y))));
    }
    camera.clip_margin = std::max(64.0, std::ceil((margin + 4) / 64) * 64);
    camera.clip_margin = std::max(camera.clip_margin, double(graph.offscreen_margin));
    uint64_t signature = hash_value(geometry->revision, camera.projection);
    if (graph.depth_of_field || graph.motion_blur) {
      signature = hash_value(signature, camera.previous_projection);
      signature = hash_value(signature, camera.next_projection);
      signature = hash_value(signature, camera.use_previous_geometry);
    }
    signature = hash_value(signature, extent);
    signature = hash_value(signature, camera.clip_margin);
    const uint64_t visibility_key = signature;
    signature = hash_value(signature, graph.connection_signature);
    const bool interactive = inst.is_viewport() && !inst.is_viewport_image_render && !inst.is_image_render;
    const auto now = std::chrono::steady_clock::now();
    if (requested_signature != signature) {
      worker.cancel();
      requested_signature = signature;
      changed_at = now;
      current_job = LineJob{geometry, camera, graph.plan, graph.rules, signature, visibility_key};
    }
    const bool moving = inst.is_navigating || inst.is_transforming || inst.is_playback;
    if (interactive && moving) { changed_at = now; }
    const bool automatic = interactive && graph.preview == 0;
    const bool approximate = interactive && graph.preview != 1;
    if (!approximate && (result_signature != signature || quick)) {
      worker.cancel();
      result = evaluator.compute(*current_job, cancelled);
      quick = false;
      result_signature = result ? signature : 0;
      paint_signature = 0;
    }
    else if (approximate && (result_signature != signature || !result || (graph.preview == 2 && !quick))) {
      auto quick_camera = camera;
      quick_camera.quick = true;
      auto next = std::make_shared<glines::Result>(glines::evaluate(*geometry, quick_camera,
                                         graph.plan, graph.rules, {}, cancelled, false));
      for (const auto &segment : next->segments) {
        glines::Stroke stroke;
        stroke.identity = segment.identity;
        stroke.brush = segment.brush;
        stroke.length = std::hypot(segment.b.x - segment.a.x, segment.b.y - segment.a.y);
        stroke.parts.push_back({segment, 0, stroke.length});
        next->strokes.push_back(std::move(stroke));
      }
      result = std::move(next);
      result_signature = signature;
      quick = true;
      paint_signature = 0;
    }
    if (automatic && quick && !moving && now - changed_at >= std::chrono::milliseconds(200)) {
      worker.request(*current_job);
      if (auto complete = worker.take(signature)) {
        result = std::move(complete);
        quick = false;
        paint_signature = 0;
      }
    }
    if (automatic && quick) { DRW_viewport_request_redraw(); }
    if (quick && inst.render_buffers.depth_tx.is_valid()) {
      auto *source = inst.render_buffers.depth_tx.gpu_texture();
      preview_depth_tx.ensure_2d(GPU_texture_format(source),
                                int2(GPU_texture_width(source), GPU_texture_height(source)),
                                GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT);
      GPU_texture_copy(preview_depth_tx, source);
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
    }
    if (paint_signature == 0) { report_result(); }
    previous_projection = camera.projection;
    has_previous_projection = true;
    last_geometry_revision = geometry->revision;
    uint64_t paint_key = hash_value(hash_value(result_signature, quick), graph.paint_signature);
    if (inst.geometry_lines.uses_aov()) {
      paint_key = hash_value(paint_key, inst.sampling.sample_index());
      paint_key = hash_value(paint_key, DEG_get_update_count(inst.depsgraph));
    }
    const auto drawing_start = std::chrono::steady_clock::now();
    const bool repainted = paint_signature != paint_key || !color_tx.is_valid();
    if (paint_signature != paint_key || !color_tx.is_valid()) {
      auto *previous_framebuffer = GPU_framebuffer_active_get();
      paint(view);
      if (previous_framebuffer) { GPU_framebuffer_bind(previous_framebuffer); }
      paint_signature = paint_key;
    }
    auto *previous_framebuffer = GPU_framebuffer_active_get();
    process_effects(view);
    if (previous_framebuffer) { GPU_framebuffer_bind(previous_framebuffer); }
    if (timing_enabled) { publish_timing(repainted, elapsed_ms(drawing_start)); }
  }
};

GeometryLineModule::GeometryLineModule(Instance &instance) : impl_(std::make_unique<Impl>(instance)) {}
GeometryLineModule::~GeometryLineModule() = default;
void GeometryLineModule::begin_sync() { impl_->begin_sync(); }
void GeometryLineModule::sync_object(const draw::ObjectRef &object)
{
  if (impl_->enabled && !impl_->reference_geometry) {
    const auto start = std::chrono::steady_clock::now();
    const auto handle = impl_->inst.manager->unique_handle(object);
    impl_->builder.add(object, handle.sub_handle(0).index());
    if (impl_->timing_enabled) { impl_->prepare_ms += elapsed_ms(start); }
  }
}
void GeometryLineModule::end_sync() { impl_->end_sync(); }
void GeometryLineModule::render(draw::View &view)
{
  auto *framebuffer = GPU_framebuffer_active_get();
  impl_->render(view);
  if (framebuffer) { GPU_framebuffer_bind(framebuffer); }
}
void GeometryLineModule::reset_accumulation() { impl_->reset_accumulation(); }
void GeometryLineModule::capture_reference_geometry() { impl_->capture_reference_geometry(); }
void GeometryLineModule::accumulate(draw::View &view) { impl_->accumulate(view); }
bool GeometryLineModule::enabled() const { return impl_->enabled; }
bool GeometryLineModule::use_in_combined() const { return impl_->enabled && impl_->graph.composite; }
bool GeometryLineModule::is_quick_preview() const { return impl_->enabled && impl_->quick; }
bool GeometryLineModule::uses_aov() const
{
  if (!impl_->enabled) { return false; }
  for (const auto &brush : impl_->graph.brushes) {
    if (brush.fields && std::any_of(brush.fields->sources.begin(), brush.fields->sources.end(),
                                   [](const auto &source) { return source.type == 2; })) { return true; }
  }
  return false;
}
bool GeometryLineModule::uses_aov(uint32_t hash, bool value_type) const
{
  if (!impl_->enabled) { return false; }
  for (const auto &brush : impl_->graph.brushes) {
    if (!brush.fields) { continue; }
    for (const auto &source : brush.fields->sources) {
      if (source.type == 2 && bool(source.aov_type) == value_type &&
          BLI_hash_string(source.attribute.name.c_str()) == hash) { return true; }
    }
  }
  return false;
}
std::string GeometryLineModule::timing_info() const
{
  return impl_->timing_enabled && impl_->inst.scene->eevee.geometry_line_tree ?
             nodes::npr_line_timing(*impl_->inst.scene->eevee.geometry_line_tree) : std::string();
}
gpu::Texture *GeometryLineModule::texture() const { return impl_->color_tx.is_valid() ? impl_->color_tx.gpu_texture() : nullptr; }
gpu::Texture *GeometryLineModule::pass_texture() const
{
  if (impl_->raw_reference && impl_->raw_reference->color_tx.is_valid()) {
    return impl_->raw_reference->color_tx.gpu_texture();
  }
  return !impl_->inst.is_viewport() && impl_->accumulated_samples && impl_->raw_accum_tx.is_valid() ?
             impl_->raw_accum_tx.gpu_texture() : texture();
}
gpu::Texture *GeometryLineModule::depth_texture() const { return impl_->depth_tx.is_valid() ? impl_->depth_tx.gpu_texture() : nullptr; }
gpu::Texture *GeometryLineModule::velocity_texture() const { return impl_->velocity_tx.is_valid() ? impl_->velocity_tx.gpu_texture() : nullptr; }
float *GeometryLineModule::read_pass()
{
  return pass_texture() ? static_cast<float *>(GPU_texture_read(pass_texture(), GPU_DATA_FLOAT, 0)) : nullptr;
}

void GeometryLineModule::present(draw::View &view)
{
  const auto timing_start = std::chrono::steady_clock::now();
  bool repainted = false;
  if (impl_->enabled && impl_->current_job) {
    /* Image.update and texture painting can change pixels without a depsgraph rebuild.
     * Track the same partial-update register as Blender's GPU textures, without readback. */
    if (impl_->refresh_images()) {
      impl_->image_revision++;
      impl_->graph.paint_signature = hash_value(impl_->graph.paint_signature, impl_->image_revision);
      impl_->inst.sampling.reset();
    }
    const bool moving = impl_->inst.is_navigating || impl_->inst.is_transforming || impl_->inst.is_playback;
    if (moving) { impl_->changed_at = std::chrono::steady_clock::now(); }
    if (impl_->quick && impl_->graph.preview == 0 && !moving &&
        std::chrono::steady_clock::now() - impl_->changed_at >= std::chrono::milliseconds(200)) {
      impl_->worker.request(*impl_->current_job);
      if (auto complete = impl_->worker.take(impl_->requested_signature)) {
        impl_->result = std::move(complete);
        impl_->quick = false;
        impl_->paint_signature = 0;
        impl_->report_result();
      }
    }
    if (impl_->quick && impl_->graph.preview == 0) { DRW_viewport_request_redraw(); }
    const uint64_t key = hash_value(hash_value(impl_->result_signature, impl_->quick), impl_->graph.paint_signature);
    if (impl_->paint_signature != key) {
      repainted = true;
      auto *framebuffer = GPU_framebuffer_active_get();
      impl_->paint(impl_->render_view ? *impl_->render_view : view);
      impl_->process_effects(impl_->render_view ? *impl_->render_view : view);
      if (framebuffer) { GPU_framebuffer_bind(framebuffer); }
      impl_->paint_signature = key;
    }
  }
  if (!use_in_combined() || !texture() || !impl_->ensure_shaders()) { return; }
  draw::PassSimple pass("GeometryLines.Present");
  pass.init();
  pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA_PREMUL);
  pass.shader_set(impl_->present_shader);
  pass.bind_texture("line_tx", impl_->display_texture());
  pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  impl_->inst.manager->submit(pass, view);
  if (impl_->timing_enabled) { impl_->publish_timing(repainted, elapsed_ms(timing_start)); }
}

gpu::Texture *GeometryLineModule::composite(draw::View &view, gpu::Texture *scene)
{
  if (!use_in_combined() || !texture() || !scene || !impl_->ensure_shaders()) { return scene; }
  const int2 extent(GPU_texture_width(scene), GPU_texture_height(scene));
  impl_->composite_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, extent,
                                GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE);
  draw::PassSimple pass("GeometryLines.Composite");
  pass.init();
  pass.shader_set(impl_->composite_shader);
  pass.bind_texture("scene_tx", scene);
  pass.bind_texture("line_tx", impl_->display_texture());
  pass.bind_image("output_img", impl_->composite_tx);
  pass.dispatch(int2((extent.x + 15) / 16, (extent.y + 15) / 16));
  impl_->inst.manager->submit(pass, view);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_TEXTURE_UPDATE);
  return impl_->composite_tx;
}

}  // namespace blender::eevee
