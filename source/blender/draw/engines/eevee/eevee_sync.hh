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

class SyncModule {
 private:
  Instance &inst_;
  /* Keep draws sharing a material and expansion budget batched together. */
  Map<std::pair<PassMain::Sub *, int>, PassMain::Sub *> tessellation_passes_;
  /* Cache only the request schema. Object/instance attribute values are read on every sync. */
  Map<Vector<GPUMaterial *>, Vector<const GPUUniformAttr *>> object_attribute_requests_;
  int64_t object_attribute_request_hits_ = 0;
  int64_t mesh_attribute_requests_ = 0;

 public:
  SyncModule(Instance &inst) : inst_(inst) {};
  ~SyncModule() {};

  void begin_sync()
  {
    tessellation_passes_.clear();
    object_attribute_requests_.clear();
    object_attribute_request_hits_ = 0;
    mesh_attribute_requests_ = 0;
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
  void geometry_call(PassMain::Sub *sub_pass,
                     gpu::Batch *geom,
                     ResourceHandleRange resource_handle,
                     int tessellation_level = 0);
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
