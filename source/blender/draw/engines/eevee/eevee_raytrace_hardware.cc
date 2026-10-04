/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Hardware scene lifecycle and dispatch, ported from Nuru c603d408e9f3.
 */

#include "BLI_time.h"
#include "GPU_context.hh"
#include "GPU_debug.hh"
#include "GPU_index_buffer.hh"
#include "MEM_guardedalloc.h"

#include "eevee_instance.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace blender::eevee {

static bool hardware_perf_logging_enabled()
{
  return std::getenv("BLENDER_EEVEE_HWRT_PERF_LOG") != nullptr;
}

RayTraceModule::~RayTraceModule()
{
  free_hardware_rt_scene_cache();
}

static Vector<GPUHardwareRaytraceSceneEntry> build_hardware_rt_scene_entries(
    Span<HardwareRaytraceSceneEntry> scene_entries,
    int *r_emissive_entry_count = nullptr,
    float *r_emissive_peak = nullptr)
{
  if (r_emissive_entry_count != nullptr) {
    *r_emissive_entry_count = 0;
  }
  if (r_emissive_peak != nullptr) {
    *r_emissive_peak = 0.0f;
  }

  Vector<GPUHardwareRaytraceSceneEntry> rt_scene_entries;
  rt_scene_entries.reserve(scene_entries.size());

  uint32_t user_id = 0;
  int emissive_entry_count = 0;
  float emissive_peak = 0.0f;
  for (const HardwareRaytraceSceneEntry &entry : scene_entries) {
    if (entry.batch == nullptr) {
      continue;
    }

    GPUHardwareRaytraceSceneEntry rt_entry;
    rt_entry.batch = entry.batch;
    rt_entry.object_to_world = entry.object_to_world;
    rt_entry.instance_count = std::max(1u, entry.instance_count);
    rt_entry.user_id = user_id++;
    rt_entry.emissive_radiance = entry.emissive_radiance;
    rt_entry.diffuse_albedo = entry.diffuse_albedo;
    rt_entry.reflection_color = entry.reflection_color;
    rt_entry.reflection_roughness = entry.reflection_roughness;
    rt_entry.transmission_color = entry.transmission_color;
    rt_entry.transmission_roughness = entry.transmission_roughness;
    rt_entry.reflection_ior = entry.reflection_ior;
    rt_entry.refraction_ior = entry.refraction_ior;
    rt_entry.packed_thickness = entry.packed_thickness;
    rt_entry.alpha = entry.alpha;
    rt_entry.reflection_layer_coverage = entry.reflection_layer_coverage;
    rt_entry.closure_type = entry.closure_type;
    rt_entry.proxy_flags = entry.proxy_flags;
    emissive_peak = std::max(emissive_peak, math::reduce_max(rt_entry.emissive_radiance));
    if (math::reduce_max(rt_entry.emissive_radiance) > 0.0f) {
      emissive_entry_count++;
    }
    rt_entry.material_slot = entry.material_slot;
    rt_entry.is_sculpt = entry.is_sculpt;
    rt_scene_entries.append(rt_entry);
  }

  if (r_emissive_entry_count != nullptr) {
    *r_emissive_entry_count = emissive_entry_count;
  }
  if (r_emissive_peak != nullptr) {
    *r_emissive_peak = emissive_peak;
  }

  return rt_scene_entries;
}

static Vector<HardwareRaytraceSceneEntry> sorted_hardware_scene_entries(
    const Vector<HardwareRaytraceSceneEntry> &scene_entries)
{
  Vector<HardwareRaytraceSceneEntry> sorted_entries = scene_entries;
  std::sort(sorted_entries.begin(),
            sorted_entries.end(),
            [](const HardwareRaytraceSceneEntry &a, const HardwareRaytraceSceneEntry &b) {
              if (a.object_key.hash() != b.object_key.hash()) {
                return a.object_key.hash() < b.object_key.hash();
              }
              if (a.material_slot != b.material_slot) {
                return a.material_slot < b.material_slot;
              }
              if (a.is_sculpt != b.is_sculpt) {
                return int(a.is_sculpt) < int(b.is_sculpt);
              }
              return uintptr_t(a.batch) < uintptr_t(b.batch);
            });
  return sorted_entries;
}

static GPUHardwareRaytraceScene *build_hardware_rt_scene(
    Span<HardwareRaytraceSceneEntry> scene_entries,
                                                         GPUHardwareRaytraceSceneStats *r_stats,
                                                         int *r_emissive_entry_count = nullptr,
                                                         float *r_emissive_peak = nullptr)
{
  if (r_stats != nullptr) {
    *r_stats = {};
  }
  Vector<GPUHardwareRaytraceSceneEntry> rt_scene_entries = build_hardware_rt_scene_entries(
      scene_entries, r_emissive_entry_count, r_emissive_peak);
  return GPU_hardware_raytrace_scene_build(rt_scene_entries.as_span(), r_stats);
}

static const char *hardware_scene_entries_geometry_mismatch_reason(
    const Vector<HardwareRaytraceSceneEntry> &entries,
    const Vector<HardwareRaytraceSceneEntry> &cached_entries,
    int *r_index = nullptr)
{
  if (entries.size() != cached_entries.size()) {
    if (r_index != nullptr) {
      *r_index = -1;
    }
    return "count";
  }
  for (const int i : entries.index_range()) {
    const HardwareRaytraceSceneEntry &entry = entries[i];
    const HardwareRaytraceSceneEntry &cached = cached_entries[i];
    if (!(entry.object_key == cached.object_key)) {
      if (r_index != nullptr) {
        *r_index = i;
      }
      return "object_key";
    }
    if (entry.material_slot != cached.material_slot) {
      if (r_index != nullptr) {
        *r_index = i;
      }
      return "material_slot";
    }
    if (entry.is_sculpt != cached.is_sculpt) {
      if (r_index != nullptr) {
        *r_index = i;
      }
      return "is_sculpt";
    }
    /* Entries with a geometry recalc are rebuilt selectively from their fresh batch; a changed
     * batch pointer or resource range is expected there and must not force a full scene
     * rebuild. */
    const bool geometry_recalc = (entry.recalc & ID_RECALC_GEOMETRY) != 0;
    if (geometry_recalc) {
      continue;
    }
    if (entry.batch != cached.batch) {
      if (r_index != nullptr) {
        *r_index = i;
      }
      return "batch";
    }
    if (entry.resource_handle.id_range() != cached.resource_handle.id_range()) {
      if (r_index != nullptr) {
        *r_index = i;
      }
      return "resource_range";
    }
  }
  return nullptr;
}

static bool hardware_scene_entries_emissive_changed(
    const Vector<HardwareRaytraceSceneEntry> &entries,
    const Vector<HardwareRaytraceSceneEntry> &cached_entries)
{
  if (entries.size() != cached_entries.size()) {
    return false;
  }
  for (const int i : entries.index_range()) {
    const HardwareRaytraceSceneEntry &entry = entries[i];
    const HardwareRaytraceSceneEntry &cached = cached_entries[i];
    if (entry.emissive_radiance != cached.emissive_radiance) {
      return true;
    }
  }
  return false;
}

static bool hardware_scene_entries_transform_changed(
    const Vector<HardwareRaytraceSceneEntry> &entries,
    const Vector<HardwareRaytraceSceneEntry> &cached_entries)
{
  if (entries.size() != cached_entries.size()) {
    return false;
  }
  for (const int i : entries.index_range()) {
    const HardwareRaytraceSceneEntry &entry = entries[i];
    const HardwareRaytraceSceneEntry &cached = cached_entries[i];
    if (entry.object_to_world != cached.object_to_world) {
      return true;
    }
  }
  return false;
}

static bool hardware_scene_entries_instance_count_changed(
    const Vector<HardwareRaytraceSceneEntry> &entries,
    const Vector<HardwareRaytraceSceneEntry> &cached_entries)
{
  if (entries.size() != cached_entries.size()) {
    return false;
  }
  for (const int i : entries.index_range()) {
    const HardwareRaytraceSceneEntry &entry = entries[i];
    const HardwareRaytraceSceneEntry &cached = cached_entries[i];
    if (entry.instance_count != cached.instance_count) {
      return true;
    }
  }
  return false;
}

static bool hardware_scene_entries_material_proxy_changed(
    const Vector<HardwareRaytraceSceneEntry> &entries,
    const Vector<HardwareRaytraceSceneEntry> &cached_entries)
{
  if (entries.size() != cached_entries.size()) {
    return false;
  }
  for (const int i : entries.index_range()) {
    const HardwareRaytraceSceneEntry &entry = entries[i];
    const HardwareRaytraceSceneEntry &cached = cached_entries[i];
    if (entry.diffuse_albedo != cached.diffuse_albedo ||
        entry.reflection_color != cached.reflection_color ||
        entry.reflection_roughness != cached.reflection_roughness ||
        entry.transmission_color != cached.transmission_color ||
        entry.transmission_roughness != cached.transmission_roughness ||
        entry.reflection_ior != cached.reflection_ior ||
        entry.refraction_ior != cached.refraction_ior ||
        entry.packed_thickness != cached.packed_thickness ||
        entry.alpha != cached.alpha ||
        entry.reflection_layer_coverage != cached.reflection_layer_coverage ||
        entry.closure_type != cached.closure_type || entry.proxy_flags != cached.proxy_flags ||
        entry.material_runtime_hash != cached.material_runtime_hash)
    {
      return true;
    }
  }
  return false;
}

/* Indices are in the GPU entry space: `build_hardware_rt_scene_entries` skips null-batch
 * entries, so count only batched entries while walking the host list. */
static Vector<int> hardware_scene_entries_blas_rebuild_indices(
    const Vector<HardwareRaytraceSceneEntry> &entries)
{
  Vector<int> indices;
  int gpu_index = 0;
  for (const int i : entries.index_range()) {
    if (entries[i].batch == nullptr) {
      continue;
    }
    if ((entries[i].recalc & ID_RECALC_GEOMETRY) != 0) {
      indices.append(gpu_index);
    }
    gpu_index++;
  }
  return indices;
}

void RayTraceModule::free_hardware_rt_scene_cache()
{
  GPU_hardware_raytrace_scene_free(hardware_rt_scene_cache_);
  hardware_rt_scene_cache_ = nullptr;
  hardware_rt_scene_stats_cache_ = {};
  hardware_rt_scene_entries_cache_.clear();
  hardware_rt_scene_update_count_ = 0;
  hardware_rt_scene_update_count_valid_ = false;
  hardware_rt_scene_signature_ = 0;
  hardware_rt_scene_signature_valid_ = false;
  hit_eval_resource_ids_valid_ = false;
  hardware_light_records_valid_ = false;
  invalidate_sorted_hardware_scene_entries_cache();
}

void RayTraceModule::invalidate_sorted_hardware_scene_entries_cache()
{
  hardware_sorted_scene_entries_cache_.clear();
  hardware_sorted_scene_entries_update_count_ = 0;
  hardware_sorted_scene_entries_update_count_valid_ = false;
}


const Vector<HardwareRaytraceSceneEntry> &RayTraceModule::current_sorted_hardware_scene_entries(
    const uint64_t depsgraph_update_count)
{
  if (!hardware_sorted_scene_entries_update_count_valid_ ||
      hardware_sorted_scene_entries_update_count_ != depsgraph_update_count)
  {
    hardware_sorted_scene_entries_cache_ = sorted_hardware_scene_entries(
        inst_.sync.hardware_raytrace_scene_entries());
    hardware_sorted_scene_entries_update_count_ = depsgraph_update_count;
    hardware_sorted_scene_entries_update_count_valid_ = true;
  }
  return hardware_sorted_scene_entries_cache_;
}

