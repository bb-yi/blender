/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * The ray-tracing module class handles ray generation, scheduling, tracing and denoising.
 */

#pragma once

#include <memory>

#include "DNA_scene_types.h"
#include "GPU_capabilities.hh"
#include "GPU_nuru_hardware_raytrace.hh"

#include "DRW_gpu_wrapper.hh"
#include "DRW_render.hh"

#include "eevee_raytrace_shared.hh"
#include "eevee_sync.hh"

namespace blender::eevee {

class Instance;

using RayTraceTileBuf = draw::StorageArrayBuffer<uint, 1024, true>;

/* -------------------------------------------------------------------- */
/** \name Ray-tracing Buffers
 *
 * Contain persistent data used for temporal denoising. Similar to \class GBuffer but only contains
 * persistent data.
 * \{ */

/**
 * Contain persistent buffer that need to be stored per view, per deferred layer.
 */
struct RayTraceBuffer {
  /** Set of buffers that need to be allocated for each ray type. */
  struct DenoiseBuffer {
    /* Persistent history buffers. */
    TextureFromPool radiance_history_tx = {"radiance_tx"};
    TextureFromPool variance_history_tx = {"variance_tx"};
    TextureFromPool signature_history_tx = {"anisotropy_history_tx"};
    TextureFromPool denoised_signature_tx = {"anisotropy_signature_tx"};
    /* Map of tiles that were processed inside the history buffer. */
    Texture tilemask_history_tx = {"tilemask_tx"};
    /** Perspective matrix for which the history buffers were recorded. */
    float4x4 history_persmat;
    /** True if history buffer was used last frame and can be re-projected. */
    bool valid_history = false;
    /**
     * Textures containing the ray hit radiance denoised (full-res). One of them is result_tx.
     * One might become result buffer so it need instantiation by closure type to avoid reuse.
     */
    TextureFromPool denoised_spatial_tx = {"denoised_spatial_tx"};
    TextureFromPool denoised_temporal_tx = {"denoised_temporal_tx"};
    TextureFromPool denoised_bilateral_tx = {"denoised_bilateral_tx"};
  };
  /**
   * One for each closure. Not to be mistaken with deferred layer type.
   */
  DenoiseBuffer closures[3];
  /* Separate phase histories: a bin can contain diffuse at one pixel and glass at another. */
  std::unique_ptr<RayTraceBuffer> scene_final_specular;
  TextureFromPool shared_indirect_tx = {"shared_indirect_reconstructed_tx"};
  TextureFromPool merged_indirect_tx[3] = {
      {"merged_indirect_0"}, {"merged_indirect_1"}, {"merged_indirect_2"}};
  eHardwareTracePhase history_phase = HWRT_TRACE_PHASE_FULL;
  /** Last viewport sampling reset applied to this deferred layer's NPR history. */
  uint64_t npr_history_reset_generation = 0;

  /**
   * Radiance feedback of the deferred layer for next sample's reflection or next layer's
   * transmission.
   */
  Texture radiance_feedback_tx = {"radiance_feedback_tx"};
  /**
   * Perspective matrix for which the radiance feedback buffer was recorded.
   * Can be different from de-noise buffer's history matrix.
   */
  float4x4 history_persmat = float4x4::zero();

  gpu::Texture *feedback_ensure(bool is_dummy, int2 extent)
  {
    eGPUTextureUsage usage_rw = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
    if (radiance_feedback_tx.ensure_2d(
            gpu::TextureFormat::SFLOAT_16_16_16_16, is_dummy ? int2(1) : extent, usage_rw))
    {
      radiance_feedback_tx.clear(float4(0.0f));
    }
    return radiance_feedback_tx;
  }
};

/**
 * Contains the result texture.
 * The result buffer is usually short lived and is kept in a TextureFromPool managed by the mode.
 * This structure contains a reference to it so that it can be freed after use by the caller.
 */
class RayTraceResultTexture {
 private:
  /** Result is in a temporary texture that needs to be released. */
  TextureFromPool *result_ = nullptr;
  /** Value of `result_->tx_` that can be referenced in advance. */
  gpu::Texture *tx_ = nullptr;
  /** History buffer to swap the temporary texture that does not need to be released. */
  TextureFromPool *history_ = nullptr;

