/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Structures to identify unique data blocks. The keys are unique so we are able to
 * match ids across frame updates.
 */

#pragma once

#include "BKE_duplilist.hh"
#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DRW_render.hh"
#include "GPU_nuru_hardware_raytrace.hh"

#include "draw_handle.hh"
#include "draw_pass.hh"

namespace blender::eevee {

using namespace draw;

class Instance;

/* -------------------------------------------------------------------- */
/** \name Sync Module
 *
 * \{ */

struct BaseHandle {
  const uint recalc = 0;
};

struct ObjectHandle : ObjectRef, BaseHandle {
  const ResourceHandleRange res_handle;

  ObjectHandle(const ObjectRef &ob_ref,
               const ResourceHandleRange &res_handle,
               uint recalc,
               uint sub_key = 0)
      : ObjectRef(ob_ref, sub_key), BaseHandle{recalc}, res_handle(res_handle) {};
};

struct WorldHandle : public BaseHandle {};

struct Material;
struct MaterialPass;

struct HardwareRaytraceSceneEntry {
  ObjectKey object_key;
  /* Replay-safe evaluated object used for sparse hit-eval material replay. */
  Object *hit_eval_object = nullptr;
  blender::gpu::Batch *batch = nullptr;
  int recalc = 0;
  ResourceHandleRange resource_handle = {};
  float4x4 object_to_world = float4x4::identity();
  uint32_t instance_count = 1;
  int material_slot = -1;
  bool is_sculpt = false;
  float3 emissive_radiance = float3(0.0f);
  float3 diffuse_albedo = float3(0.8f);
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
  uint64_t material_runtime_hash = 0;
  /* Compiled during sync, so sparse draws never re-enter material sync with a stale ObjectRef. */
  GPUMaterial *hit_eval_material = nullptr;
  float shadow_terminator_geometry_offset = 0.1f;
  uint32_t shadow_blocker_set = 0;
  bool casts_shadow = true;
  bool visible_raycast = false;
  bool raycast_only_alpha = false;
  GPUHardwareRaytraceShadowAlpha shadow_alpha;
  GPUMaterial *shadow_tint_material = nullptr;
  const blender::Material *raycast_alpha_material = nullptr;
  int raycast_batch_id = 0;
};

class SyncModule {
 private:
  Instance &inst_;
  Vector<HardwareRaytraceSceneEntry> hardware_raytrace_scene_entries_;
  uint64_t hardware_raytrace_scene_signature_ = 0;
  uint64_t hardware_raytrace_sync_generation_ = 0;
  const Object *untraced_shadow_caster_ = nullptr;
  /* Keep draws sharing a material and expansion budget batched together. */
  Map<std::pair<PassMain::Sub *, int>, PassMain::Sub *> tessellation_passes_;
  Map<gpu::Batch *, int> raycast_batch_ids_;
  Map<std::pair<PassMain::Sub *, int>, PassMain::Sub *> raycast_batch_passes_;
  /* Cache only the request schema. Object/instance attribute values are read on every sync. */
  Map<Vector<GPUMaterial *>, Vector<const GPUUniformAttr *>> object_attribute_requests_;
  int64_t object_attribute_request_hits_ = 0;
  int64_t mesh_attribute_requests_ = 0;

 public:
  SyncModule(Instance &inst) : inst_(inst) {};
  ~SyncModule() {};

  void begin_sync();
  void finalize_hardware_raytrace_scene_entries();
  uint64_t hardware_raytrace_sync_generation() const
  {
    return hardware_raytrace_sync_generation_;
  }
  const Vector<HardwareRaytraceSceneEntry> &hardware_raytrace_scene_entries() const
  {
    return hardware_raytrace_scene_entries_;
  }
  uint64_t hardware_raytrace_scene_signature() const
  {
    return hardware_raytrace_scene_signature_;
  }
  const Object *untraced_shadow_caster() const
  {
    return untraced_shadow_caster_;
  }

  int64_t object_attribute_request_count() const
  {
    return object_attribute_requests_.size();
  }
  int64_t object_attribute_request_hits() const
  {
    return object_attribute_request_hits_;
  }
  int64_t mesh_attribute_requests() const
  {
    return mesh_attribute_requests_;
  }

  ObjectHandle sync_object(const ObjectRef &ob_ref,
                           const ResourceHandleRange &res_handle,
                           uint sub_key = 0);

  void sync_mesh(const ObjectRef &ob_ref);
  bool sync_sculpt(const ObjectRef &ob_ref);
  void sync_pointcloud(const ObjectRef &ob_ref);
  void sync_volume(const ObjectRef &ob_ref);
  void sync_curves(const ObjectRef &ob_ref,
                   struct HairParticleInfo const *hair_particle = nullptr);

 private:
  int raycast_batch_id(gpu::Batch *batch);
  void capture_hardware_raytrace_scene_entry(const ObjectHandle &ob_handle,
                                              uint32_t shadow_blocker_set,
                                             const Material &material,
                                             gpu::Batch *geom,
                                             int material_slot,
                                             bool is_sculpt);

  void geometry_call(PassMain::Sub *sub_pass,
                     gpu::Batch *geom,
                     ResourceHandleRange resource_handle,
                     int tessellation_level = 0,
                     GPUMaterial *gpumat = nullptr,
                     bool is_sculpt = false);
  void sync_common_passes(const Material &material,
                          FunctionRef<void(const MaterialPass &)> sync_cb);
  void sync_volume_passes(const ObjectHandle &ob_handle,
                          const Material &material,
                          FunctionRef<void(const MaterialPass &, int)> sync_cb);
  void sync_alpha_blended_passes(const ObjectHandle &ob_handle,
                                 const Material &material,
                                 FunctionRef<void(const MaterialPass &, int)> sync_cb,
                                 bool outline_shell = false);
  void sync_outline_occlusion_passes(const ObjectHandle &ob_handle,
                                     const Material &material,
                                     FunctionRef<void(const MaterialPass &)> sync_cb);

  void sync_common(const ObjectHandle &ob_handle,
                   Span<Material *> materials,
                   Span<GPUMaterial *> gpu_materials);
};

struct HairParticleInfo {
  ModifierData &md;
  ParticleSystem &psys;
  uint recalc_flags;
  uint sub_key;
};

using HairHandleCallback = FunctionRef<void(const HairParticleInfo &)>;
void foreach_hair_particle(Instance &inst, ObjectRef &ob_ref, HairHandleCallback callback);

/** \} */

}  // namespace blender::eevee