GPUHardwareRaytraceScene *RayTraceModule::acquire_hardware_rt_scene(
    GPUHardwareRaytraceSceneStats *r_stats)
{
  if (r_stats != nullptr) {
    *r_stats = {};
  }
  const bool perf_logging_enabled = hardware_perf_logging_enabled();
  const double perf_start_time = perf_logging_enabled ? BLI_time_now_seconds() : 0.0;
  const uint64_t current_scene_signature = inst_.sync.hardware_raytrace_scene_signature();

  if (!use_hardware_tracing() || active_hardware_feature_mask() == 0) {
    free_hardware_rt_scene_cache();
    return nullptr;
  }
  const uint64_t depsgraph_update_count = inst_.sync.hardware_raytrace_sync_generation();
  hardware_scene_entry_count_ = int(inst_.sync.hardware_raytrace_scene_entries().size());
  hardware_scene_instance_count_ = hardware_scene_entry_count_;
  if (hardware_rt_scene_cache_ != nullptr && hardware_rt_scene_update_count_valid_ &&
      hardware_rt_scene_update_count_ == depsgraph_update_count &&
      hardware_rt_scene_signature_valid_ &&
      hardware_rt_scene_signature_ == current_scene_signature)
  {
    if (std::getenv("BLENDER_EEVEE_HWRT_CACHE_LOG") != nullptr) {
      std::fprintf(stderr,
                   "EEVEE HWRT scene cache hit update=%llu entries=%d instances=%d\n",
                   (unsigned long long)depsgraph_update_count,
                   hardware_scene_entry_count_,
                   hardware_scene_instance_count_);
    }
    if (perf_logging_enabled) {
      const double elapsed_ms = (BLI_time_now_seconds() - perf_start_time) * 1000.0;
      std::fprintf(stderr,
                   "EEVEE HWRT perf scene_cache=hit entries=%d instances=%d elapsed_ms=%.2f\n",
                   hardware_scene_entry_count_,
                   hardware_scene_instance_count_,
                   elapsed_ms);
    }
    if (r_stats != nullptr) {
      *r_stats = hardware_rt_scene_stats_cache_;
    }
    return hardware_rt_scene_cache_;
  }

  const Vector<HardwareRaytraceSceneEntry> sorted_scene_entries =
      current_sorted_hardware_scene_entries(depsgraph_update_count);
  int geometry_mismatch_index = -1;
  const char *geometry_mismatch_reason = hardware_scene_entries_geometry_mismatch_reason(
      sorted_scene_entries, hardware_rt_scene_entries_cache_, &geometry_mismatch_index);
  const bool geometry_matches_cache = geometry_mismatch_reason == nullptr;
  const Vector<int> blas_rebuild_indices = geometry_matches_cache ?
                                               hardware_scene_entries_blas_rebuild_indices(
                                                   sorted_scene_entries) :
                                               Vector<int>();
  const bool blas_rebuild_requested = !blas_rebuild_indices.is_empty();
  const bool transform_changed = geometry_matches_cache &&
                                 hardware_scene_entries_transform_changed(
                                     sorted_scene_entries, hardware_rt_scene_entries_cache_);
  const bool instance_count_changed = geometry_matches_cache &&
                                      hardware_scene_entries_instance_count_changed(
                                          sorted_scene_entries,
                                          hardware_rt_scene_entries_cache_);
  const bool animation_changed = transform_changed || instance_count_changed;
  const bool emissive_changed = geometry_matches_cache &&
                                hardware_scene_entries_emissive_changed(
                                    sorted_scene_entries, hardware_rt_scene_entries_cache_);
  const bool material_changed = geometry_matches_cache &&
                                hardware_scene_entries_material_proxy_changed(
                                    sorted_scene_entries, hardware_rt_scene_entries_cache_);
  const bool needs_full_rebuild = !geometry_matches_cache;
  const bool cache_logging_enabled = std::getenv("BLENDER_EEVEE_HWRT_CACHE_LOG") != nullptr;

  if (hardware_rt_scene_cache_ != nullptr && !needs_full_rebuild) {
    if (!animation_changed && !emissive_changed && !material_changed && !blas_rebuild_requested) {
      hardware_rt_scene_entries_cache_ = sorted_scene_entries;
      hardware_rt_scene_update_count_ = depsgraph_update_count;
      hardware_rt_scene_update_count_valid_ = true;
      hardware_rt_scene_signature_ = current_scene_signature;
      hardware_rt_scene_signature_valid_ = true;
      if (cache_logging_enabled) {
        std::fprintf(stderr,
                     "EEVEE HWRT scene cache reuse update=%llu entries=%d instances=%d reason=depsgraph_only\n",
                     (unsigned long long)depsgraph_update_count,
                     hardware_scene_entry_count_,
                     hardware_scene_instance_count_);
      }
      if (perf_logging_enabled) {
        const double elapsed_ms = (BLI_time_now_seconds() - perf_start_time) * 1000.0;
        std::fprintf(stderr,
                     "EEVEE HWRT perf scene_cache=reuse entries=%d instances=%d elapsed_ms=%.2f\n",
                     hardware_scene_entry_count_,
                     hardware_scene_instance_count_,
                     elapsed_ms);
      }
      if (r_stats != nullptr) {
        *r_stats = hardware_rt_scene_stats_cache_;
      }
      return hardware_rt_scene_cache_;
    }
    Vector<GPUHardwareRaytraceSceneEntry> rt_scene_entries = build_hardware_rt_scene_entries(
        sorted_scene_entries);
    GPUHardwareRaytraceSceneUpdateParams update_params;
    /* A selective BLAS rebuild moves geometry: the TLAS, the packed world-geometry streams, and
     * the emissive bounding spheres all reference it and must refresh together. */
    update_params.update_tlas = animation_changed || blas_rebuild_requested;
    update_params.update_emissive_data = emissive_changed || animation_changed ||
                                         blas_rebuild_requested;
    update_params.update_material_data = material_changed;
    update_params.update_world_geometry_data = animation_changed || blas_rebuild_requested;
    update_params.rebuild_blas_indices = blas_rebuild_indices;
    if (GPU_hardware_raytrace_scene_update(
            hardware_rt_scene_cache_,
            rt_scene_entries.as_span(),
            update_params,
            &hardware_rt_scene_stats_cache_))
    {
      hardware_rt_scene_entries_cache_ = sorted_scene_entries;
      hardware_rt_scene_update_count_ = depsgraph_update_count;
      hardware_rt_scene_update_count_valid_ = true;
      hardware_rt_scene_signature_ = current_scene_signature;
      hardware_rt_scene_signature_valid_ = true;
      if (cache_logging_enabled) {
        std::fprintf(stderr,
                     "EEVEE HWRT scene cache update update=%llu entries=%d instances=%d tlas=%d emissive=%d material=%d world_geom=%d\n",
                     (unsigned long long)depsgraph_update_count,
                     hardware_scene_entry_count_,
                     hardware_scene_instance_count_,
                     update_params.update_tlas ? 1 : 0,
                     update_params.update_emissive_data ? 1 : 0,
                     update_params.update_material_data ? 1 : 0,
                     update_params.update_world_geometry_data ? 1 : 0);
      }
      if (perf_logging_enabled) {
        const double elapsed_ms = (BLI_time_now_seconds() - perf_start_time) * 1000.0;
        std::fprintf(stderr,
                     "EEVEE HWRT perf scene_cache=update entries=%d instances=%d tlas=%d emissive=%d material=%d world_geom=%d elapsed_ms=%.2f\n",
                     hardware_scene_entry_count_,
                     hardware_scene_instance_count_,
                     update_params.update_tlas ? 1 : 0,
                     update_params.update_emissive_data ? 1 : 0,
                     update_params.update_material_data ? 1 : 0,
                     update_params.update_world_geometry_data ? 1 : 0,
                     elapsed_ms);
      }
      if (r_stats != nullptr) {
        *r_stats = hardware_rt_scene_stats_cache_;
      }
      return hardware_rt_scene_cache_;
    }
  }

  free_hardware_rt_scene_cache();
  hardware_rt_scene_cache_ = build_hardware_rt_scene(
      sorted_scene_entries, &hardware_rt_scene_stats_cache_);
  hardware_rt_scene_entries_cache_ = sorted_scene_entries;
  hardware_rt_scene_update_count_ = depsgraph_update_count;
  hardware_rt_scene_update_count_valid_ = true;
  hardware_rt_scene_signature_ = current_scene_signature;
  hardware_rt_scene_signature_valid_ = true;
  if (cache_logging_enabled) {
    std::fprintf(stderr,
                 "EEVEE HWRT scene cache miss update=%llu entries=%d instances=%d built=%d reason=%s reason_index=%d geometry_match=%d blas_rebuild=%d animation=%d shading=%d\n",
                 (unsigned long long)depsgraph_update_count,
                 hardware_scene_entry_count_,
                 hardware_scene_instance_count_,
                 hardware_rt_scene_stats_cache_.built_scene ? 1 : 0,
                 geometry_mismatch_reason != nullptr ?
                     geometry_mismatch_reason :
                     (blas_rebuild_requested ? "geometry_recalc" : "cold_start"),
                 geometry_mismatch_index,
                 geometry_matches_cache ? 1 : 0,
                 blas_rebuild_requested ? 1 : 0,
                 animation_changed ? 1 : 0,
                 (emissive_changed || material_changed) ? 1 : 0);
  }
  if (perf_logging_enabled) {
    const double elapsed_ms = (BLI_time_now_seconds() - perf_start_time) * 1000.0;
    std::fprintf(stderr,
                 "EEVEE HWRT perf scene_cache=miss entries=%d instances=%d built=%d elapsed_ms=%.2f\n",
                 hardware_scene_entry_count_,
                 hardware_scene_instance_count_,
                 hardware_rt_scene_stats_cache_.built_scene ? 1 : 0,
                 elapsed_ms);
  }
  if (r_stats != nullptr) {
    *r_stats = hardware_rt_scene_stats_cache_;
  }
  return hardware_rt_scene_cache_;
}
void RayTraceModule::submit_hardware_denoise_guides()
{
  PassSimple pass("Trace.OIDNGuides");
  pass.init();
  pass.shader_set(inst_.shaders.static_shader_get(RAY_HIT_DENOISE_GUIDES));
  pass.bind_image("hit_albedo_img", &hardware_hit_.albedo);
  pass.bind_image("hit_normal_img", &hardware_hit_.normal);
  pass.bind_image("denoise_albedo_img", &ray_oidn_albedo_tx_);
  pass.bind_image("denoise_normal_img", &ray_oidn_normal_tx_);
  pass.dispatch(int3(math::divide_ceil(ray_data_tx_.size().xy(), int2(RAYTRACE_GROUP_SIZE)), 1));
  pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_.manager->submit(pass);
}

