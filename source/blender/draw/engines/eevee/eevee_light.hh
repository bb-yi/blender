/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * The light module manages light data buffers and light culling system.
 *
 * The culling follows the principles of Tiled Culling + Z binning from:
 * "Improved Culling for Tiled and Clustered Rendering"
 * by Michal Drobot
 * http://advances.realtimerendering.com/s2017/2017_Sig_Improved_Culling_final.pdf
 *
 * The culling is separated in 4 compute phases:
 * - View Culling (select pass): Create a z distance and a index buffer of visible lights.
 * - Light sorting: Outputs visible lights sorted by Z distance.
 * - Z binning: Compute the Z bins min/max light indices.
 * - Tile intersection: Fine grained 2D culling of each lights outputting a bitmap per tile.
 */

#pragma once

#include <memory>
#include <string>

#include "DNA_light_types.h"

#include "DRW_gpu_wrapper.hh"

#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "eevee_camera.hh"
#include "eevee_light_shared.hh"
#include "eevee_lightprobe_shared.hh"
#include "eevee_sampling.hh"
#include "eevee_sync.hh"
#include "eevee_telemetry.hh"

namespace blender::eevee {

class Instance;
class ShadowModule;
class ShadowDirectional;
class ShadowPunctual;

/* -------------------------------------------------------------------- */
/** \name Light Object
 * \{ */

using LightCullingDataBuf = draw::StorageBuffer<LightCullingData>;
using LightCullingKeyBuf = draw::StorageArrayBuffer<uint, LIGHT_CHUNK, true>;
using LightCullingTileBuf = draw::StorageArrayBuffer<uint, LIGHT_CHUNK, true>;
using LightCullingZbinBuf = draw::StorageArrayBuffer<uint, CULLING_ZBIN_COUNT, true>;
using LightCullingZdistBuf = draw::StorageArrayBuffer<float, LIGHT_CHUNK, true>;
using LightDataBuf = draw::StorageArrayBuffer<LightData, LIGHT_CHUNK>;
using LightShaderIndexBuf = draw::StorageArrayBuffer<int, LIGHT_CHUNK>;
using SurfelLightShaderBuf = draw::StorageArrayBuffer<float4, LIGHT_CHUNK, true>;
using UniformLightShaderBuf = draw::StorageArrayBuffer<float4, LIGHT_CHUNK, true>;

struct Light : public LightData, NonCopyable {
 public:
  bool initialized = false;
  bool used = false;
  int light_shader_index = -1;
  int front_light_shader_index = -1;
  int volume_light_shader_index = -1;
  int surfel_light_shader_index = -1;
  int uniform_light_shader_index = -1;
  float light_shader_range_scale = 1.0f;

  /** Pointers to source Shadow. Type depends on `LightData::type`. */
  ShadowDirectional *directional = nullptr;
  ShadowPunctual *punctual = nullptr;

  Light()
  {
    /* Avoid valgrind warning. */
    this->type = LIGHT_SUN;
  }

  /* Only used for debugging. */
#ifndef NDEBUG
  Light(Light &&other)
  {
    *static_cast<LightData *>(this) = other;
    this->initialized = other.initialized;
    this->used = other.used;
    this->light_shader_index = other.light_shader_index;
    this->front_light_shader_index = other.front_light_shader_index;
    this->volume_light_shader_index = other.volume_light_shader_index;
    this->surfel_light_shader_index = other.surfel_light_shader_index;
    this->uniform_light_shader_index = other.uniform_light_shader_index;
    this->light_shader_range_scale = other.light_shader_range_scale;
    this->directional = other.directional;
    this->punctual = other.punctual;
    other.directional = nullptr;
    other.punctual = nullptr;
  }

  ~Light()
  {
    BLI_assert(directional == nullptr);
    BLI_assert(punctual == nullptr);
  }
#endif

  void sync(ShadowModule &shadows,
            float4x4 object_to_world,
            char visibility_flag,
            const blender::Light *la,
            const LightLinking *light_linking,
            float light_shader_range_scale,
            float threshold,
            int lightgroup_id = 0);

  void shadow_ensure(ShadowModule &shadows);
  void shadow_discard_safe(ShadowModule &shadows);

  void debug_draw();