 public:
  RayTraceResultTexture() = default;
  RayTraceResultTexture(TextureFromPool &result) : result_(result.ptr()), tx_(result) {};
  RayTraceResultTexture(TextureFromPool &result, TextureFromPool &history)
      : result_(result.ptr()), tx_(result), history_(history.ptr()) {};

  operator gpu::Texture *() const
  {
    BLI_assert(tx_ != nullptr);
    return tx_;
  }

  gpu::Texture **operator&()
  {
    return &tx_;
  }

  void release()
  {
    if (history_) {
      /* Swap after last use, retain history until next cycle. */
      TextureFromPool::swap(*result_, *history_);
      history_->retain();
    }
    /* Release previous history. */
    result_->release();
  }
};

struct RayTraceResult {
  RayTraceResultTexture closures[3];
  RayTraceResultTexture shared_indirect;
  bool use_shared_indirect = false;

  void release()
  {
    for (int i = 0; i < 3; i++) {
      closures[i].release();
    }
    if (use_shared_indirect) {
      shared_indirect.release();
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Ray-tracing
 * \{ */

class RayTraceModule {
 private:
  Instance &inst_;

  draw::PassSimple tile_classify_ps_ = {"TileClassify"};
  draw::PassSimple tile_compact_ps_ = {"TileCompact"};
  draw::PassSimple generate_ps_ = {"RayGenerate"};
  draw::PassSimple trace_planar_ps_ = {"Trace.Planar"};
  draw::PassSimple trace_screen_ps_ = {"Trace.Screen"};
  draw::PassSimple trace_fallback_ps_ = {"Trace.Fallback"};
  draw::PassSimple denoise_spatial_ps_ = {"DenoiseSpatial"};
  draw::PassSimple denoise_temporal_ps_ = {"DenoiseTemporal"};
  draw::PassSimple denoise_bilateral_ps_ = {"DenoiseBilateral"};
  draw::PassSimple fast_gi_schedule_ps_ = {"FastGI.Schedule"};
  draw::PassSimple fast_gi_setup_ps_ = {"FastGI.Setup"};
  draw::PassSimple fast_gi_scan_ps_ = {"FastGI.Trace"};
  draw::PassSimple fast_gi_denoise_ps_ = {"FastGI.Denoise"};
  draw::PassSimple fast_gi_resolve_ps_ = {"FastGI.Resolve"};

  /** Dispatch with enough tiles for the whole screen. */
  int3 tile_classify_dispatch_size_ = int3(1);
  /** Dispatch with enough tiles for the tile mask. */
  int3 tile_compact_dispatch_size_ = int3(1);
  int3 fast_gi_schedule_dispatch_size_ = int3(1);
  /** Dispatch with enough tiles for the tracing resolution. */
  int3 tracing_dispatch_size_ = int3(1);
  int3 fast_gi_tracing_dispatch_size_ = int3(1);
  /** 2D tile mask to check which unused adjacent tile we need to clear and which tile we need to
   * dispatch for each work type. */
  Texture tile_raytrace_denoise_tx_ = {"tile_raytrace_denoise_tx_"};
  Texture tile_raytrace_tracing_tx_ = {"tile_raytrace_tracing_tx_"};
  Texture tile_fast_gi_denoise_tx_ = {"tile_fast_gi_denoise_tx_"};
  Texture tile_fast_gi_tracing_tx_ = {"tile_fast_gi_tracing_tx_"};
  /** Indirect dispatch rays. Avoid dispatching work-groups that will not trace anything. */
  DispatchIndirectBuf raytrace_tracing_dispatch_buf_ = {"raytrace_tracing_dispatch_buf_"};
  /** Indirect dispatch denoise full-resolution tiles. */
  DispatchIndirectBuf raytrace_denoise_dispatch_buf_ = {"raytrace_denoise_dispatch_buf_"};
  /** Indirect dispatch fast GI scan. Avoid dispatching work-groups that will not scan anything. */
  DispatchIndirectBuf fast_gi_tracing_dispatch_buf_ = {"fast_gi_tracing_dispatch_buf_"};
  /** Indirect dispatch denoise full-resolution tiles. */
  DispatchIndirectBuf fast_gi_denoise_dispatch_buf_ = {"fast_gi_denoise_dispatch_buf_"};
  /** Pointer to the texture to store the result of fast GI scan in. */
  gpu::Texture *fast_gi_scan_output_tx_[3] = {nullptr};
  /** Tile buffer that contains tile coordinates. */
  RayTraceTileBuf raytrace_tracing_tiles_buf_ = {"raytrace_tracing_tiles_buf_"};
  RayTraceTileBuf raytrace_denoise_tiles_buf_ = {"raytrace_denoise_tiles_buf_"};
  RayTraceTileBuf fast_gi_tracing_tiles_buf_ = {"fast_gi_tracing_tiles_buf_"};
  RayTraceTileBuf fast_gi_denoise_tiles_buf_ = {"fast_gi_denoise_tiles_buf_"};
  /** Texture containing the ray direction and PDF. */
  TextureFromPool ray_data_tx_ = {"ray_data_tx"};
  /** Texture containing the ray hit time. */
  TextureFromPool ray_time_tx_ = {"ray_data_tx"};
  /** Texture containing the ray hit radiance (tracing-res). */
  TextureFromPool ray_radiance_tx_ = {"ray_radiance_tx"};
  /** Nuru's per-closure OIDN input guides, merged across the disjoint GI/specular phases. */
  TextureFromPool ray_oidn_albedo_tx_ = {"ray_oidn_albedo_tx"};
  TextureFromPool ray_oidn_normal_tx_ = {"ray_oidn_normal_tx"};
  TextureFromPool ray_radiance_oidn_tx_ = {"ray_radiance_oidn_tx"};
  TextureFromPool shared_indirect_radiance_tx_ = {"shared_indirect_radiance_tx"};
  TextureFromPool shared_indirect_albedo_tx_ = {"shared_indirect_albedo_tx"};
  TextureFromPool shared_indirect_normal_tx_ = {"shared_indirect_normal_tx"};
  bool use_shared_oidn_ = false;
  /** Texture containing the fast GI local radiance. */
  TextureFromPool fast_gi_radiance_tx_[4] = {{"fast_gi_radiance_tx_"}};
  TextureFromPool fast_gi_radiance_denoised_tx_[4] = {{"fast_gi_radiance_denoised_tx_"}};
  /** Texture containing the input screen radiance but re-projected. */
  Texture downsampled_in_radiance_tx_ = {"downsampled_in_radiance_tx_"};
  /** Texture containing the view space normal. The BSDF normal is arbitrarily chosen. */
  Texture downsampled_in_normal_tx_ = {"downsampled_in_normal_tx_"};
  /** Pointers to the mip view of the above textures. Available after sync. */
  gpu::Texture *downsampled_in_radiance_tx_ptr_[4] = {nullptr};
  gpu::Texture *downsampled_in_normal_tx_ptr_[4] = {nullptr};
  /** Textures containing the ray hit radiance denoised (full-res). One of them is result_tx. */
  gpu::Texture *denoised_spatial_tx_ = nullptr;
  gpu::Texture *denoised_temporal_tx_ = nullptr;
  gpu::Texture *denoised_bilateral_tx_ = nullptr;
  /** Ray hit depth for temporal denoising. Output of spatial denoise. */
  TextureFromPool hit_depth_tx_ = {"hit_depth_tx_"};
  /** Ray hit variance for temporal denoising. Output of spatial denoise. */
  TextureFromPool hit_variance_tx_ = {"hit_variance_tx_"};
  /** Temporally stable variance for temporal denoising. Output of temporal denoise. */
  TextureFromPool denoise_variance_tx_ = {"denoise_variance_tx_"};
  /** Persistent texture reference for temporal denoising input. */
  gpu::Texture *radiance_history_tx_ = nullptr;
  gpu::Texture *variance_history_tx_ = nullptr;
  gpu::Texture *signature_history_tx_ = nullptr;
  gpu::Texture *denoised_signature_tx_ = nullptr;
  bool use_npr_history_ = false;
  uint64_t npr_history_reset_generation_ = 0;
  gpu::Texture *tilemask_history_tx_ = nullptr;
  /** Radiance input for screen space tracing. */
  gpu::Texture *screen_radiance_front_tx_ = nullptr;
  gpu::Texture *screen_radiance_back_tx_ = nullptr;

  Texture radiance_dummy_black_tx_ = {"radiance_dummy_black_tx"};
  /** Dummy texture when the tracing is disabled. */
  TextureFromPool dummy_result_tx_ = {"dummy_result_tx"};
  /** Pointer to `inst_.render_buffers.depth_tx` updated before submission. */
  gpu::Texture *renderbuf_depth_view_ = nullptr;

  /** Copy of the scene options to avoid changing parameters during motion blur. */
  RaytraceEEVEE ray_tracing_options_;
  int fast_gi_ray_count_ = 0;
  int fast_gi_step_count_ = 0;
  bool fast_gi_ao_only_ = false;

  bool use_raytracing_ = false;

  RaytraceEEVEE_Method tracing_method_ = RAYTRACE_EEVEE_METHOD_PROBE;

  RayTraceData &data_;

  uint32_t hardware_feature_mask_ = 0;
  uint32_t current_hardware_feature_mask_ = 0;
  GPUHardwareRaytraceScene *hardware_rt_scene_cache_ = nullptr;
  GPUHardwareRaytraceSceneStats hardware_rt_scene_stats_cache_ = {};
  Vector<HardwareRaytraceSceneEntry> hardware_rt_scene_entries_cache_;
  uint64_t hardware_rt_scene_update_count_ = 0;
  bool hardware_rt_scene_update_count_valid_ = false;
  uint64_t hardware_rt_scene_signature_ = 0;
  bool hardware_rt_scene_signature_valid_ = false;
  Vector<HardwareRaytraceSceneEntry> hardware_sorted_scene_entries_cache_;
  uint64_t hardware_sorted_scene_entries_update_count_ = 0;
  bool hardware_sorted_scene_entries_update_count_valid_ = false;
  int hardware_scene_entry_count_ = 0;
  int hardware_scene_instance_count_ = 0;
  Texture hardware_shadow_visibility_tx_ = {"hardware_shadow_visibility_tx"};
  bool hardware_shadow_ready_ = false;
  bool hardware_direct_ready_ = false;
  bool hardware_direct_replace_ = false;
  Texture hardware_direct_closures_tx_ = {"hwrt_direct_closures"};
  Texture hardware_direct_accum_tx_ = {"hwrt_direct_accum"};
  Texture hardware_direct_combined_tx_ = {"hwrt_direct_combined"};
  Texture hardware_direct_depth_tx_ = {"hwrt_direct_depth"};
  Texture hardware_direct_tilemask_tx_ = {"hwrt_direct_tilemask"};
  DispatchIndirectBuf hardware_direct_dispatch_buf_ = {"hwrt_direct_dispatch"};
  draw::StorageArrayBuffer<HardwareDirectLightWorkTile, 1024, true>
      hardware_direct_tiles_buf_ = {"hwrt_direct_tiles"};
  draw::StorageArrayBuffer<HardwareDirectLightVisibilitySample, 1024, true>
      hardware_direct_samples_buf_ = {"hwrt_direct_samples"};
  draw::StorageArrayBuffer<float, 32> hardware_cluster_weights_buf_ = {"hwrt_cluster_weights"};
  draw::StorageArrayBuffer<float, 4> hardware_nis_weights_buf_ = {"hwrt_nis_weights"};
  draw::StorageArrayBuffer<int, 4> hardware_nis_grads_buf_ = {"hwrt_nis_grads"};
  draw::StorageArrayBuffer<uint, 4> hardware_nis_count_buf_ = {"hwrt_nis_count"};
  draw::StorageArrayBuffer<float, 4> hardware_nis_m_buf_ = {"hwrt_nis_m"};
  draw::StorageArrayBuffer<float, 4> hardware_nis_v_buf_ = {"hwrt_nis_v"};
  bool hardware_nis_initialized_ = false;
  int hardware_nis_train_offset_ = 0;

  struct HardwareHitEvalPayload {
    gpu::Texture *ray_time;
    gpu::Texture *ray_radiance;
    gpu::Texture *albedo;
    gpu::Texture *throughput;
    gpu::Texture *material;
    gpu::Texture *normal;
    gpu::Texture *position;
    gpu::Texture *world_position;
    gpu::Texture *identity;
    gpu::Texture *barycentric;
    gpu::Texture *emission_replay;
  };
  struct HardwareHitBuffers {
    TextureFromPool time = {"hwrt_hit_time"};
    TextureFromPool radiance = {"hwrt_hit_radiance"};
    TextureFromPool albedo = {"hwrt_hit_albedo"};
    TextureFromPool throughput = {"hwrt_hit_throughput"};
    TextureFromPool material = {"hwrt_hit_material"};
    TextureFromPool normal = {"hwrt_hit_normal"};
    TextureFromPool position = {"hwrt_hit_position"};
    TextureFromPool world_position = {"hwrt_hit_world_position"};
    TextureFromPool identity = {"hwrt_hit_identity"};
    TextureFromPool barycentric = {"hwrt_hit_barycentric"};
    /* Incoming throughput and whether traversal already accumulated this hit's proxy emission. */
    TextureFromPool emission_replay = {"hwrt_hit_emission_replay"};
    TextureFromPool shadow_visibility = {"hwrt_hit_shadow_visibility"};
    TextureFromPool receiver_gi = {"hwrt_receiver_gi"};
    bool shadows_valid = false;

    void acquire(int2 extent);
    void release();
    HardwareHitEvalPayload payload();
  };
  /* The traced surface and Nuru's separate layered/transmission receiver payloads. */
  HardwareHitBuffers hardware_hit_, hardware_layered_hit_, hardware_transmission_hit_;
  TextureFromPool hardware_screen_continuation_tx_ = {"hardware_screen_continuation_tx"};
  draw::StorageArrayBuffer<GPUHardwareRaytraceFastGILightRecord, 16> hardware_light_records_ = {
      "hardware_light_records"};
  uint64_t hardware_light_records_generation_ = 0;
  bool hardware_light_records_valid_ = false;
  int hardware_light_count_ = 0;
  int hardware_local_light_count_ = 0;
  draw::PassSimple hit_eval_count_ps_ = {"Trace.HitEvalCount"};
  draw::PassSimple hit_eval_prefix_ps_ = {"Trace.HitEvalPrefix"};
  draw::PassSimple hit_eval_compact_ps_ = {"Trace.HitEvalCompact"};
  draw::PassSimple hit_eval_ps_ = {"Trace.HitEval"};
  draw::StorageArrayBuffer<uint, 64, true> hit_eval_count_buf_ = {"hit_eval_count_buf"};
  draw::StorageArrayBuffer<uint, 64, true> hit_eval_offset_buf_ = {"hit_eval_offset_buf"};
  draw::StorageArrayBuffer<uint, 64, true> hit_eval_cursor_buf_ = {"hit_eval_cursor_buf"};
  draw::StorageArrayBuffer<uint, 64> hit_eval_resource_id_buf_ = {"hit_eval_resource_id_buf"};
  draw::StorageArrayBuffer<DrawCommandArray, 16, true> hit_eval_indirect_buf_ = {
      "hit_eval_indirect_buf"};
  draw::StorageArrayBuffer<HardwareHitEvalRecord, 1024, true> hit_eval_records_buf_ = {
      "hit_eval_records_buf"};
  uint64_t hit_eval_resource_ids_generation_ = 0;
  bool hit_eval_resource_ids_valid_ = false;
  int hit_eval_resource_ids_count_ = 0;
  draw::Framebuffer hit_eval_fb_ = {"Trace.HitEvalFB"};

 public:
  RayTraceModule(Instance &inst, RayTraceData &data) : inst_(inst), data_(data) {};
  ~RayTraceModule();

  bool use_hardware_tracing_method() const
  {
    return use_raytracing_ && tracing_method_ == RAYTRACE_EEVEE_METHOD_HARDWARE &&
           GPU_viewport_hardware_raytracing_support();
  }
  bool use_hardware_tracing() const
  {
    return use_hardware_tracing_method() && hardware_feature_mask_ != 0;
  }
  uint32_t active_hardware_feature_mask() const
  {
    return hardware_feature_mask_;
  }
  bool use_hardware_shadows() const
  {
    return use_hardware_tracing() &&
           (hardware_feature_mask_ & RAYTRACE_EEVEE_HARDWARE_SHADOWS) != 0;
  }
  const bool *hardware_shadow_ready() const
  {
    return &hardware_shadow_ready_;
  }
  gpu::Texture **hardware_shadow_visibility_tx()
  {
    return &hardware_shadow_visibility_tx_;
  }
  void render_hardware_shadows(View &view, gpu::Texture *depth_tx, int2 extent);
  void render_hardware_direct_light(View &view, gpu::Texture *depth_tx, int2 extent);
  const bool *hardware_direct_replace() const
  {
    return &hardware_direct_replace_;
  }
  gpu::Texture **hardware_direct_closures_tx()
  {
    return &hardware_direct_closures_tx_;
  }

  void init();

  void sync();

  /** Invalidate only NPR history after all scene/view updates, before sampling.step(). */
  void reset_npr_history()
  {
    npr_history_reset_generation_++;
  }

  /**
   * RayTrace the scene and resolve radiance buffer for the corresponding `closure_bit`.
   *
   * IMPORTANT: Should not be conditionally executed as it manages the RayTraceResult.
   * IMPORTANT: The screen tracing will be using the front and back Hierarchical-Z Buffer in its
   * current state.
   *
   * \arg rt_buffer is the layer's permanent storage.
   * \arg active_closures is a mask of all active closures in a deferred layer.
   * \arg render_view is the TAA jittered view.
   * \arg force_no_tracing will run the pipeline without any tracing, relying only on local probes.
   */
  RayTraceResult render(RayTraceBuffer &rt_buffer,
                        gpu::Texture *screen_radiance_back_tx,
                        eClosureBits active_closures,
                        View &render_view);

  RayTraceResult render_phase(RayTraceBuffer &rt_buffer,
                              gpu::Texture *screen_radiance_front_tx,
                              gpu::Texture *screen_radiance_back_tx,
                              eClosureBits active_closures,
                              View &render_view,
                              eHardwareTracePhase phase);

  /** Resolve Nuru's specular-only phase after the main scene has been composited. */
  void render_scene_final_specular(RayTraceBuffer &rt_buffer,
                                   gpu::Texture *scene_radiance_tx,
                                   eClosureBits active_closures,
                                   View &render_view);

  /** Preserve GI and native families while installing the late specular result for NPR/combine. */
  void merge_specular_result(RayTraceBuffer &rt_buffer,
                              RayTraceResult &result,
                              RayTraceResult &specular,
                              View &render_view);

  /**
   * Only allocate the RayTraceResult results buffers to be used by other passes.
   */
  RayTraceResult alloc_only(RayTraceBuffer &rt_buffer);

  /**
   * Only allocate the RayTraceResult results buffers as dummy texture to ensure correct bindings.
   */
  RayTraceResult alloc_dummy(RayTraceBuffer &rt_buffer);

  void debug_pass_sync();
  void debug_draw(View &view, gpu::FrameBuffer *view_fb);

  bool use_raytracing() const
  {
    return use_raytracing_;
  }

  bool use_fast_gi() const
  {
    return use_raytracing() && ray_tracing_options_.trace_max_roughness < 1.0f;
  }

  /**
   * Update screen space thickness parameters.
   * Needs to be called early in the frame to allow ray-cast node usage.
   * Needs uniform_data.push_update() to be called afterward to take effect.
   */
  void thickness_parameters_setup(const float4x4 &winmat, const int2 extent);

 private:
  void free_hardware_rt_scene_cache();
  void invalidate_sorted_hardware_scene_entries_cache();
  const Vector<HardwareRaytraceSceneEntry> &current_sorted_hardware_scene_entries(
      uint64_t sync_generation);
  GPUHardwareRaytraceScene *acquire_hardware_rt_scene(GPUHardwareRaytraceSceneStats *r_stats);
  bool submit_hardware_tracing(View &render_view, int trace_phase);
  bool submit_hardware_hit_lighting(View &render_view, int trace_phase);
  void submit_hardware_denoise_guides();
  bool submit_hardware_denoise(bool shared = false);
  void accumulate_hardware_shared_indirect();
  RayTraceResultTexture resolve_hardware_shared_indirect(RayTraceBuffer &rt_buffer,
                                                         View &render_view);
  void render_hardware_hit_shadows(GPUHardwareRaytraceScene *scene, HardwareHitBuffers &hit);
  void release_hardware_hit_buffers();
  void debug_hardware_hit_payload(const HardwareHitEvalPayload &payload,
                                  int trace_phase,
                                  int payload_index,
                                  const char *stage);
  bool submit_hardware_hit_evaluation(View &render_view,
                                      const HardwareHitEvalPayload &payload,
                                      DispatchIndirectBuf &dispatch,
                                      RayTraceTileBuf &tiles);

  RayTraceResultTexture trace(int closure_index,
                              bool active_layer,
                              RaytraceEEVEE options,
                              RayTraceBuffer &rt_buffer,
                              View &render_view);
};

/** \} */

}  // namespace blender::eevee