bool RayTraceModule::submit_hardware_denoise(bool shared)
{
  TextureFromPool &radiance = shared ? shared_indirect_radiance_tx_ : ray_radiance_tx_;
  const int2 extent = radiance.size().xy();
  ray_radiance_oidn_tx_.acquire_2d(extent, gpu::TextureFormat::RAYTRACE_RADIANCE_FORMAT);
  GPUHardwareRaytraceOIDNDenoiseParams params;
  params.input_radiance_tx = radiance;
  params.output_radiance_tx = ray_radiance_oidn_tx_;
  params.albedo_tx = shared ? shared_indirect_albedo_tx_ : ray_oidn_albedo_tx_;
  params.normal_tx = shared ? shared_indirect_normal_tx_ : ray_oidn_normal_tx_;
  params.extent = extent;
  params.use_albedo = true;
  params.use_normal = true;
  /* Match the pinned Nuru baseline: GPU OIDN, balanced quality, fast auxiliary prefilter. */
  const bool log_perf = hardware_perf_logging_enabled();
  const double start = log_perf ? BLI_time_now_seconds() : 0.0;
  GPU_flush();
  const bool submitted = GPU_hardware_raytrace_denoise_oidn(params);
  if (submitted) {
    GPU_texture_copy(radiance, ray_radiance_oidn_tx_);
  }
  else {
    inst_.info_append_i18n("Hardware ray tracing OIDN failed; using Eevee denoising");
  }
  if (log_perf) {
    std::fprintf(stderr,
                 "EEVEE HWRT oidn shared=%d closure=%d extent=%dx%d submitted=%d submit_ms=%.3f\n",
                 int(shared), data_.closure_index, extent.x, extent.y, int(submitted),
                 (BLI_time_now_seconds() - start) * 1000.0);
  }
  return submitted;
}

void RayTraceModule::accumulate_hardware_shared_indirect()
{
  PassSimple pass("Trace.SharedIndirectAccumulate");
  pass.init();
  pass.shader_set(inst_.shaders.static_shader_get(RAY_SHARED_INDIRECT_ACCUM));
  pass.push_constant("closure_index", data_.closure_index);
  pass.bind_resources(inst_.uniform_data);
  pass.bind_resources(inst_.gbuffer);
  pass.bind_ssbo("tiles_coord_buf", &raytrace_tracing_tiles_buf_);
  pass.bind_image("ray_radiance_img", &ray_radiance_tx_);
  pass.bind_image("shared_radiance_img", &shared_indirect_radiance_tx_);
  pass.bind_image("shared_albedo_img", &shared_indirect_albedo_tx_);
  pass.bind_image("shared_normal_img", &shared_indirect_normal_tx_);
  pass.dispatch(raytrace_tracing_dispatch_buf_);
  pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_.manager->submit(pass);
}

RayTraceResultTexture RayTraceModule::resolve_hardware_shared_indirect(RayTraceBuffer &rt_buffer,
                                                                      View &render_view)
{
  const bool denoised = submit_hardware_denoise(true);
  const int2 extent = inst_.film.render_extent_get();
  rt_buffer.shared_indirect_tx.acquire_2d(extent, gpu::TextureFormat::RAYTRACE_RADIANCE_FORMAT);
  PassSimple pass("Trace.SharedIndirectReconstruct");
  pass.init();
  pass.shader_set(inst_.shaders.static_shader_get(RAY_SHARED_INDIRECT_RECONSTRUCT));
  pass.push_constant("active_closure_count", 3);
  pass.push_constant("source_is_oidn", denoised);
  pass.bind_resources(inst_.uniform_data);
  pass.bind_resources(inst_.gbuffer);
  pass.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
  pass.bind_image("shared_radiance_img", &shared_indirect_radiance_tx_);
  pass.bind_image("shared_albedo_img", &shared_indirect_albedo_tx_);
  pass.bind_image("out_radiance_img", &rt_buffer.shared_indirect_tx);
  pass.dispatch(int3(math::divide_ceil(extent, int2(RAYTRACE_GROUP_SIZE)), 1));
  pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_.manager->submit(pass, render_view);
  shared_indirect_radiance_tx_.release();
  shared_indirect_albedo_tx_.release();
  shared_indirect_normal_tx_.release();
  ray_radiance_oidn_tx_.release();
  return {rt_buffer.shared_indirect_tx};
}

void RayTraceModule::render_scene_final_specular(RayTraceBuffer &rt_buffer,
                                                gpu::Texture *scene_radiance_tx,
                                                eClosureBits active_closures,
                                                View &render_view)
{
  RayTraceResult result = render_phase(rt_buffer,
                                        scene_radiance_tx,
                                        (active_closures & CLOSURE_REFRACTION) ? scene_radiance_tx :
                                                                                nullptr,
                                        active_closures,
                                        render_view,
                                        HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR);
  PassSimple pass("Trace.SceneFinalSpecular");
  pass.init();
  pass.shader_set(inst_.shaders.static_shader_get(RAY_SCENE_FINAL_SPECULAR));
  pass.push_constant("feature_mask", int(hardware_feature_mask_));
  pass.push_constant("use_shared_indirect", result.use_shared_indirect);
  pass.bind_texture("specular_0_tx", &result.closures[0]);
  pass.bind_texture("specular_1_tx", &result.closures[1]);
  pass.bind_texture("specular_2_tx", &result.closures[2]);
  pass.bind_texture("shared_specular_tx", result.use_shared_indirect ?
                        &result.shared_indirect : &result.closures[0]);
  pass.bind_image("combined_img", &inst_.render_buffers.combined_tx);
  pass.bind_image("rp_color_img", &inst_.render_buffers.rp_color_tx);
  pass.bind_resources(inst_.gbuffer);
  pass.bind_resources(inst_.uniform_data);
  pass.dispatch(int3(math::divide_ceil(data_.full_resolution, int2(RAYTRACE_GROUP_SIZE)), 1));
  pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_.manager->submit(pass, render_view);
  result.release();
  if (hardware_perf_logging_enabled()) {
    std::fprintf(stderr, "EEVEE HWRT scene_final_specular features=0x%x\n",
                 unsigned(current_hardware_feature_mask_));
  }
}

void RayTraceModule::merge_specular_result(RayTraceBuffer &rt_buffer,
                                           RayTraceResult &result,
                                           RayTraceResult &specular,
                                           View &render_view)
{
  BLI_assert(result.use_shared_indirect == specular.use_shared_indirect);
  /* Do not write the specular result into the precombine temporal history. The next frame's
   * precombine pass must not blend yesterday's reflection into this frame's diffuse lighting. */
  for (int bin = 0; bin < 3; bin++) {
    gpu::Texture *source = result.closures[bin];
    rt_buffer.merged_indirect_tx[bin].acquire_2d(
        int2(GPU_texture_width(source), GPU_texture_height(source)), GPU_texture_format(source));
    GPU_texture_copy(rt_buffer.merged_indirect_tx[bin], source);
    result.closures[bin].release();
    result.closures[bin] = {rt_buffer.merged_indirect_tx[bin]};
  }
  PassSimple pass("Trace.MergeSpecular");
  pass.init();
  pass.shader_set(inst_.shaders.static_shader_get(RAY_MERGE_SPECULAR));
  pass.push_constant("feature_mask", int(hardware_feature_mask_));
  pass.push_constant("use_shared_indirect", result.use_shared_indirect);
  pass.bind_image("indirect_0_img", &result.closures[0]);
  pass.bind_image("indirect_1_img", &result.closures[1]);
  pass.bind_image("indirect_2_img", &result.closures[2]);
  pass.bind_texture("specular_0_tx", &specular.closures[0]);
  pass.bind_texture("specular_1_tx", &specular.closures[1]);
  pass.bind_texture("specular_2_tx", &specular.closures[2]);
  pass.bind_image("shared_indirect_img", result.use_shared_indirect ?
                      &result.shared_indirect : &result.closures[0]);
  pass.bind_texture("shared_specular_tx", specular.use_shared_indirect ?
                        &specular.shared_indirect : &specular.closures[0]);
  pass.bind_resources(inst_.gbuffer);
  pass.dispatch(int3(math::divide_ceil(data_.full_resolution, int2(RAYTRACE_GROUP_SIZE)), 1));
  pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_.manager->submit(pass, render_view);
}

void RayTraceModule::render_hardware_shadows(View &view, gpu::Texture *depth_tx, int2 extent)
{
  hardware_shadow_ready_ = false;
  const int lights_len = inst_.lights.light_count();
  if (!use_hardware_shadows() || lights_len == 0 ||
      inst_.sync.hardware_raytrace_scene_entries().is_empty())
  {
    const float visibility[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    hardware_shadow_visibility_tx_.ensure_2d_array(
        gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), 1,
        GPU_TEXTURE_USAGE_SHADER_READ, visibility);
    return;
  }

  GPUHardwareRaytraceSceneStats stats;
  GPUHardwareRaytraceScene *scene = acquire_hardware_rt_scene(&stats);
  if (!scene || !stats.built_scene) {
    return;
  }

  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  hardware_shadow_visibility_tx_.ensure_2d_array(
      gpu::TextureFormat::SFLOAT_16_16_16_16, extent, lights_len, usage);
  hardware_shadow_visibility_tx_.clear(float4(1.0f));
  GPU_flush();

  const SceneEEVEE &options = inst_.scene->eevee;
  const int sample_count = clamp_i(options.hardware_raytracing_shadow_samples, 1, 16);
  const float3 rng = inst_.sampling.rng_3d_get(SAMPLING_SHADOW_U);
  const float4 sampling_rand(rng, inst_.sampling.rng_get(SAMPLING_SHADOW_X));
  const bool log_perf = hardware_perf_logging_enabled();
  const double start = log_perf ? BLI_time_now_seconds() : 0.0;
  bool success = true;
  int traced = 0;
  GPU_debug_group_begin("Hardware RT Shadows");
  const bool batched = GPU_hardware_raytrace_scene_shadow_batch_begin(scene);
  for (const int i : IndexRange(lights_len)) {
    const LightData &light = inst_.lights.sync_light(i);
    if (light.hwrt_shadow_layer < 0) {
      continue;
    }
    if (is_sun_light(light.type)) {
      GPUHardwareRaytraceDirectionalShadowParams p;
      p.depth_tx = depth_tx;
      p.gbuf_header_tx = inst_.gbuffer.header_tx;
      p.gbuf_normal_tx = inst_.gbuffer.normal_tx;
      p.shadow_visibility_tx = hardware_shadow_visibility_tx_;
      p.viewinv = view.viewinv();
      p.wininv = view.wininv();
      p.full_resolution = extent;
      p.shadow_layer = light.hwrt_shadow_layer;
      p.light_direction = math::normalize(float3(light.sun().direction));
      p.normal_bias = std::max(5.0e-3f, light.filter_radius * 5.0e-3f);
      p.shadow_angle = light.sun().shadow_angle;
      p.sample_count = sample_count;
      p.color_intensity = options.hardware_raytracing_shadow_color_intensity;
      p.shadow_transparency = options.hardware_raytracing_shadow_transparency;
      p.use_caustics = false;
      p.photons_intensity = 0.0f;
      p.sampling_rand = sampling_rand;
      success &= GPU_hardware_raytrace_scene_trace_directional_shadow(scene, p);
    }
    else {
      GPUHardwareRaytraceLocalShadowParams p;
      p.depth_tx = depth_tx;
      p.gbuf_header_tx = inst_.gbuffer.header_tx;
      p.gbuf_normal_tx = inst_.gbuffer.normal_tx;
      p.shadow_visibility_tx = hardware_shadow_visibility_tx_;
      p.viewinv = view.viewinv();
      p.wininv = view.wininv();
      p.full_resolution = extent;
      p.shadow_layer = light.hwrt_shadow_layer;
      p.light_type = uint32_t(light.type);
      p.light_position = light.position();
      p.shadow_radius = light.local().local.shadow_radius;
      p.light_x_axis = light.x_axis();
      p.light_y_axis = light.y_axis();
      p.shadow_offset = light.x_axis() * light.local().local.shadow_position.x +
                        light.y_axis() * light.local().local.shadow_position.y +
                        light.z_axis() * light.local().local.shadow_position.z;
      if (is_area_light(light.type)) {
        p.area_size_x = light.area().size.x;
        p.area_size_y = light.area().size.y;
        p.area_shadow_scale = light.area().shadow_scale;
      }
      p.normal_bias = std::max(4.0e-3f, light.filter_radius * 4.0e-3f);
      p.sample_count = sample_count;
      p.color_intensity = options.hardware_raytracing_shadow_color_intensity;
      p.shadow_transparency = options.hardware_raytracing_shadow_transparency;
      p.use_caustics = false;
      p.photons_intensity = 0.0f;
      p.sampling_rand = sampling_rand;
      success &= GPU_hardware_raytrace_scene_trace_local_shadow(scene, p);
    }
    traced++;
  }
  if (batched) {
    success &= GPU_hardware_raytrace_scene_shadow_batch_end(scene);
  }
  GPU_debug_group_end();
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  hardware_shadow_ready_ = success;
  if (!success) {
    inst_.info_append_i18n("Hardware ray-traced shadows failed; using shadow maps");
  }
  if (log_perf) {
    std::fprintf(stderr,
                 "EEVEE HWRT primary_shadows lights=%d samples=%d success=%d submit_ms=%.3f\n",
                 traced, sample_count, int(success), (BLI_time_now_seconds() - start) * 1000.0);
  }
}