 private:
  float shadow_lod_min_get(const blender::Light *la);
  float attenuation_radius_get(const blender::Light *la, float light_threshold, float light_power);
  void shape_parameters_set(const blender::Light *la,
                            const float3 &scale,
                            const float3 &z_axis,
                            float light_shader_range_scale,
                            float threshold,
                            bool use_jitter);
  float shape_radiance_get();
  float point_radiance_get();
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name LightModule
 * \{ */

/**
 * The light module manages light data buffers and light culling system.
 */
class LightModule {
  friend ShadowModule;

 private:
  /* Keep tile count reasonable for memory usage and 2D culling performance. */
  static constexpr uint max_memory_threshold = 32 * 1024 * 1024; /* 32 MiB */
  static constexpr uint max_word_count_threshold = max_memory_threshold / sizeof(uint);
  static constexpr uint max_tile_count_threshold = 8192;

  Instance &inst_;
  /** Map of light objects data. Converted to flat array each frame. */
  Map<ObjectKey, Light> light_map_;
  /**
   * In order to treat the world sun lights the same way as regular lights,
   * an #ObjectKey needs to be associated to each of them.
   * */
  ObjectKey world_sunlight_key_[WORLD_SUN_MAX] = {ObjectKey(WORLD_SUN_DIFFUSE),
                                                  ObjectKey(WORLD_SUN_GLOSSY)};
  /** Flat array sent to GPU, populated from light_map_. Source buffer for light culling. */
  LightDataBuf light_buf_ = {"Lights_no_cull"};
  /** Luminous intensity to consider the light boundary at. Used for culling. */
  float light_threshold_ = 0.01f;
  /** If false, will prevent all scene lights from being synced. */
  bool use_scene_lights_ = false;
  /** If false, will prevent all sun lights from being synced. */
  bool use_sun_lights_ = false;
  /** Number of sun lights synced during the last sync. Used as offset. */
  int sun_lights_len_ = 0;
  int local_lights_len_ = 0;
  /** Sun plus local lights count for convenience. */
  int lights_len_ = 0;
  Map<ObjectKey, std::string> light_names_;
  Vector<TelemetryShadowLightCost> shadow_light_costs_;

  /**
   * Light Culling
   */

  /** LightData buffer used for rendering. Filled by the culling pass. */
  LightDataBuf culling_light_buf_ = {"Lights_culled"};
  LightShaderIndexBuf light_shader_src_index_buf_ = {"LightShader.SrcIndices"};
  LightShaderIndexBuf light_shader_index_buf_ = {"LightShader.CulledIndices"};
  LightShaderIndexBuf front_light_shader_src_index_buf_ = {"FrontLightShader.SrcIndices"};
  LightShaderIndexBuf front_light_shader_index_buf_ = {"FrontLightShader.CulledIndices"};
  LightShaderIndexBuf volume_light_shader_src_index_buf_ = {"VolumeLightShader.SrcIndices"};
  LightShaderIndexBuf volume_light_shader_index_buf_ = {"VolumeLightShader.CulledIndices"};
  LightShaderIndexBuf surfel_light_shader_src_index_buf_ = {"SurfelLightShader.SrcIndices"};
  LightShaderIndexBuf surfel_light_shader_index_buf_ = {"SurfelLightShader.CulledIndices"};
  Texture light_shader_tx_ = {"LightShader"};
  Texture front_light_shader_tx_ = {"FrontLightShader"};
  Texture volume_light_shader_dummy_tx_ = {"VolumeLightShader.Dummy"};
  Texture volume_light_shader_tx_ = {"VolumeLightShader"};
  Vector<std::unique_ptr<Framebuffer>> light_shader_fbs_;
  Vector<std::unique_ptr<Framebuffer>> front_light_shader_fbs_;
  Vector<GPUMaterial *> light_shader_materials_;
  Vector<GPUMaterial *> front_light_shader_materials_;
  Vector<GPUMaterial *> volume_light_shader_materials_;
  Vector<GPUMaterial *> surfel_light_shader_materials_;
  Vector<GPUMaterial *> uniform_light_shader_materials_;
  Vector<LightData> light_shader_lights_;
  Vector<LightData> front_light_shader_lights_;
  Vector<LightData> volume_light_shader_lights_;
  Vector<LightData> surfel_light_shader_lights_;
  Vector<LightData> uniform_light_shader_lights_;
  LightDataBuf light_shader_light_buf_ = {"LightShader.Lights"};
  LightDataBuf front_light_shader_light_buf_ = {"FrontLightShader.Lights"};
  LightDataBuf volume_light_shader_light_buf_ = {"VolumeLightShader.Lights"};
  LightDataBuf surfel_light_shader_light_buf_ = {"SurfelLightShader.Lights"};
  LightDataBuf uniform_light_shader_light_buf_ = {"UniformLightShader.Lights"};
  SurfelLightShaderBuf surfel_light_shader_buf_ = {"SurfelLightShader.Results"};
  UniformLightShaderBuf uniform_light_shader_buf_ = {"UniformLightShader.Results"};
  bool light_shader_valid_ = false;
  bool front_light_shader_valid_ = false;
  bool uniform_light_shader_valid_ = false;
  bool front_light_shader_missing_prepass_reported_ = false;
  bool front_light_shader_needed_ = false;
  bool volume_light_shader_valid_ = false;
  bool surfel_light_shader_valid_ = false;
  bool has_time_dependent_light_shaders_ = false;
  /** Culling information. */
  LightCullingDataBuf culling_data_buf_ = {"LightCull_data"};
  /** Z-distance matching the key for each visible lights. Used for sorting. */
  LightCullingZdistBuf culling_zdist_buf_ = {"LightCull_zdist"};
  /** Key buffer containing only visible lights indices. Used for sorting. */
  LightCullingKeyBuf culling_key_buf_ = {"LightCull_key"};
  /** Zbins containing min and max light index for each Z bin. */
  LightCullingZbinBuf culling_zbin_buf_ = {"LightCull_zbin"};
  /** Bitmap of lights touching each tiles. */
  LightCullingTileBuf culling_tile_buf_ = {"LightCull_tile"};
  /** Culling compute passes. */
  PassSimple culling_ps_ = {"LightCulling"};
  /** Total number of words the tile buffer needs to contain for the render resolution. */
  uint total_word_count_ = 0;
  /** Flipped state of the view being processed. True for planar probe views. */
  bool view_is_flipped_ = false;

  /** Update light on the GPU after culling. Ran for each sample. */
  PassSimple update_ps_ = {"LightUpdate"};
  /** Draw camera-visible light shapes. */
  PassSimple shape_display_ps_ = {"Light.ShapeDisplay"};

  /** Debug Culling visualization. */
  PassSimple debug_draw_ps_ = {"LightCulling.Debug"};

 public:
  LightModule(Instance &inst);
  ~LightModule();

  void begin_sync();
  void sync_light(const ObjectRef &ob_ref);
  void end_sync();
  void sync_render_extent(const int2 render_extent);

  /**
   * Update acceleration structure for the given view.
   */
  void set_view(View &view, const int2 extent);
  void eval_light_shaders(View &view, const int2 extent);
  void eval_front_light_shaders(View &view, const int2 extent);
  void eval_bake_light_shaders(View &view,
                               const int2 extent,
                               Texture &position_tx,
                               Texture &normal_tx);
  void eval_uniform_light_shaders(View &view);
  void sync_volume_light_shaders(const int3 grid_size);
  void eval_volume_light_shaders(View &view, const int3 grid_size);
  void eval_surfel_light_shaders(View &view,
                                 draw::StorageArrayBuffer<Surfel, 64> &surfels_buf,
                                 draw::StorageBuffer<CaptureInfoData> &capture_info_buf,
                                 uint surfel_len);

  void shape_display_draw(View &view, gpu::FrameBuffer *view_fb);
  void debug_draw(View &view, gpu::FrameBuffer *view_fb);

  int light_count() const
  {
    return lights_len_;
  }

  Span<const TelemetryShadowLightCost> shadow_light_costs() const
  {
    return Span<const TelemetryShadowLightCost>(shadow_light_costs_.data(),
                                                shadow_light_costs_.size());
  }

  bool has_time_dependent_light_shaders() const
  {
    return has_time_dependent_light_shaders_;
  }

  bool needs_front_light_shader() const
  {
    return front_light_shader_needed_ && !front_light_shader_materials_.is_empty();
  }

  bool needs_bake_light_shader() const
  {
    return !front_light_shader_materials_.is_empty();
  }

  void tag_front_light_shader_needed()
  {
    front_light_shader_needed_ = true;
  }

  template<typename PassType> void bind_resources(PassType &pass)
  {
    pass.bind_ssbo(LIGHT_CULL_BUF_SLOT, &culling_data_buf_);
    pass.bind_ssbo(LIGHT_BUF_SLOT, &culling_light_buf_);
    pass.bind_ssbo(LIGHT_ZBIN_BUF_SLOT, &culling_zbin_buf_);
    pass.bind_ssbo(LIGHT_TILE_BUF_SLOT, &culling_tile_buf_);
    pass.bind_ubo(WORLD_SUNLIGHT_BUF_SLOT, world_sunlight_ubo());
  }

  template<typename PassType> void bind_light_shader_resources(PassType &pass)
  {
    /* Deferred direct-light eval reuses the prepass based cache. The GBuffer cache path is not
     * stable across the 5.2 framebuffer/resource-table transition. */
    pass.bind_texture(LIGHT_SHADER_TEX_SLOT, &front_light_shader_tx_);
    pass.bind_ssbo(LIGHT_SHADER_INDEX_BUF_SLOT, &front_light_shader_index_buf_);
    pass.bind_ssbo(LIGHT_SHADER_UNIFORM_BUF_SLOT, &uniform_light_shader_buf_);
  }

  template<typename PassType> void bind_front_light_shader_resources(PassType &pass)
  {
    pass.bind_texture(LIGHT_SHADER_TEX_SLOT, &front_light_shader_tx_);
    pass.bind_ssbo(LIGHT_SHADER_INDEX_BUF_SLOT, &front_light_shader_index_buf_);
    pass.bind_ssbo(LIGHT_SHADER_UNIFORM_BUF_SLOT, &uniform_light_shader_buf_);
  }

  template<typename PassType> void bind_npr_front_light_shader_resources(PassType &pass)
  {
    pass.bind_texture(LIGHT_SHADER_NPR_TEX_SLOT, &front_light_shader_tx_);
    pass.bind_ssbo(LIGHT_SHADER_INDEX_BUF_SLOT, &front_light_shader_index_buf_);
    pass.bind_ssbo(LIGHT_SHADER_UNIFORM_BUF_SLOT, &uniform_light_shader_buf_);
  }

  template<typename PassType> void bind_volume_light_shader_resources(PassType &pass)
  {
    pass.bind_texture(LIGHT_SHADER_TEX_SLOT,
                      (!volume_light_shader_valid_ || volume_light_shader_materials_.is_empty() ||
                       !volume_light_shader_tx_.is_valid()) ?
                          &volume_light_shader_dummy_tx_ :
                          &volume_light_shader_tx_);
    pass.bind_ssbo(LIGHT_SHADER_INDEX_BUF_SLOT, &volume_light_shader_index_buf_);
    pass.bind_ssbo(LIGHT_SHADER_UNIFORM_BUF_SLOT, &uniform_light_shader_buf_);
  }

  /**
   * Ensure GPU buffers backing the light shader SSBOs are allocated, even when no lights
   * with light shaders are present. Required because the volume scatter shader struct
   * always includes LightShaderEvalData bindings (LIGHT_SHADER_INDEX_BUF_SLOT,
   * LIGHT_SHADER_UNIFORM_BUF_SLOT). Without this call the VkBuffer handles may remain
   * unregistered in the resource tracker, causing a crash when the render graph builds
   * resource links.
   */
  void ensure_volume_light_shader_resources_allocated()
  {
    volume_light_shader_index_buf_.clear_to_zero();
    uniform_light_shader_buf_.clear_to_zero();
  }

  template<typename PassType> void bind_surfel_light_shader_resources(PassType &pass)
  {
    pass.bind_ssbo(LIGHT_SHADER_SURFEL_INDEX_BUF_SLOT, &surfel_light_shader_index_buf_);
    pass.bind_ssbo(LIGHT_SHADER_SURFEL_BUF_SLOT, &surfel_light_shader_buf_);
    pass.bind_ssbo(LIGHT_SHADER_UNIFORM_BUF_SLOT, &uniform_light_shader_buf_);
  }

 private:
  gpu::UniformBuf *world_sunlight_ubo() const;
  void culling_pass_sync();
  void update_pass_sync();
  void light_shader_pass_sync(const int2 extent);
  void front_light_shader_pass_sync(const int2 extent);
  void uniform_light_shader_pass_sync();
  void volume_light_shader_pass_sync(const int3 grid_size);
  void surfel_light_shader_pass_sync(uint surfel_len);
  void shape_display_pass_sync();
  void debug_pass_sync();
  void culling_extent_sync(const int2 render_extent);

  void add_world_sun_light(const ObjectKey &key, bool use_diffuse, bool use_glossy);
  void update_shadow_light_costs();
  void disable_point_dependent_front_light_shader_indices();
};

/** \} */

}  // namespace blender::eevee
