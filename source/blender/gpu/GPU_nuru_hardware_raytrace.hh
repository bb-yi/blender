/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_sys_types.h"

namespace blender {

/** Must match #HWRT_SPECULAR_MAX_BOUNCES in eevee_raytrace_shared.hh. */
static constexpr int GPU_HARDWARE_RAYTRACE_SPECULAR_MAX_BOUNCES = 8;

namespace gpu {
class Batch;
class StorageBuf;
class Texture;
}

enum GPUHardwareRaytraceMaterialEvalPolicy : uint32_t {
  /** Direct-light visibility and shadow rays never replay materials at the occluder hit. */
  GPU_HARDWARE_RAYTRACE_MATERIAL_EVAL_DIRECT_VISIBILITY_ONLY = 0u,
  /** Traversal and continuation use only the sync-time material proxy stored per scene entry. */
  GPU_HARDWARE_RAYTRACE_MATERIAL_EVAL_PROXY_CONTINUATION = 1u,
  /** Full Eevee material replay is reserved for compacted final hit records only. */
  GPU_HARDWARE_RAYTRACE_MATERIAL_EVAL_COMPACT_HIT_REPLAY = 2u,
};

enum GPUHardwareRaytraceMaterialProxySet : uint32_t {
  /** Indirect diffuse GI only needs emissive radiance plus coarse diffuse albedo. */
  GPU_HARDWARE_RAYTRACE_PROXY_INDIRECT_DIFFUSE = 0u,
  /** Direct/specular fallback needs one dominant closure family plus tint/roughness/IOR. */
  GPU_HARDWARE_RAYTRACE_PROXY_DIRECT_AND_SPECULAR = 1u,
};

struct GPUHardwareRaytraceSceneEntry {
  gpu::Batch *batch = nullptr;
  float4x4 object_to_world = float4x4::identity();
  uint32_t instance_count = 1;
  uint32_t user_id = 0;
  /** Indirect diffuse proxy set. See #GPUHardwareRaytraceMaterialProxySet. */
  float3 emissive_radiance = float3(0.0f);
  float3 diffuse_albedo = float3(0.8f);
  /** Direct/specular proxy set. See #GPUHardwareRaytraceMaterialProxySet. */
  float3 reflection_color = float3(0.8f);
  float reflection_roughness = 1.0f;
  float3 transmission_color = float3(0.8f);
  float transmission_roughness = 1.0f;
  float reflection_ior = 1.45f;
  float refraction_ior = 1.45f;
  float packed_thickness = 0.0f;
  float alpha = 1.0f;
  float reflection_layer_coverage = 0.0f;
  uint32_t closure_type = 1u;
  uint32_t proxy_flags = 0u;
  int material_slot = -1;
  bool is_sculpt = false;
};

struct GPUHardwareRaytraceTraceParams {
  /** Full replay is a later sparse stage; traversal itself stays on proxy-only material data. */
  gpu::Texture *ray_data_tx = nullptr;
  gpu::Texture *depth_tx = nullptr;
  gpu::Texture *gbuf_header_tx = nullptr;
  gpu::Texture *gbuf_normal_tx = nullptr;
  gpu::Texture *screen_continuation_tx = nullptr;
  gpu::Texture *world_probe_tx = nullptr;
  gpu::Texture *ray_time_tx = nullptr;
  gpu::Texture *ray_radiance_tx = nullptr;
  gpu::Texture *hit_albedo_tx = nullptr;
  gpu::Texture *hit_throughput_tx = nullptr;
  gpu::Texture *hit_emission_replay_tx = nullptr;
  gpu::Texture *hit_material_tx = nullptr;
  gpu::Texture *hit_normal_tx = nullptr;
  gpu::Texture *hit_position_tx = nullptr;
  gpu::Texture *hit_world_position_tx = nullptr;
  gpu::Texture *hit_identity_tx = nullptr;
  gpu::Texture *hit_barycentric_tx = nullptr;
  /* Nuru Secondary GI: per-pixel receiver GI outputs for mirror-visible diffuse surfaces
   * (main, layered/Principled-metallic, and transmission payload variants). Optional;
   * backends without the dedicated receiver-GI kernel leave them untouched. */
  gpu::Texture *reflected_receiver_gi_tx = nullptr;
  gpu::Texture *layered_receiver_gi_tx = nullptr;
  gpu::Texture *transmission_receiver_gi_tx = nullptr;
  gpu::Texture *layered_receiver_ray_time_tx = nullptr;
  gpu::Texture *layered_receiver_ray_radiance_tx = nullptr;
  gpu::Texture *layered_receiver_albedo_tx = nullptr;
  gpu::Texture *layered_receiver_throughput_tx = nullptr;
  gpu::Texture *layered_receiver_emission_replay_tx = nullptr;
  gpu::Texture *layered_receiver_material_tx = nullptr;
  gpu::Texture *layered_receiver_normal_tx = nullptr;
  gpu::Texture *layered_receiver_position_tx = nullptr;
  gpu::Texture *layered_receiver_world_position_tx = nullptr;
  gpu::Texture *layered_receiver_identity_tx = nullptr;
  gpu::Texture *layered_receiver_barycentric_tx = nullptr;
  gpu::Texture *transmission_receiver_ray_time_tx = nullptr;
  gpu::Texture *transmission_receiver_ray_radiance_tx = nullptr;
  gpu::Texture *transmission_receiver_albedo_tx = nullptr;
  gpu::Texture *transmission_receiver_throughput_tx = nullptr;
  gpu::Texture *transmission_receiver_emission_replay_tx = nullptr;
  gpu::Texture *transmission_receiver_material_tx = nullptr;
  gpu::Texture *transmission_receiver_normal_tx = nullptr;
  gpu::Texture *transmission_receiver_position_tx = nullptr;
  gpu::Texture *transmission_receiver_world_position_tx = nullptr;
  gpu::Texture *transmission_receiver_identity_tx = nullptr;
  gpu::Texture *transmission_receiver_barycentric_tx = nullptr;
  gpu::StorageBuf *dispatch_buf = nullptr;
  gpu::StorageBuf *tiles_coord_buf = nullptr;
  gpu::StorageBuf *light_buf = nullptr;
  /* Nuru NIS: trained cluster-multiplier network parameters (read-only; may be null). */
  gpu::StorageBuf *nis_weights_buf = nullptr;
  /* Nuru NIS: enables the learned multipliers in kernel light sampling. */
  bool nis_enable = false;
  /* Nuru NIS: receiver training feedback ring (entry 0 = atomic counter uint; entries follow
   * as float4(P, luma) + uint4(cluster, 0, 0, 0) pairs). May be null (no feedback). */
  gpu::StorageBuf *nis_feedback_buf = nullptr;
  float4x4 viewinv = float4x4::identity();
  float4x4 wininv = float4x4::identity();
  int2 full_resolution = int2(1);
  int resolution_scale = 1;
  int resolution_scale_denominator = 1;
  int closure_index = 0;
  uint32_t feature_mask = 0;
  int hardware_trace_phase = 0;
  int reflection_bounces = 1;
  int refraction_bounces = 1;
  int2 resolution_bias = int2(0);
  float clamp_indirect = 1.0e10f;
  float4 world_probe_atlas_coord = float4(0.0f, 0.0f, 0.0f, -1.0f);
  /** Reflection/refraction HDRI miss contribution owned by the Full RT specular path. */
  bool use_environment = false;
  /** Diffuse GI world misses are owned by the GI Spatial gather. */
  bool use_diffuse_environment = true;
  int gi_diffuse_sample_count = 8;
  int light_count = 0;
  /* Records [0, local_light_count) are the local lights spanned by the light tree;
   * [local_light_count, light_count) are suns. */
  int local_light_count = 0;
  /* Nuru Secondary GI: per-pixel diffuse final gather at scene-final receiver hits. */
  bool secondary_gi = false;
  int secondary_gi_samples = 2;
  int light_sample_count = 0;
  float4 sampling_rand = float4(0.0f);
};

struct GPUHardwareRaytraceDirectionalShadowParams {
  gpu::Texture *depth_tx = nullptr;
  gpu::Texture *gbuf_header_tx = nullptr;
  gpu::Texture *gbuf_normal_tx = nullptr;
  gpu::Texture *shadow_visibility_tx = nullptr;
  gpu::StorageBuf *world_sunlight_direction_buf = nullptr;
  float4x4 viewinv = float4x4::identity();
  float4x4 wininv = float4x4::identity();
  int2 full_resolution = int2(1);
  int shadow_layer = 0;
  int world_sun_slot = -1;
  float3 light_direction = float3(0.0f, 0.0f, 1.0f);
  float normal_bias = 1.0e-3f;
  float shadow_angle = 0.0f;
  int sample_count = 1;
  /** Nuru: enable curvature-driven caustic focus inside the transparent-shadow loop. */
  bool use_caustics = false;
  /** Nuru: glass transmission color saturation for the tinted shadow (0..1). */
  float color_intensity = 0.5f;
  /** Nuru: multiplier on the caustic peak strength (0.5 subtle .. 10 dramatic, default 1). */
  float photons_intensity = 1.0f;
  /** Nuru: blend opaque HWRT shadows (0) with transparent transmission shadows (1). */
  float shadow_transparency = 1.0f;
  float4 sampling_rand = float4(0.0f);
};

struct GPUHardwareRaytraceDirectionalHitShadowParams {
  gpu::Texture *hit_normal_tx = nullptr;
  gpu::Texture *hit_world_position_tx = nullptr;
  gpu::Texture *hit_identity_tx = nullptr;
  gpu::Texture *shadow_visibility_tx = nullptr;
  gpu::StorageBuf *dispatch_buf = nullptr;
  gpu::StorageBuf *tiles_coord_buf = nullptr;
  gpu::StorageBuf *world_sunlight_direction_buf = nullptr;
  int2 tracing_resolution = int2(1);
  int shadow_layer = 0;
  int world_sun_slot = -1;
  float3 light_direction = float3(0.0f, 0.0f, 1.0f);
  float normal_bias = 1.0e-3f;
  float shadow_angle = 0.0f;
  int sample_count = 1;
  bool use_caustics = false;
  float color_intensity = 0.5f;
  float photons_intensity = 1.0f;
  float shadow_transparency = 1.0f;
  float4 sampling_rand = float4(0.0f);
};

struct GPUHardwareRaytraceLocalShadowParams {
  gpu::Texture *depth_tx = nullptr;
  gpu::Texture *gbuf_header_tx = nullptr;
  gpu::Texture *gbuf_normal_tx = nullptr;
  gpu::Texture *shadow_visibility_tx = nullptr;
  float4x4 viewinv = float4x4::identity();
  float4x4 wininv = float4x4::identity();
  int2 full_resolution = int2(1);
  int shadow_layer = 0;
  uint32_t light_type = 0;
  float3 light_position = float3(0.0f);
  float shadow_radius = 0.0f;
  float3 light_x_axis = float3(1.0f, 0.0f, 0.0f);
  float area_size_x = 0.0f;
  float3 light_y_axis = float3(0.0f, 1.0f, 0.0f);
  float area_size_y = 0.0f;
  float3 shadow_offset = float3(0.0f);
  float area_shadow_scale = 1.0f;
  float normal_bias = 1.0e-3f;
  int sample_count = 1;
  bool use_caustics = false;
  float color_intensity = 0.5f;
  float photons_intensity = 1.0f;
  float shadow_transparency = 1.0f;
  float4 sampling_rand = float4(0.0f);
};

struct GPUHardwareRaytraceLocalHitShadowParams {
  gpu::Texture *hit_normal_tx = nullptr;
  gpu::Texture *hit_world_position_tx = nullptr;
  gpu::Texture *hit_identity_tx = nullptr;
  gpu::Texture *shadow_visibility_tx = nullptr;
  gpu::StorageBuf *dispatch_buf = nullptr;
  gpu::StorageBuf *tiles_coord_buf = nullptr;
  int2 tracing_resolution = int2(1);
  int shadow_layer = 0;
  uint32_t light_type = 0;
  float3 light_position = float3(0.0f);
  float shadow_radius = 0.0f;
  float3 light_x_axis = float3(1.0f, 0.0f, 0.0f);
  float area_size_x = 0.0f;
  float3 light_y_axis = float3(0.0f, 1.0f, 0.0f);
  float area_size_y = 0.0f;
  float3 shadow_offset = float3(0.0f);
  float area_shadow_scale = 1.0f;
  float normal_bias = 1.0e-3f;
  int sample_count = 1;
  bool use_caustics = false;
  float color_intensity = 0.5f;
  float photons_intensity = 1.0f;
  float shadow_transparency = 1.0f;
  float4 sampling_rand = float4(0.0f);
};

struct GPUHardwareRaytraceEnvironmentVisibilityParams {
  gpu::Texture *depth_tx = nullptr;
  gpu::Texture *gbuf_header_tx = nullptr;
  gpu::Texture *gbuf_normal_tx = nullptr;
  gpu::Texture *environment_visibility_tx = nullptr;
  float4x4 viewinv = float4x4::identity();
  float4x4 wininv = float4x4::identity();
  int2 full_resolution = int2(1);
  int sample_count = 1;
  float normal_bias = 1.0e-3f;
  float4 sampling_rand = float4(0.0f);
};

struct GPUHardwareRaytraceHitEnvironmentVisibilityParams {
  gpu::Texture *hit_normal_tx = nullptr;
  gpu::Texture *hit_world_position_tx = nullptr;
  gpu::Texture *environment_visibility_tx = nullptr;
  gpu::StorageBuf *dispatch_buf = nullptr;
  gpu::StorageBuf *tiles_coord_buf = nullptr;
  int2 tracing_resolution = int2(1);
  int sample_count = 1;
  float normal_bias = 1.0e-3f;
  float4 sampling_rand = float4(0.0f);
};

/* Compact light payload for Fast GI direct-light estimation.
 * Keep the layout float4-only so the CPU writer and Metal kernel can share it without depending on
 * Eevee light headers in the GPU module. */
struct GPUHardwareRaytraceFastGILightRecord {
  float4 object_to_world_x = float4(1.0f, 0.0f, 0.0f, 0.0f);
  float4 object_to_world_y = float4(0.0f, 1.0f, 0.0f, 0.0f);
  float4 object_to_world_z = float4(0.0f, 0.0f, 1.0f, 0.0f);
  float4 color_diffuse_power = float4(0.0f);
  float4 direction_type = float4(0.0f, 0.0f, 1.0f, 0.0f);
  float4 attenuation_spot = float4(0.0f);
  float4 spot_size_inv = float4(0.0f);
};

/* Nuru light tree (many-light sampling, Stage A). Classic published light-hierarchy importance
 * sampling (Conty&Kulla 2018 adaptive tree splitting metrics / Yuksel stochastic lightcuts
 * descent); intentionally NO reservoirs and NO spatiotemporal sample reuse. Nodes are appended
 * to the light record buffer after the light records, encoded into the first three float4 lanes
 * of a record-sized slot so every existing binding/dispatch path carries the tree for free:
 * - object_to_world_x = bounding sphere center.xyz, radius (w)
 * - object_to_world_y = orientation cone axis.xyz, cos(theta_o) (w; -2.0 = omnidirectional)
 * - object_to_world_z = (power, left child, right child, leaf light record index); children and
 *   leaf index are float-encoded integers, -1 when absent. Internal nodes have leaf index -1.
 * The tree spans LOCAL lights only (records [0, local_light_count)); suns keep their records in
 * [local_light_count, light_count) and are sampled as a separate top-level group. For
 * local_light_count > 0 the node count is exactly 2 * local_light_count - 1, with node 0 as the
 * root. */

struct GPUHardwareRaytraceOIDNDenoiseParams {
  gpu::Texture *input_radiance_tx = nullptr;
  gpu::Texture *output_radiance_tx = nullptr;
  gpu::Texture *albedo_tx = nullptr;
  gpu::Texture *normal_tx = nullptr;
  int2 extent = int2(1);
  bool use_albedo = false;
  bool use_normal = false;
  bool use_gpu = true;
  /* Route the filter step through the NVIDIA OptiX denoiser instead of the OIDN filter.
   * Vulkan/NVIDIA only; pack/unpack kernels, shared buffers, and call sites are unchanged.
   * Backends without OptiX support ignore this and run OIDN. */
  bool use_optix_denoiser = false;
  int quality = 2;
  int prefilter = 2;
};

struct GPUHardwareRaytraceSceneStats {
  int geometry_count = 0;
  int instance_count = 0;
  int built_blas_count = 0;
  int emissive_light_count = 0;
  float emissive_energy_sum = 0.0f;
  bool built_scene = false;
};

struct GPUHardwareRaytraceSceneUpdateParams {
  bool update_tlas = true;
  bool update_emissive_data = true;
  bool update_material_data = true;
  bool update_world_geometry_data = true;
  /* Entry indices whose geometry changed: their BLAS is rebuilt from the entry's current batch
   * and the per-entry triangle data refreshed, while every other BLAS is reused. Editing one
   * object then costs one BLAS build plus a TLAS rebuild instead of a full scene rebuild.
   * Implies TLAS and world-geometry refreshes (the caller must keep those flags enabled). */
  Span<int> rebuild_blas_indices;
};

struct GPUHardwareRaytraceScene;

GPUHardwareRaytraceScene *GPU_hardware_raytrace_scene_build(
    Span<GPUHardwareRaytraceSceneEntry> entries, GPUHardwareRaytraceSceneStats *r_stats = nullptr);
bool GPU_hardware_raytrace_scene_update(GPUHardwareRaytraceScene *scene,
                                     Span<GPUHardwareRaytraceSceneEntry> entries,
                                     const GPUHardwareRaytraceSceneUpdateParams &update_params,
                                     GPUHardwareRaytraceSceneStats *r_stats = nullptr);
bool GPU_hardware_raytrace_scene_trace(GPUHardwareRaytraceScene *scene,
                                    const GPUHardwareRaytraceTraceParams &params);
bool GPU_hardware_raytrace_scene_trace_directional_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceDirectionalShadowParams &params);
bool GPU_hardware_raytrace_scene_trace_local_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceLocalShadowParams &params);
bool GPU_hardware_raytrace_scene_trace_directional_hit_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceDirectionalHitShadowParams &params);
bool GPU_hardware_raytrace_scene_trace_local_hit_shadow(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceLocalHitShadowParams &params);
bool GPU_hardware_raytrace_scene_shadow_batch_begin(GPUHardwareRaytraceScene *scene);
bool GPU_hardware_raytrace_scene_shadow_batch_end(GPUHardwareRaytraceScene *scene);
bool GPU_hardware_raytrace_scene_trace_environment_visibility(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceEnvironmentVisibilityParams &params);
bool GPU_hardware_raytrace_scene_trace_hit_environment_visibility(
    GPUHardwareRaytraceScene *scene, const GPUHardwareRaytraceHitEnvironmentVisibilityParams &params);
bool GPU_hardware_raytrace_denoise_oidn(const GPUHardwareRaytraceOIDNDenoiseParams &params);
void GPU_hardware_raytrace_scene_free(GPUHardwareRaytraceScene *scene);

}  // namespace blender