void RayTraceModule::render_hardware_direct_light(View &view, gpu::Texture *depth_tx, int2 extent)
{
  hardware_direct_ready_ = false;
  hardware_direct_replace_ = false;
  data_.hardware_nis_enable = false;
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  /* Nuru's viewport-reference path also includes image renders: GI consumes the resolved
   * direct light in both. Only primary viewport shading replaces the native evaluator. */
  if ((!inst_.is_viewport() && !inst_.is_image_render) || !hardware_shadow_ready_ ||
      std::getenv("BLENDER_EEVEE_HWRT_NATIVE_DIRECT") != nullptr)
  {
    hardware_direct_closures_tx_.ensure_2d_array(
        gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), 6, usage);
    hardware_direct_closures_tx_.clear(float4(0.0f));
    hardware_direct_accum_tx_.free();
    hardware_direct_combined_tx_.free();
    hardware_direct_depth_tx_.free();
    hardware_direct_tilemask_tx_.free();
    return;
  }

  const LightCullingData &culling = inst_.lights.culling_data();
  const int tile_count = int(culling.tile_x_len * culling.tile_y_len);
  if (tile_count == 0) {
    return;
  }
  const int capacity = int(ceil_to_multiple_u(tile_count, 512));
  hardware_direct_tiles_buf_.resize(capacity);
  hardware_direct_samples_buf_.resize(capacity);
  hardware_direct_dispatch_buf_.clear_to_zero();
  /* The trainer is dispatched to allocation capacity, not just the compacted tile count. */
  hardware_direct_samples_buf_.clear_to_zero();
  hardware_direct_closures_tx_.ensure_2d_array(
      gpu::TextureFormat::SFLOAT_16_16_16_16, extent, 6, usage);
  hardware_direct_accum_tx_.ensure_2d(gpu::TextureFormat::RAYTRACE_RADIANCE_FORMAT, extent, usage);
  hardware_direct_combined_tx_.ensure_2d(gpu::TextureFormat::RAYTRACE_RADIANCE_FORMAT, extent, usage);
  hardware_direct_depth_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32, extent, usage);
  hardware_direct_tilemask_tx_.ensure_2d(gpu::TextureFormat::RAYTRACE_TILEMASK_FORMAT,
                                       int2(culling.tile_x_len, culling.tile_y_len), usage);
  hardware_direct_closures_tx_.clear(float4(0.0f));
  hardware_direct_accum_tx_.clear(float4(0.0f));
  hardware_direct_combined_tx_.clear(float4(0.0f));
  hardware_direct_depth_tx_.clear(float4(0.0f));
  hardware_direct_tilemask_tx_.clear(uint4(0));

  constexpr int nis_param_count = 24 * 32 + 32 + 32 * 32 + 32 + 32 * 32 + 32;
  if (!hardware_nis_initialized_) {
    hardware_nis_weights_buf_.resize(nis_param_count);
    hardware_nis_grads_buf_.resize(nis_param_count);
    hardware_nis_m_buf_.resize(nis_param_count);
    hardware_nis_v_buf_.resize(nis_param_count);
    uint rng_state = 0x9E3779B9u;
    for (int i = 0; i < nis_param_count; i++) {
      float weight = 0.0f;
      if (i < 24 * 32 || (i >= 24 * 32 + 32 && i < 24 * 32 + 32 + 32 * 32)) {
        rng_state = rng_state * 1664525u + 1013904223u;
        weight = ((float(rng_state >> 8u) / float(1u << 24u)) * 2.0f - 1.0f) *
                 (i < 24 * 32 ? 0.25f : 0.18f);
      }
      hardware_nis_weights_buf_[i] = weight;
      hardware_nis_grads_buf_[i] = 0;
      hardware_nis_m_buf_[i] = hardware_nis_v_buf_[i] = 0.0f;
    }
    for (int i = 0; i < 4; i++) {
      hardware_nis_count_buf_[i] = 0u;
    }
    for (int i = 0; i < HWRT_LIGHT_CLUSTER_COUNT; i++) {
      hardware_cluster_weights_buf_[i] = 1.0f;
    }
    hardware_cluster_weights_buf_.push_update();
    hardware_nis_weights_buf_.push_update();
    hardware_nis_grads_buf_.push_update();
    hardware_nis_count_buf_.push_update();
    hardware_nis_m_buf_.push_update();
    hardware_nis_v_buf_.push_update();
    hardware_nis_initialized_ = true;
  }

  data_.full_resolution = extent;
  data_.full_resolution_inv = 1.0f / float2(extent);
  data_.hardware_nis_enable = true;
  HardwareDirectLightData &direct = data_.hardware_direct_light;
  direct = {};
  direct.tile_size_px = uint(max_ff(culling.tile_size, 1.0f));
  direct.tile_word_len = culling.tile_word_len;
  direct.candidate_local_lights_len = direct.local_lights_len = culling.local_lights_len;
  direct.sun_lights_len = culling.sun_lights_len;
  direct.light_samples_per_shading_point = 2; /* Pinned balanced viewport-reference budget. */
  direct.trace_sun_lights_separately = culling.sun_lights_len > 0;
  direct.local_light_importance_scale = 1.0f;
  direct.area_light_importance_scale = 1.35f;
  direct.textured_light_importance_scale = 1.5f;
  direct.sun_light_importance_scale = 2.0f;
  inst_.uniform_data.raytrace.push_update();

  PassSimple pass("HardwareDirectLight");
  auto setup = [&](eShaderType type) {
    pass.init();
    gpu::Shader *shader = inst_.shaders.static_shader_get(type);
    if (type == RAY_HW_DIRECT_ACCUM || type == RAY_HW_DIRECT_DENOISE) {
      pass.specialize_constant(shader, "use_hardware_rt_shadows", true);
    }
    pass.shader_set(shader);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.lights);
    pass.bind_resources(inst_.sampling);
    pass.push_constant("hardware_direct_light_tile_capacity", capacity);
  };
  auto bind_selection = [&]() {
    pass.bind_ssbo("hardware_light_cluster_weight_buf", &hardware_cluster_weights_buf_);
    pass.bind_ssbo("hardware_nis_weights_buf", &hardware_nis_weights_buf_);
    pass.bind_ssbo("hardware_direct_light_work_tiles_buf", &hardware_direct_tiles_buf_);
    pass.bind_ssbo("hardware_direct_light_visibility_samples_buf", &hardware_direct_samples_buf_);
    pass.bind_texture("depth_tx", depth_tx);
  };
  auto bind_evaluation = [&]() {
    pass.bind_resources(inst_.gbuffer);
    pass.bind_resources(inst_.shadows);
    pass.bind_resources(inst_.hiz_buffer.front);
    inst_.lights.bind_light_shader_resources(pass);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_texture(NPR_SHADOW_OBJECT_ID_TEX_SLOT, &inst_.render_buffers.object_id_tx);
    pass.bind_texture(NPR_SHADOW_NORMAL_TEX_SLOT, &inst_.render_buffers.prepass_normal_tx);
    pass.bind_texture("hardware_shadow_visibility_tx", &hardware_shadow_visibility_tx_);
  };
  auto submit_indirect = [&]() {
    pass.dispatch(hardware_direct_dispatch_buf_);
    pass.barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_SHADER_IMAGE_ACCESS |
                 GPU_BARRIER_TEXTURE_FETCH);
    inst_.manager->submit(pass, view);
  };

  setup(RAY_HW_DIRECT_TILE_COMPACT);
  pass.bind_ssbo("hardware_direct_light_dispatch_buf", &hardware_direct_dispatch_buf_);
  pass.bind_ssbo("hardware_direct_light_work_tiles_buf", &hardware_direct_tiles_buf_);
  pass.dispatch(int3(divide_ceil_u(tile_count, 64), 1, 1));
  pass.barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_COMMAND);
  inst_.manager->submit(pass, view);

  setup(RAY_HW_DIRECT_VISIBILITY);
  bind_selection();
  pass.bind_texture("hardware_rt_shadow_visibility_tx", &hardware_shadow_visibility_tx_);
  submit_indirect();

  setup(RAY_HW_DIRECT_ACCUM);
  bind_selection();
  bind_evaluation();
  pass.bind_image("out_direct_light_accum_img", &hardware_direct_accum_tx_);
  submit_indirect();

  setup(RAY_HW_NIS_TRAIN);
  bind_selection();
  pass.push_constant("hardware_nis_train_stride", 4);
  pass.push_constant("hardware_nis_train_offset", hardware_nis_train_offset_);
  hardware_nis_train_offset_ = (hardware_nis_train_offset_ + 1) % 4;
  pass.bind_ssbo("hardware_nis_grads_buf", &hardware_nis_grads_buf_);
  pass.bind_ssbo("hardware_nis_train_count_buf", &hardware_nis_count_buf_);
  pass.dispatch(int3(divide_ceil_u(capacity, 64), 1, 1));
  pass.barrier(GPU_BARRIER_SHADER_STORAGE);
  inst_.manager->submit(pass, view);

  /* Counter reset must follow all parameter workgroups, not race them in thread zero. */
  for (bool finalize : {false, true}) {
    pass.init();
    pass.shader_set(inst_.shaders.static_shader_get(RAY_HW_NIS_ADAM));
    pass.push_constant("hardware_nis_param_count", nis_param_count);
    pass.push_constant("hardware_nis_finalize", finalize);
    pass.bind_ssbo("hardware_nis_weights_buf", &hardware_nis_weights_buf_);
    pass.bind_ssbo("hardware_nis_grads_buf", &hardware_nis_grads_buf_);
    pass.bind_ssbo("hardware_nis_train_count_buf", &hardware_nis_count_buf_);
    pass.bind_ssbo("hardware_nis_adam_m_buf", &hardware_nis_m_buf_);
    pass.bind_ssbo("hardware_nis_adam_v_buf", &hardware_nis_v_buf_);
    pass.dispatch(int3(finalize ? 1 : divide_ceil_u(nis_param_count, 64), 1, 1));
    pass.barrier(GPU_BARRIER_SHADER_STORAGE);
    inst_.manager->submit(pass, view);
  }

  setup(RAY_HW_DIRECT_DENOISE);
  pass.bind_texture("depth_tx", depth_tx);
  pass.bind_texture("hardware_rt_shadow_visibility_tx", &hardware_shadow_visibility_tx_);
  pass.bind_ssbo("hardware_direct_light_work_tiles_buf", &hardware_direct_tiles_buf_);
  pass.bind_ssbo("hardware_direct_light_visibility_samples_buf", &hardware_direct_samples_buf_);
  bind_evaluation();
  pass.bind_image("in_direct_light_accum_img", &hardware_direct_accum_tx_);
  pass.bind_image("out_direct_light_denoised_img", &hardware_direct_combined_tx_);
  pass.bind_image("out_direct_light_depth_img", &hardware_direct_depth_tx_);
  pass.bind_image("direct_light_tilemask_img", &hardware_direct_tilemask_tx_);
  pass.bind_image("out_direct_light_closures_img", &hardware_direct_closures_tx_);
  submit_indirect();
  hardware_direct_ready_ = true;
  hardware_direct_replace_ = inst_.is_viewport();
  if (hardware_perf_logging_enabled()) {
    std::fprintf(stderr, "EEVEE HWRT primary_direct local=%u sun=%u tiles=%d samples=%u\n",
                 direct.local_lights_len, direct.sun_lights_len, tile_count,
                 direct.light_samples_per_shading_point);
  }
}

void RayTraceModule::render_hardware_hit_shadows(GPUHardwareRaytraceScene *scene,
                                                 HardwareHitBuffers &hit)
{
  const int2 extent = ray_data_tx_.size().xy();
  const int lights_len = inst_.lights.light_count();
  hit.shadows_valid = false;
  /* The Shadows toggle selects the primary-view shadow method. Traced receivers can be
   * outside the camera's VSM allocation, so their visibility must still be queried when
   * reflection/refraction/GI is enabled on its own. Falling back to an unallocated VSM tile
   * incorrectly turns a lit off-screen receiver black. */
  if (lights_len == 0) {
    hit.shadow_visibility.acquire_2d_array(
        int2(1), 1, gpu::TextureFormat::SFLOAT_16_16_16_16);
    hit.shadow_visibility.clear(float4(1.0f));
    return;
  }
  eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  if (std::getenv("BLENDER_EEVEE_HWRT_HIT_READBACK") != nullptr) {
    usage |= GPU_TEXTURE_USAGE_HOST_READ;
  }
  hit.shadow_visibility.acquire_2d_array(
      extent, lights_len, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
  hit.shadow_visibility.clear(float4(1.0f));
  GPU_flush();
  const SceneEEVEE &options = inst_.scene->eevee;
  const int sample_count = clamp_i(options.hardware_raytracing_shadow_samples, 1, 16);
  const float3 rng = inst_.sampling.rng_3d_get(SAMPLING_SHADOW_U);
  const float4 sampling_rand(rng, inst_.sampling.rng_get(SAMPLING_SHADOW_X));
  const bool log_perf = hardware_perf_logging_enabled();
  const double start = log_perf ? BLI_time_now_seconds() : 0.0;
  bool success = true;
  int traced = 0;
  GPU_debug_group_begin("Hardware RT Hit Shadows");
  const bool batched = GPU_hardware_raytrace_scene_shadow_batch_begin(scene);
  for (const int i : IndexRange(lights_len)) {
    const LightData &light = inst_.lights.sync_light(i);
    if (light.hwrt_shadow_layer < 0) {
      continue;
    }
    if (is_sun_light(light.type)) {
      GPUHardwareRaytraceDirectionalHitShadowParams p;
      p.hit_normal_tx = hit.normal;
      p.hit_world_position_tx = hit.world_position;
      p.hit_identity_tx = hit.identity;
      p.dispatch_buf = raytrace_tracing_dispatch_buf_;
      p.tiles_coord_buf = raytrace_tracing_tiles_buf_;
      p.tracing_resolution = extent;
      p.shadow_visibility_tx = hit.shadow_visibility;
      p.shadow_layer = light.hwrt_shadow_layer;
      p.light_direction = math::normalize(float3(light.sun().direction));
      p.normal_bias = std::max(5.0e-3f, light.filter_radius * 5.0e-3f);
      p.shadow_angle = light.sun().shadow_angle;
      p.sample_count = sample_count;
      p.color_intensity = options.hardware_raytracing_shadow_color_intensity;
      p.shadow_transparency = options.hardware_raytracing_shadow_transparency;
      p.use_caustics = false;
      p.photons_intensity = 0.0f;
      p.sampling_rand = sampling_rand;
      success &= GPU_hardware_raytrace_scene_trace_directional_hit_shadow(scene, p);
    }
    else {
      GPUHardwareRaytraceLocalHitShadowParams p;
      p.hit_normal_tx = hit.normal;
      p.hit_world_position_tx = hit.world_position;
      p.hit_identity_tx = hit.identity;
      p.dispatch_buf = raytrace_tracing_dispatch_buf_;
      p.tiles_coord_buf = raytrace_tracing_tiles_buf_;
      p.tracing_resolution = extent;
      p.shadow_visibility_tx = hit.shadow_visibility;
      p.shadow_layer = light.hwrt_shadow_layer;
      p.light_type = uint32_t(light.type);
      p.light_position = light.position();
      p.shadow_radius = light.local().local.shadow_radius;
      p.light_x_axis = light.x_axis();
      p.light_y_axis = light.y_axis();
      p.shadow_offset = light.x_axis() * light.local().local.shadow_position.x +
                        light.y_axis() * light.local().local.shadow_position.y +
                        light.z_axis() * light.local().local.shadow_position.z;
      if (is_area_light(light.type)) {
        p.area_size_x = light.area().size.x;
        p.area_size_y = light.area().size.y;
        p.area_shadow_scale = light.area().shadow_scale;
      }
      p.normal_bias = std::max(4.0e-3f, light.filter_radius * 4.0e-3f);
      p.sample_count = sample_count;
      p.color_intensity = options.hardware_raytracing_shadow_color_intensity;
      p.shadow_transparency = options.hardware_raytracing_shadow_transparency;
      p.use_caustics = false;
      p.photons_intensity = 0.0f;
      p.sampling_rand = sampling_rand;
      success &= GPU_hardware_raytrace_scene_trace_local_hit_shadow(scene, p);
    }
    traced++;
  }
  if (batched) {
    success &= GPU_hardware_raytrace_scene_shadow_batch_end(scene);
  }
  GPU_debug_group_end();
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);

  hit.shadows_valid = success;
  if (!success) {
    inst_.info_append_i18n("Hardware hit shadows failed; using shadow maps");
  }
  if (log_perf) {
    std::fprintf(stderr,
                 "EEVEE HWRT hit_shadows lights=%d samples=%d success=%d submit_ms=%.3f\n",
                 traced, sample_count, int(success), (BLI_time_now_seconds() - start) * 1000.0);
  }
  if (std::getenv("BLENDER_EEVEE_HWRT_HIT_READBACK") != nullptr) {
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
    float *time = static_cast<float *>(GPU_texture_read(hit.time, GPU_DATA_FLOAT, 0));
    float4 *visibility = static_cast<float4 *>(
        GPU_texture_read(hit.shadow_visibility, GPU_DATA_FLOAT, 0));
    const int pixel_count = extent.x * extent.y;
    for (int layer = 0; layer < lights_len; layer++) {
      if (inst_.lights.sync_light(layer).hwrt_shadow_layer < 0) {
        continue; /* World-extracted lights still use VSM, not this visibility array. */
      }
      int hits = 0;
      float3 sum(0.0f), minimum(1.0f), maximum(0.0f);
      for (int pixel = 0; pixel < pixel_count; pixel++) {
        if (!(time[pixel] > 0.0f)) {
          continue;
        }
        const float3 value = visibility[layer * pixel_count + pixel].xyz();
        sum += value;
        minimum = math::min(minimum, value);
        maximum = math::max(maximum, value);
        hits++;
      }
      if (hits > 0) {
        sum /= float(hits);
        std::fprintf(stderr, "EEVEE HWRT hit_shadow_readback layer=%d hits=%d "
                             "avg=(%.6f,%.6f,%.6f) min=(%.6f,%.6f,%.6f) max=(%.6f,%.6f,%.6f)\n",
                     layer, hits, UNPACK3(sum), UNPACK3(minimum), UNPACK3(maximum));
      }
    }
    MEM_delete_void(static_cast<void *>(time));
    MEM_delete_void(static_cast<void *>(visibility));
  }
}

void RayTraceModule::HardwareHitBuffers::acquire(int2 extent)
{
  eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  if (std::getenv("BLENDER_EEVEE_HWRT_HIT_READBACK") != nullptr) {
    usage |= GPU_TEXTURE_USAGE_HOST_READ;
  }
  time.acquire_2d(extent, gpu::TextureFormat::RAYTRACE_RAYTIME_FORMAT, usage);
  radiance.acquire_2d(extent, gpu::TextureFormat::RAYTRACE_RADIANCE_FORMAT, usage);
  identity.acquire_2d(extent, gpu::TextureFormat::UINT_32_32_32_32, usage);
  albedo.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
  throughput.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
  material.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
  normal.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
  position.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
  /* Shadow rays need the actual hit position. Half precision snaps hits onto adjacent
   * wall edges and requires a large bias that then skips those same blockers. */
  world_position.acquire_2d(extent, gpu::TextureFormat::SFLOAT_32_32_32_32, usage);
  barycentric.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
  emission_replay.acquire_2d(extent, gpu::TextureFormat::SFLOAT_32_32_32_32, usage);
  time.clear(float4(0.0f));
  radiance.clear(float4(0.0f));
  identity.clear(uint4(0u, 0u, 0u, UINT32_MAX));
  albedo.clear(float4(0.0f));
  throughput.clear(float4(0.0f));
  material.clear(float4(0.0f));
  normal.clear(float4(0.0f));
  position.clear(float4(0.0f));
  world_position.clear(float4(0.0f));
  barycentric.clear(float4(0.0f));
  emission_replay.clear(float4(0.0f));
}

void RayTraceModule::HardwareHitBuffers::release()
{
  receiver_gi.release();
  shadow_visibility.release();
  shadows_valid = false;
  time.release();
  radiance.release();
  identity.release();
  albedo.release();
  throughput.release();
  material.release();
  normal.release();
  position.release();
  world_position.release();
  barycentric.release();
  emission_replay.release();
}

RayTraceModule::HardwareHitEvalPayload RayTraceModule::HardwareHitBuffers::payload()
{
  return {time, radiance, albedo, throughput, material, normal, position,
          world_position, identity, barycentric, emission_replay};
}

void RayTraceModule::release_hardware_hit_buffers()
{
  hardware_hit_.release();
  hardware_layered_hit_.release();
  hardware_transmission_hit_.release();
  hardware_screen_continuation_tx_.release();
}

static GPUHardwareRaytraceFastGILightRecord hardware_fast_gi_light_record_from_light(
    const LightData &light)
{
  GPUHardwareRaytraceFastGILightRecord record;
  record.object_to_world_x = light.object_to_world.x;
  record.object_to_world_y = light.object_to_world.y;
  record.object_to_world_z = light.object_to_world.z;
  record.color_diffuse_power = float4(
      std::abs(light.color.x),
      std::abs(light.color.y),
      std::abs(light.color.z),
      light.power[LIGHT_DIFFUSE]);
  record.direction_type = float4(0.0f, 0.0f, 1.0f, float(light.type));
  /* Nuru NIS: stable cluster id rides the spare spot lane for kernel-side multiplier lookup
   * (tree nodes carry their representative leaf's cluster in the same lane). */
  record.spot_size_inv.z = 0.0f; /* Neutral NIS: no learned cluster weights. */
  if (is_sun_light(light.type)) {
    record.direction_type = float4(light.sun().direction, float(light.type));
    record.attenuation_spot.x = light.sun().shape_radius;
  }
  else {
    record.attenuation_spot.x = light.local().local.shape_radius;
    record.attenuation_spot.y = light.local().local.influence_radius_invsqr_surface;
    if (is_spot_light(light.type)) {
      record.attenuation_spot.z = light.spot().spot_mul;
      record.attenuation_spot.w = light.spot().spot_bias;
      record.spot_size_inv = float4(
          light.spot().spot_size_inv.x, light.spot().spot_size_inv.y, 0.0f, 0.0f);
    }
  }
  return record;
}


bool RayTraceModule::submit_hardware_tracing(View &render_view, int trace_phase)
{
  GPUHardwareRaytraceSceneStats stats;
  GPUHardwareRaytraceScene *scene = acquire_hardware_rt_scene(&stats);
  if (!scene || !stats.built_scene) {
    return false;
  }
  const int2 extent = ray_data_tx_.size().xy();
  hardware_hit_.acquire(extent);
  hardware_layered_hit_.acquire(extent);
  hardware_transmission_hit_.acquire(extent);
  GPU_texture_copy(hardware_hit_.time, ray_time_tx_);
  GPU_texture_copy(hardware_hit_.radiance, ray_radiance_tx_);
  hardware_screen_continuation_tx_.acquire_2d(
      extent, gpu::TextureFormat::SFLOAT_16_16_16_16,
      GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE);
  hardware_screen_continuation_tx_.clear(float4(0.0f));

  GPUHardwareRaytraceTraceParams p;
  p.secondary_gi = trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR &&
                   (hardware_feature_mask_ & RAYTRACE_EEVEE_HARDWARE_SECONDARY_GI) != 0;
  p.secondary_gi_samples = inst_.scene->eevee.hardware_raytracing_secondary_gi_samples;
  for (HardwareHitBuffers *hit :
       {&hardware_hit_, &hardware_layered_hit_, &hardware_transmission_hit_})
  {
    if (p.secondary_gi) {
      hit->receiver_gi.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16_16_16,
                                GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE);
      hit->receiver_gi.clear(float4(0.0f));
    }
    else {
      hit->receiver_gi.release();
    }
  }
  p.reflected_receiver_gi_tx = hardware_hit_.receiver_gi;
  p.layered_receiver_gi_tx = hardware_layered_hit_.receiver_gi;
  p.transmission_receiver_gi_tx = hardware_transmission_hit_.receiver_gi;
  p.ray_data_tx = ray_data_tx_;
  p.depth_tx = renderbuf_depth_view_;
  p.gbuf_header_tx = inst_.gbuffer.header_tx;
  p.gbuf_normal_tx = inst_.gbuffer.normal_tx;
  p.screen_continuation_tx = hardware_screen_continuation_tx_;
  p.world_probe_tx = inst_.sphere_probes.octahedral_probes_texture();
  p.ray_time_tx = hardware_hit_.time;
  p.ray_radiance_tx = hardware_hit_.radiance;
  p.hit_albedo_tx = hardware_hit_.albedo;
  p.hit_throughput_tx = hardware_hit_.throughput;
  p.hit_emission_replay_tx = hardware_hit_.emission_replay;
  p.hit_material_tx = hardware_hit_.material;
  p.hit_normal_tx = hardware_hit_.normal;
  p.hit_position_tx = hardware_hit_.position;
  p.hit_world_position_tx = hardware_hit_.world_position;
  p.hit_identity_tx = hardware_hit_.identity;
  p.hit_barycentric_tx = hardware_hit_.barycentric;
  p.layered_receiver_ray_time_tx = hardware_layered_hit_.time;
  p.layered_receiver_ray_radiance_tx = hardware_layered_hit_.radiance;
  p.layered_receiver_albedo_tx = hardware_layered_hit_.albedo;
  p.layered_receiver_throughput_tx = hardware_layered_hit_.throughput;
  p.layered_receiver_emission_replay_tx = hardware_layered_hit_.emission_replay;
  p.layered_receiver_material_tx = hardware_layered_hit_.material;
  p.layered_receiver_normal_tx = hardware_layered_hit_.normal;
  p.layered_receiver_position_tx = hardware_layered_hit_.position;
  p.layered_receiver_world_position_tx = hardware_layered_hit_.world_position;
  p.layered_receiver_identity_tx = hardware_layered_hit_.identity;
  p.layered_receiver_barycentric_tx = hardware_layered_hit_.barycentric;
  p.transmission_receiver_ray_time_tx = hardware_transmission_hit_.time;
  p.transmission_receiver_ray_radiance_tx = hardware_transmission_hit_.radiance;
  p.transmission_receiver_albedo_tx = hardware_transmission_hit_.albedo;
  p.transmission_receiver_throughput_tx = hardware_transmission_hit_.throughput;
  p.transmission_receiver_emission_replay_tx = hardware_transmission_hit_.emission_replay;
  p.transmission_receiver_material_tx = hardware_transmission_hit_.material;
  p.transmission_receiver_normal_tx = hardware_transmission_hit_.normal;
  p.transmission_receiver_position_tx = hardware_transmission_hit_.position;
  p.transmission_receiver_world_position_tx = hardware_transmission_hit_.world_position;
  p.transmission_receiver_identity_tx = hardware_transmission_hit_.identity;
  p.transmission_receiver_barycentric_tx = hardware_transmission_hit_.barycentric;
  p.viewinv = render_view.viewinv();
  p.wininv = render_view.wininv();
  p.full_resolution = data_.full_resolution;
  p.resolution_scale = data_.trace_pixel_scale;
  p.resolution_bias = data_.trace_pixel_offset;
  p.closure_index = data_.closure_index;
  /* Vulkan gather uses these compact records for sun NEE; hit lighting owns local lights. */
  const uint64_t light_generation = inst_.sync.hardware_raytrace_sync_generation();
  if (!hardware_light_records_valid_ || hardware_light_records_generation_ != light_generation) {
    Vector<LightData> local_lights;
    Vector<LightData> sun_lights;
    for (const int index : IndexRange(inst_.lights.light_count())) {
      const LightData &light = inst_.lights.sync_light(index);
      if (light.color.x < 0.0f) {
        continue; /* GPU-extracted world light belongs to the world visibility path. */
      }
      (is_sun_light(light.type) ? sun_lights : local_lights).append(light);
    }
    hardware_local_light_count_ = min_ii(local_lights.size(), 256);
    hardware_light_count_ = min_ii(hardware_local_light_count_ + sun_lights.size(), 256);
    hardware_light_records_.resize(max_ii(hardware_light_count_, 1));
    for (int index = 0; index < hardware_light_count_; index++) {
      const LightData &light = index < hardware_local_light_count_ ?
                                  local_lights[index] : sun_lights[index - hardware_local_light_count_];
      hardware_light_records_[index] = hardware_fast_gi_light_record_from_light(light);
    }
    hardware_light_records_.push_update();
    hardware_light_records_generation_ = light_generation;
    hardware_light_records_valid_ = true;
  }
  p.light_buf = hardware_light_count_ ? static_cast<gpu::StorageBuf *>(hardware_light_records_) : nullptr;
  p.light_count = hardware_light_count_;
  p.local_light_count = hardware_local_light_count_;
  p.light_sample_count = hardware_light_count_ ? (inst_.is_viewport() ? 4 : 8) : 0;
  /* GI resolves before the scene-final specular family, without retracing either family twice. */
  const uint32_t phase_features = trace_phase == HWRT_TRACE_PHASE_PRECOMBINE ?
      RAYTRACE_EEVEE_HARDWARE_GI :
      (RAYTRACE_EEVEE_HARDWARE_REFLECTIONS | RAYTRACE_EEVEE_HARDWARE_REFRACTIONS);
  p.feature_mask = current_hardware_feature_mask_ & phase_features;
  p.hardware_trace_phase = trace_phase;
  switch (ray_tracing_options_.gi_spatial_samples) {
    case RAYTRACE_EEVEE_GI_SPATIAL_8:
    case RAYTRACE_EEVEE_GI_SPATIAL_16:
    case RAYTRACE_EEVEE_GI_SPATIAL_32:
      p.gi_diffuse_sample_count = ray_tracing_options_.gi_spatial_samples;
      break;
    default:
      p.gi_diffuse_sample_count = RAYTRACE_EEVEE_GI_SPATIAL_8;
  }
  p.reflection_bounces = inst_.scene->eevee.ray_tracing_reflection_bounces;
  p.refraction_bounces = inst_.scene->eevee.ray_tracing_refraction_bounces;
  const SphereProbe &world_probe = inst_.sphere_probes.world_sphere_probe();
  if (world_probe.atlas_coord.atlas_layer >= 0 && world_probe.atlas_coord.subdivision_lvl >= 0) {
    const SphereProbeUvArea coord = world_probe.atlas_coord.as_sampling_coord();
    p.world_probe_atlas_coord = float4(coord.offset.x, coord.offset.y, coord.scale, coord.layer);
  }
  p.use_environment = (hardware_feature_mask_ &
      (RAYTRACE_EEVEE_HARDWARE_REFLECTIONS | RAYTRACE_EEVEE_HARDWARE_REFRACTIONS)) != 0;
  p.use_diffuse_environment = (hardware_feature_mask_ & RAYTRACE_EEVEE_HARDWARE_GI) != 0;
  p.sampling_rand = float4(inst_.sampling.rng_3d_get(SAMPLING_RAYTRACE_U),
                          inst_.sampling.rng_get(SAMPLING_CLOSURE));
  p.dispatch_buf = raytrace_tracing_dispatch_buf_;
  p.tiles_coord_buf = raytrace_tracing_tiles_buf_;
  /* Submit generated rays, indirect arguments and payload clears before entering the raw queue. */
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_COMMAND |
                     GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  GPU_flush();
  if (!GPU_hardware_raytrace_scene_trace(scene, p)) {
    inst_.info_append_i18n("Hardware ray tracing dispatch failed");
    return false;
  }
  if (hardware_perf_logging_enabled()) {
    std::fprintf(stderr, "EEVEE HWRT trace phase=%d features=0x%x gi_spatial_samples=%d "
                 "world=(%g,%g,%g,%g) diffuse_environment=%d\n",
                 trace_phase, unsigned(p.feature_mask), p.gi_diffuse_sample_count,
                 UNPACK4(p.world_probe_atlas_coord), int(p.use_diffuse_environment));
  }
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  int payload_index = 0;
  for (HardwareHitBuffers *hit :
       {&hardware_hit_, &hardware_layered_hit_, &hardware_transmission_hit_})
  {
    HardwareHitEvalPayload payload = hit->payload();
    debug_hardware_hit_payload(payload, trace_phase, payload_index, "proxy");
    submit_hardware_hit_evaluation(
        render_view, payload, raytrace_tracing_dispatch_buf_, raytrace_tracing_tiles_buf_);
    debug_hardware_hit_payload(payload, trace_phase, payload_index++, "replay");
    render_hardware_hit_shadows(scene, *hit);
  }
  return true;
}

bool RayTraceModule::submit_hardware_hit_lighting(View &render_view, int trace_phase)
{
  gpu::Shader *shader = inst_.shaders.static_shader_get(RAY_TRACE_HARDWARE_LIGHTING);
  if (shader == nullptr) {
    return false;
  }
  /* Inactive receiver GI and the unimplemented photon producer use an invalid neutral payload. */
  TextureFromPool neutral("Hardware Optional Payload");
  neutral.acquire_2d(data_.full_resolution, gpu::TextureFormat::SFLOAT_16_16_16_16);
  neutral.clear(float4(0.0f));
  TextureFromPool environment("Hardware Environment Visibility");
  environment.acquire_2d(data_.full_resolution, gpu::TextureFormat::SFLOAT_16_16_16_16);
  environment.clear(float4(0.0f, 0.0f, 0.0f, 1.0f));
  TextureFromPool hit_environment("Hardware Hit Environment Visibility");
  hit_environment.acquire_2d(ray_data_tx_.size().xy(), gpu::TextureFormat::SFLOAT_16_16_16_16);
  hit_environment.clear(float4(0.0f, 0.0f, 0.0f, 1.0f));
  const bool use_environment = (hardware_feature_mask_ &
      (RAYTRACE_EEVEE_HARDWARE_REFLECTIONS | RAYTRACE_EEVEE_HARDWARE_REFRACTIONS)) != 0;
  bool environment_valid = false;
  if (use_environment) {
    GPU_flush();
    const float4 rng(inst_.sampling.rng_3d_get(SAMPLING_SHADOW_U),
                     inst_.sampling.rng_get(SAMPLING_SHADOW_X));
    GPUHardwareRaytraceEnvironmentVisibilityParams p;
    p.depth_tx = renderbuf_depth_view_;
    p.gbuf_header_tx = inst_.gbuffer.header_tx;
    p.gbuf_normal_tx = inst_.gbuffer.normal_tx;
    p.environment_visibility_tx = environment;
    p.viewinv = render_view.viewinv();
    p.wininv = render_view.wininv();
    p.full_resolution = data_.full_resolution;
    p.sample_count = 1; /* Nuru's temporal visibility sample count. */
    p.normal_bias = 5.0e-3f;
    p.sampling_rand = rng;
    environment_valid = GPU_hardware_raytrace_scene_trace_environment_visibility(
        hardware_rt_scene_cache_, p);
    GPUHardwareRaytraceHitEnvironmentVisibilityParams hit_p;
    hit_p.hit_normal_tx = hardware_hit_.normal;
    hit_p.hit_world_position_tx = hardware_hit_.world_position;
    hit_p.environment_visibility_tx = hit_environment;
    hit_p.dispatch_buf = raytrace_tracing_dispatch_buf_;
    hit_p.tiles_coord_buf = raytrace_tracing_tiles_buf_;
    hit_p.tracing_resolution = ray_data_tx_.size().xy();
    hit_p.sample_count = 1;
    hit_p.normal_bias = 5.0e-3f;
    hit_p.sampling_rand = rng;
    environment_valid &= GPU_hardware_raytrace_scene_trace_hit_environment_visibility(
        hardware_rt_scene_cache_, hit_p);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }

  PassSimple pass("Hardware Hit Lighting");
  pass.init();
  pass.specialize_constant(shader, "closure_index", data_.closure_index);
  pass.specialize_constant(shader, "use_hardware_environment", use_environment);
  pass.specialize_constant(shader, "use_hardware_rt_shadows", hardware_hit_.shadows_valid);
  pass.specialize_constant(shader, "use_hardware_rt_environment_visibility", environment_valid);
  pass.shader_set(shader);
  pass.push_constant("hardware_trace_phase", trace_phase);
  pass.push_constant("hardware_reflection_mode",
      (hardware_feature_mask_ & RAYTRACE_EEVEE_HARDWARE_REFLECTIONS) ? 2 : 0);
  pass.push_constant("hardware_refraction_mode",
      (hardware_feature_mask_ & RAYTRACE_EEVEE_HARDWARE_REFRACTIONS) ? 2 : 0);
  pass.bind_image("ray_data_img", &ray_data_tx_);
  pass.bind_image("ray_time_img", &hardware_hit_.time);
  pass.bind_image("ray_radiance_img", &hardware_hit_.radiance);
  pass.bind_image("hit_albedo_img", &hardware_hit_.albedo);
  pass.bind_image("hit_material_img", &hardware_hit_.material);
  pass.bind_image("hit_normal_img", &hardware_hit_.normal);
  pass.bind_image("hit_position_img", &hardware_hit_.position);
  pass.bind_image("hit_identity_img", &hardware_hit_.identity);
  pass.bind_texture("hit_world_position_tx", &hardware_hit_.world_position);
  pass.bind_texture("hit_barycentric_tx", &hardware_hit_.barycentric);
  pass.bind_texture("hit_transmission_layer_tx", &hardware_hit_.throughput);
  pass.bind_texture("hardware_rt_shadow_visibility_tx", &hardware_hit_.shadow_visibility);
  pass.bind_texture("layered_receiver_throughput_tx", &hardware_layered_hit_.throughput);
  pass.bind_texture("layered_receiver_ray_time_tx", &hardware_layered_hit_.time);
  pass.bind_texture("layered_receiver_ray_radiance_tx", &hardware_layered_hit_.radiance);
  pass.bind_texture("layered_receiver_hit_albedo_tx", &hardware_layered_hit_.albedo);
  pass.bind_texture("layered_receiver_hit_material_tx", &hardware_layered_hit_.material);
  pass.bind_texture("layered_receiver_hit_normal_tx", &hardware_layered_hit_.normal);
  pass.bind_texture("layered_receiver_hit_position_tx", &hardware_layered_hit_.position);
  pass.bind_texture("layered_receiver_hit_identity_tx", &hardware_layered_hit_.identity);
  pass.bind_texture("layered_receiver_hit_barycentric_tx", &hardware_layered_hit_.barycentric);
  pass.bind_texture("layered_receiver_world_position_tx", &hardware_layered_hit_.world_position);
  pass.bind_texture("hardware_layered_receiver_rt_shadow_visibility_tx",
                     &hardware_layered_hit_.shadow_visibility);
  pass.bind_texture("transmission_receiver_throughput_tx", &hardware_transmission_hit_.throughput);
  pass.bind_texture("transmission_receiver_ray_time_tx", &hardware_transmission_hit_.time);
  pass.bind_texture("transmission_receiver_ray_radiance_tx", &hardware_transmission_hit_.radiance);
  pass.bind_texture("transmission_receiver_hit_albedo_tx", &hardware_transmission_hit_.albedo);
  pass.bind_texture("transmission_receiver_hit_material_tx", &hardware_transmission_hit_.material);
  pass.bind_texture("transmission_receiver_hit_normal_tx", &hardware_transmission_hit_.normal);
  pass.bind_texture("transmission_receiver_hit_position_tx", &hardware_transmission_hit_.position);
  pass.bind_texture("transmission_receiver_hit_identity_tx", &hardware_transmission_hit_.identity);
  pass.bind_texture("transmission_receiver_world_position_tx", &hardware_transmission_hit_.world_position);
  pass.bind_texture("hardware_transmission_receiver_rt_shadow_visibility_tx",
                     &hardware_transmission_hit_.shadow_visibility);
  pass.bind_texture("hardware_rt_environment_visibility_tx", &environment);
  pass.bind_texture("hardware_rt_hit_environment_visibility_tx", &hit_environment);
  pass.bind_texture("hardware_direct_light_tx",
                    hardware_direct_ready_ ? static_cast<gpu::Texture *>(hardware_direct_combined_tx_) :
                                             static_cast<gpu::Texture *>(neutral));
  pass.bind_texture("hardware_reflected_receiver_gi_tx",
                    hardware_hit_.receiver_gi ? &hardware_hit_.receiver_gi : &neutral);
  pass.bind_texture("hardware_layered_receiver_gi_tx",
                    hardware_layered_hit_.receiver_gi ? &hardware_layered_hit_.receiver_gi : &neutral);
  pass.bind_texture("hardware_transmission_receiver_gi_tx",
                    hardware_transmission_hit_.receiver_gi ? &hardware_transmission_hit_.receiver_gi :
                                                          &neutral);
  pass.bind_texture("hardware_secondary_photon_gi_tx", &neutral);
  pass.bind_texture("hardware_layered_secondary_photon_gi_tx", &neutral);
  pass.bind_texture("hardware_transmission_secondary_photon_gi_tx", &neutral);
  pass.bind_texture("radiance_front_tx", &screen_radiance_front_tx_);
  pass.bind_texture("radiance_back_tx", &screen_radiance_back_tx_);
  pass.bind_texture("depth_tx", &renderbuf_depth_view_);
  pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
  pass.bind_ssbo("tiles_coord_buf", &raytrace_tracing_tiles_buf_);
  pass.bind_resources(inst_.uniform_data);
  pass.bind_resources(inst_.sampling);
  pass.bind_resources(inst_.gbuffer);
  pass.bind_resources(inst_.sphere_probes);
  pass.bind_resources(inst_.volume_probes);
  pass.bind_resources(inst_.shadows);
  inst_.lights.bind_hardware_hit_lighting(pass);
  pass.dispatch(raytrace_tracing_dispatch_buf_);
  pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_.manager->submit(pass, render_view);
  debug_hardware_hit_payload(hardware_hit_.payload(), trace_phase, 0, "lighting");
  neutral.release();
  environment.release();
  hit_environment.release();
  return true;
}

void RayTraceModule::debug_hardware_hit_payload(const HardwareHitEvalPayload &payload,
                                                int trace_phase,
                                                int payload_index,
                                                const char *stage)
{
  if (std::getenv("BLENDER_EEVEE_HWRT_HIT_READBACK") == nullptr) {
    return;
  }
  /* Deliberate GPU synchronization for development tests only, never a normal-frame cost. */
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
  const int2 extent = ray_data_tx_.size().xy();
  const int count = extent.x * extent.y;
  uint4 *identity = static_cast<uint4 *>(GPU_texture_read(payload.identity, GPU_DATA_UINT, 0));
  float *time = static_cast<float *>(GPU_texture_read(payload.ray_time, GPU_DATA_FLOAT, 0));
  float4 *albedo = static_cast<float4 *>(GPU_texture_read(payload.albedo, GPU_DATA_FLOAT, 0));
  float3 *radiance = static_cast<float3 *>(GPU_texture_read(payload.ray_radiance, GPU_DATA_FLOAT, 0));
  if (const char *sample = std::getenv("BLENDER_EEVEE_HWRT_HIT_PIXEL")) {
    int x = 0, y = 0;
    if (std::sscanf(sample, "%d,%d", &x, &y) == 2 && x >= 0 && y >= 0 &&
        x < extent.x && y < extent.y)
    {
      const int pixel = y * extent.x + x;
      float4 *material = static_cast<float4 *>(GPU_texture_read(payload.material, GPU_DATA_FLOAT, 0));
      float4 *position = static_cast<float4 *>(GPU_texture_read(payload.position, GPU_DATA_FLOAT, 0));
      float4 *throughput = static_cast<float4 *>(GPU_texture_read(payload.throughput, GPU_DATA_FLOAT, 0));
      std::fprintf(stderr,
          "EEVEE HWRT pixel stage=%s phase=%d closure=%d payload=%d xy=%d,%d "
          "time=%g identity=(%u,%u,%u,%u) base=(%g,%g,%g,%g) "
          "material=(%g,%g,%g,%g) specular=(%g,%g,%g,%g) "
          "throughput=(%g,%g,%g,%g) radiance=(%g,%g,%g)\n",
          stage, trace_phase, data_.closure_index, payload_index, x, y, time[pixel],
          UNPACK4(identity[pixel]), UNPACK4(albedo[pixel]), UNPACK4(material[pixel]),
          UNPACK4(position[pixel]), UNPACK4(throughput[pixel]), UNPACK3(radiance[pixel]));
      MEM_delete_void(static_cast<void *>(material));
      MEM_delete_void(static_cast<void *>(position));
      MEM_delete_void(static_cast<void *>(throughput));
    }
  }
  for (const int entry : hardware_rt_scene_entries_cache_.index_range()) {
    int hits = 0;
    int valid_resources = 0;
    float3 sum(0.0f), minimum(1e10f), maximum(-1e10f), emission(0.0f);
    for (int pixel = 0; pixel < count; pixel++) {
      if (!(time[pixel] > 0.0f) || identity[pixel].x != uint(entry)) {
        continue;
      }
      hits++;
      valid_resources += identity[pixel].w ==
          hardware_rt_scene_entries_cache_[entry].resource_handle.index();
      sum += albedo[pixel].xyz();
      minimum = math::min(minimum, albedo[pixel].xyz());
      maximum = math::max(maximum, albedo[pixel].xyz());
      emission += radiance[pixel];
    }
    if (hits == 0) {
      continue;
    }
    GPUMaterial *material = hardware_rt_scene_entries_cache_[entry].hit_eval_material;
    const char *name = material ? GPU_material_get_name(material) : "<default>";
    sum /= float(hits);
    emission /= float(hits);
    std::fprintf(stderr,
        "EEVEE HWRT payload stage=%s phase=%d closure=%d payload=%d entry=%d material=%s "
        "hits=%d resources=%d avg=(%.6f,%.6f,%.6f) min=(%.6f,%.6f,%.6f) "
        "max=(%.6f,%.6f,%.6f) emission=(%.6f,%.6f,%.6f)\n",
        stage, trace_phase, data_.closure_index, payload_index, entry, name, hits, valid_resources,
        UNPACK3(sum), UNPACK3(minimum), UNPACK3(maximum), UNPACK3(emission));
  }
  MEM_delete_void(static_cast<void *>(identity));
  MEM_delete_void(static_cast<void *>(time));
  MEM_delete_void(static_cast<void *>(albedo));
  MEM_delete_void(static_cast<void *>(radiance));
}

bool RayTraceModule::submit_hardware_hit_evaluation(View &render_view,
                                                   const HardwareHitEvalPayload &payload,
                                                   DispatchIndirectBuf &dispatch,
                                                   RayTraceTileBuf &tiles)
{
  const Span<HardwareRaytraceSceneEntry> entries = hardware_rt_scene_entries_cache_;
  if (entries.is_empty()) {
    return false;
  }
  const int entry_count = entries.size();
  const int2 extent = ray_data_tx_.size().xy();
  hit_eval_count_buf_.resize(entry_count);
  hit_eval_offset_buf_.resize(entry_count);
  hit_eval_cursor_buf_.resize(entry_count);
  hit_eval_resource_id_buf_.resize(entry_count);
  hit_eval_indirect_buf_.resize(entry_count);
  /* Match Nuru's grow-only allocation across GI and scene-final specular phases. */
  hit_eval_records_buf_.resize(max_ii(int(hit_eval_records_buf_.size()), extent.x * extent.y));
  if (!hit_eval_resource_ids_valid_ || hit_eval_resource_ids_count_ != entry_count ||
      hit_eval_resource_ids_generation_ != hardware_sorted_scene_entries_update_count_)
  {
    for (const int i : entries.index_range()) {
      /* CPU ResourceID's high bit is handedness, not part of a GPU matrix-buffer index. */
      hit_eval_resource_id_buf_[i] = entries[i].resource_handle.index();
    }
    hit_eval_resource_id_buf_.push_update();
    hit_eval_resource_ids_generation_ = hardware_sorted_scene_entries_update_count_;
    hit_eval_resource_ids_count_ = entry_count;
    hit_eval_resource_ids_valid_ = true;
  }

  GPU_debug_group_begin("Hardware RT Hit Eval");
  hit_eval_count_buf_.clear_to_zero();
  hit_eval_offset_buf_.clear_to_zero();
  hit_eval_cursor_buf_.clear_to_zero();
  hit_eval_indirect_buf_.clear_to_zero();

  hit_eval_count_ps_.init();
  hit_eval_count_ps_.shader_set(inst_.shaders.static_shader_get(RAY_HIT_EVAL_COUNT));
  hit_eval_count_ps_.push_constant("scene_entry_count", entry_count);
  hit_eval_count_ps_.bind_image("ray_time_img", payload.ray_time);
  hit_eval_count_ps_.bind_image("hit_identity_img", payload.identity);
  hit_eval_count_ps_.bind_texture("depth_tx", &renderbuf_depth_view_);
  hit_eval_count_ps_.bind_ssbo("hit_eval_count_buf", &hit_eval_count_buf_);
  hit_eval_count_ps_.bind_ssbo("tiles_coord_buf", &tiles);
  hit_eval_count_ps_.bind_resources(inst_.uniform_data);
  hit_eval_count_ps_.dispatch(dispatch);
  hit_eval_count_ps_.barrier(GPU_BARRIER_SHADER_STORAGE);
  inst_.manager->submit(hit_eval_count_ps_);

  hit_eval_prefix_ps_.init();
  hit_eval_prefix_ps_.shader_set(inst_.shaders.static_shader_get(RAY_HIT_EVAL_PREFIX));
  hit_eval_prefix_ps_.push_constant("scene_entry_count", entry_count);
  hit_eval_prefix_ps_.bind_ssbo("hit_eval_count_buf", &hit_eval_count_buf_);
  hit_eval_prefix_ps_.bind_ssbo("hit_eval_offset_buf", &hit_eval_offset_buf_);
  hit_eval_prefix_ps_.bind_ssbo("hit_eval_cursor_buf", &hit_eval_cursor_buf_);
  hit_eval_prefix_ps_.bind_ssbo("hit_eval_indirect_draw_buf", &hit_eval_indirect_buf_);
  hit_eval_prefix_ps_.dispatch(int3(divide_ceil_u(entry_count, 64), 1, 1));
  hit_eval_prefix_ps_.barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_COMMAND);
  inst_.manager->submit(hit_eval_prefix_ps_);

  hit_eval_compact_ps_.init();
  hit_eval_compact_ps_.shader_set(inst_.shaders.static_shader_get(RAY_HIT_EVAL_COMPACT));
  hit_eval_compact_ps_.push_constant("scene_entry_count", entry_count);
  hit_eval_compact_ps_.bind_image("ray_time_img", payload.ray_time);
  hit_eval_compact_ps_.bind_image("hit_identity_img", payload.identity);
  hit_eval_compact_ps_.bind_image("hit_material_img", payload.material);
  hit_eval_compact_ps_.bind_image("hit_normal_img", payload.normal);
  hit_eval_compact_ps_.bind_image("hit_barycentric_img", payload.barycentric);
  hit_eval_compact_ps_.bind_texture("depth_tx", &renderbuf_depth_view_);
  hit_eval_compact_ps_.bind_texture("hit_world_position_tx", payload.world_position);
  hit_eval_compact_ps_.bind_ssbo("hit_eval_offset_buf", &hit_eval_offset_buf_);
  hit_eval_compact_ps_.bind_ssbo("hit_eval_cursor_buf", &hit_eval_cursor_buf_);
  hit_eval_compact_ps_.bind_ssbo("hit_eval_resource_id_buf", &hit_eval_resource_id_buf_);
  hit_eval_compact_ps_.bind_ssbo("hit_eval_list_buf", &hit_eval_records_buf_);
  hit_eval_compact_ps_.bind_ssbo("tiles_coord_buf", &tiles);
  hit_eval_compact_ps_.bind_resources(inst_.uniform_data);
  hit_eval_compact_ps_.dispatch(dispatch);
  hit_eval_compact_ps_.barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  inst_.manager->submit(hit_eval_compact_ps_, render_view);
  GPU_storagebuf_sync_as_indirect_buffer(hit_eval_indirect_buf_);

  hit_eval_fb_.ensure(GPU_ATTACHMENT_TEXTURE(renderbuf_depth_view_));
  hit_eval_fb_.bind();
  GPU_framebuffer_viewport_set(hit_eval_fb_, 0, 0, UNPACK2(extent));
  hit_eval_ps_.init();
  /* A write bit enables rasterization, even though this pass writes storage images and the
   * framebuffer has no color attachment. NO_DRAW would discard every replay fragment. */
  hit_eval_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_ALWAYS);
  hit_eval_ps_.framebuffer_set(&hit_eval_fb_);
  int draw_count = 0;
  for (const int i : entries.index_range()) {
    const HardwareRaytraceSceneEntry &entry = entries[i];
    GPUMaterial *gpumat = entry.hit_eval_material;
    if (entry.batch == nullptr || gpumat == nullptr || GPU_material_status(gpumat) != GPU_MAT_SUCCESS)
    {
      continue; /* Preserve the traversal proxy if compilation failed. Never drop the object. */
    }
    hit_eval_ps_.material_set(*inst_.manager, gpumat, true, inst_.anisotropic_filtering);
    hit_eval_ps_.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    hit_eval_ps_.bind_resources(inst_.hiz_buffer.front);
    hit_eval_ps_.bind_resources(inst_.uniform_data);
    hit_eval_ps_.bind_resources(inst_.sampling);
    hit_eval_ps_.bind_texture("ray_data_tx", &ray_data_tx_);
    hit_eval_ps_.bind_texture("ray_time_tx", payload.ray_time);
    hit_eval_ps_.bind_texture("hit_identity_tx", payload.identity);
    hit_eval_ps_.bind_texture("hit_barycentric_tx", payload.barycentric);
    hit_eval_ps_.bind_texture("hit_world_position_tx", payload.world_position);
    hit_eval_ps_.bind_texture("hit_emission_replay_tx", payload.emission_replay);
    /* Match the traversal clamp, before throughput is applied. */
    hit_eval_ps_.push_constant("hit_proxy_emission", math::min(entry.emissive_radiance,
                                                             float3(1.0e10f)));
    hit_eval_ps_.bind_image("ray_radiance_img", payload.ray_radiance);
    hit_eval_ps_.bind_image("hit_albedo_img", payload.albedo);
    hit_eval_ps_.bind_image("hit_throughput_img", payload.throughput);
    hit_eval_ps_.bind_image("hit_material_img", payload.material);
    hit_eval_ps_.bind_image("hit_normal_img", payload.normal);
    hit_eval_ps_.bind_image("hit_position_img", payload.position);
    hit_eval_ps_.bind_ssbo("hit_eval_list_buf", &hit_eval_records_buf_);
    hit_eval_ps_.push_constant("hit_eval_index_start",
                             entry.batch->elem ? int(entry.batch->elem->index_start_get()) : 0);
    hit_eval_ps_.draw_expand_indirect(entry.batch, GPU_PRIM_TRIS, 1, &hit_eval_indirect_buf_,
                                      uint32_t(sizeof(DrawCommandArray) * i),
                                      ResourceIDRange(entry.resource_handle).first);
    draw_count++;
  }
  if (draw_count != 0) {
    hit_eval_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
    inst_.manager->submit(hit_eval_ps_, render_view);
  }
  GPU_debug_group_end();
  if (hardware_perf_logging_enabled()) {
    std::fprintf(stderr, "EEVEE HWRT hit_replay entries=%d draws=%d extent=%dx%d\n",
                 entry_count, draw_count, extent.x, extent.y);
  }
  return draw_count != 0;
}

}  // namespace blender::eevee
