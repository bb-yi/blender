/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * The velocity pass outputs motion vectors to use for either
 * temporal re-projection or motion blur.
 *
 * It is the module that tracks the objects between frames updates.
 */

#include <algorithm>
#include <limits>

#include "BKE_duplilist.hh"
#include "BKE_object.hh"
#include "BLI_map.hh"
#include "BLI_time.h"
#include "DEG_depsgraph_query.hh"
#include "DNA_modifier_types.h"
#include "DNA_particle_types.h"
#include "DNA_rigidbody_types.h"

#include "DRW_engine.hh"
#include "GPU_capabilities.hh"
#include "GPU_context.hh"
#include "draw_cache.hh"
#include "draw_cache_impl.hh"

#include "eevee_instance.hh"
// #include "eevee_renderpasses.hh"
#include "eevee_shader.hh"
#include "eevee_velocity.hh"
#include "eevee_velocity_shared.hh"

#include "draw_common.hh"

namespace blender::eevee {

/* Keep allocation decisions local to velocity. In particular, a snapshot buffer must not shrink
 * on every small topology change, and its history must never be discarded before consumption. */
static int64_t velocity_buffer_capacity(const int64_t used,
                                        const int64_t capacity,
                                        const int64_t minimum,
                                        const int64_t maximum,
                                        uint32_t &low_usage_count)
{
  BLI_assert(used >= 0 && used <= maximum && minimum <= maximum);
  const int64_t required = std::max(used, minimum);
  bool resize = required > capacity;
  if (resize || used >= capacity / 4 + int64_t(capacity % 4 != 0)) {
    low_usage_count = 0;
  }
  else if (++low_usage_count >= 120) {
    resize = true;
    low_usage_count = 0;
  }
  if (!resize) {
    return capacity;
  }
  int64_t target = minimum;
  while (target < required) {
    /* The device limit need not be a power of two. Do not round past it or overflow. */
    target = target > maximum / 2 ? maximum : target * 2;
  }
  return target;
}

template<typename T>
static uint64_t velocity_buffer_upload(draw::StorageArrayBuffer<T, 16> &buffer, const int64_t used)
{
  /* Vulkan also uses this prefix as the descriptor range. Keep a valid, initialized dummy
   * element when there is no history, and include every slot referenced by history indices. */
  if (used == 0) {
    buffer[0] = T{};
  }
  const int64_t upload_size = GPU_backend_get_type() == GPU_BACKEND_VULKAN ?
                                  std::max<int64_t>(used, 1) :
                                  buffer.size();
  if (GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    GPU_storagebuf_usage_size_set(buffer, uint64_t(upload_size) * sizeof(T));
  }
  buffer.push_update();
  return uint64_t(upload_size) * sizeof(T);
}

/* -------------------------------------------------------------------- */
/** \name VelocityModule
 *
 * \{ */

void VelocityModule::init()
{
  runtime_stats_enabled_ = inst_.telemetry.enabled();
  runtime_stats_ = {};
  if (runtime_stats_enabled_) {
    for (const VelocityGeometryBuf *buffer : geometry_steps) {
      runtime_stats_.geometry_capacity_bytes += uint64_t(buffer->size()) * sizeof(float4);
    }
    runtime_stats_.indirection_capacity_bytes = uint64_t(indirection_buf.size()) *
                                                sizeof(VelocityIndex);
  }
  if (!inst_.is_viewport() && !inst_.is_baking() &&
      (inst_.film.render_buffer_passes_get() & EEVEE_RENDER_PASS_VECTOR) &&
      !inst_.motion_blur.postfx_enabled())
  {
    /* No motion blur and the vector pass was requested. Do the steps sync here. */
    const Scene *scene = inst_.scene;
    float initial_time = scene->r.cfra + scene->r.subframe;
    step_sync(STEP_PREVIOUS, initial_time - 1.0f);
    /* NR consumes backward motion only; avoid evaluating and copying future geometry. */
    if (!inst_.is_dlss_guide_render) {
      step_sync(STEP_NEXT, initial_time + 1.0f);
    }
    /* Let the main sync loop handle the current step. */
    inst_.set_time(initial_time);
    step_ = STEP_CURRENT;
  }

  /* For viewport, only previous motion is supported.
   * Still bind previous step to avoid undefined behavior. */
  next_step_ = (inst_.is_viewport() || inst_.is_baking() || inst_.is_dlss_guide_render) ?
                   STEP_PREVIOUS :
                   STEP_NEXT;
}

/* Similar to Instance::object_sync, but only syncs velocity. */
static void step_object_sync_render(Instance &inst, ObjectRef &ob_ref)
{
  Object *ob = ob_ref.object;

  const bool is_velocity_type = ELEM(ob->type, OB_CURVES, OB_MESH, OB_POINTCLOUD);
  const int ob_visibility = DRW_object_visibility_in_active_context(ob);
  const bool partsys_is_visible = (ob_visibility & OB_VISIBLE_PARTICLES) != 0 &&
                                  (ob->type == OB_MESH);
  const bool object_is_visible = DRW_object_is_renderable(ob) &&
                                 (ob_visibility & OB_VISIBLE_SELF) != 0;

  if (!is_velocity_type || (!partsys_is_visible && !object_is_visible)) {
    return;
  }

  /* NOTE: Dummy resource handle since this won't be used for drawing. */
  ResourceHandleRange resource_handle = {};

  if (partsys_is_visible) {
    auto sync_hair = [&](const HairParticleInfo &info) {
      ObjectHandle ob_handle(ob_ref, resource_handle, info.recalc_flags, info.sub_key);
      inst.velocity.step_object_sync(ob_handle, &info);
    };
    foreach_hair_particle(inst, ob_ref, sync_hair);
  };

  if (object_is_visible) {
    ObjectHandle ob_handle = inst.sync.sync_object(ob_ref, resource_handle);
    inst.velocity.step_object_sync(ob_handle);
  }
}

void VelocityModule::step_sync(eVelocityStep step, float time)
{
  inst_.set_time(time);
  step_ = step;
  object_steps_usage[step_] = 0;
  step_camera_sync();

  DRW_render_object_iter(
      inst_.render, inst_.depsgraph, [&](draw::ObjectRef &ob_ref, RenderEngine *, Depsgraph *) {
        step_object_sync_render(inst_, ob_ref);
      });

  geometry_steps_fill();
}

void VelocityModule::step_camera_sync()
{
  inst_.camera.sync();
  *camera_steps[step_] = inst_.camera.data_get();
  step_time[step_] = inst_.scene->r.cfra + inst_.scene->r.subframe;
  /* Fix undefined camera steps when rendering is starting. */
  if ((step_ == STEP_CURRENT) && (camera_steps[STEP_PREVIOUS]->initialized == false)) {
    *camera_steps[STEP_PREVIOUS] = *static_cast<CameraData *>(camera_steps[step_]);
    camera_steps[STEP_PREVIOUS]->initialized = true;
    step_time[STEP_PREVIOUS] = step_time[step_];
  }
}

bool VelocityModule::step_object_sync(const ObjectHandle &ob_handle,
                                      HairParticleInfo const *hair_particle /*=nullptr*/)
{
  bool has_motion = object_has_velocity(ob_handle.object) ||
                    (ob_handle.recalc & ID_RECALC_TRANSFORM);
  /* NOTE: Fragile. This will only work with 1 frame of lag since we can't record every geometry
   * just in case there might be an update the next frame. */
  bool has_deform = object_is_deform(ob_handle.object) || (ob_handle.recalc & ID_RECALC_GEOMETRY);

  if (!has_motion && !has_deform) {
    return false;
  }

  /* While VelocityObjectData is unique for each object/instance, multiple VelocityObjectDatas
   * can point to the same offset in VelocityGeometryData, since geometry is stored local space. */
  uint64_t velocity_id = hair_particle ? uint64_t(&hair_particle->psys) :
                                         uint64_t(ob_handle.object->data);

  /* Geometry motion. */
  if (has_deform) {
    auto add_cb = [&]() {
      VelocityGeometryData data;
      if (hair_particle) {
        data.pos_buf = draw::hair_pos_buffer_get(
            inst_.scene, ob_handle.object, &hair_particle->psys, &hair_particle->md);
        return data;
      }
      switch (ob_handle.object->type) {
        case OB_CURVES:
          data.pos_buf = draw::curves_pos_buffer_get(ob_handle.object);
          break;
        case OB_POINTCLOUD:
          data.pos_buf = DRW_pointcloud_position_and_radius_buffer_get(ob_handle.object);
          break;
        case OB_MESH:
          data.pos_buf = DRW_cache_mesh_surface_get(ob_handle.object);
          break;
        default:
          break;
      }
      return data;
    };

    const VelocityGeometryData &data = geometry_map.lookup_or_add_cb(velocity_id, add_cb);
    has_deform = data.has_data();
  }

  bool any_instance_has_motion = false;
  for (int i : IndexRange(ob_handle.instances_count())) {
    /* Object motion. */
    /* FIXME(fclem) As we are using original objects pointers, there is a chance the previous
     * object key matches a totally different object if the scene was changed by user or python
     * callback. In this case, we cannot correctly match objects between updates.
     * What this means is that there will be incorrect motion vectors for these objects.
     * We live with that until we have a correct way of identifying new objects. */
    VelocityObjectData &vel = velocity_map.lookup_or_add_default(ObjectKey(ob_handle, i));
    vel.obj.ofs[step_] = object_steps_usage[step_]++;
    if (ob_handle.res_handle.is_valid()) {
      vel.obj.resource_index = ob_handle.res_handle.sub_handle(i).index();
    }
    else {
      /* Called from `step_object_sync_render`. */
      vel.obj.resource_index = 0;
    }
    vel.id = velocity_id;
    object_steps[step_]->get_or_resize(vel.obj.ofs[step_]) = ob_handle.object_to_world(i);
    if (step_ == STEP_CURRENT) {
      /* Replace invalid steps. Can happen if object was hidden in one of those steps. */
      if (vel.obj.ofs[STEP_PREVIOUS] == -1) {
        vel.obj.ofs[STEP_PREVIOUS] = object_steps_usage[STEP_PREVIOUS]++;
        object_steps[STEP_PREVIOUS]->get_or_resize(
            vel.obj.ofs[STEP_PREVIOUS]) = ob_handle.object_to_world(i);
      }
      if (inst_.is_dlss_guide_render) {
        vel.obj.ofs[STEP_NEXT] = vel.obj.ofs[STEP_PREVIOUS];
      }
      if (vel.obj.ofs[STEP_NEXT] == -1) {
        if (inst_.is_viewport()) {
          /* Just set it to 0. motion.next is not meant to be valid in the viewport. */
          vel.obj.ofs[STEP_NEXT] = 0;
        }
        else {
          vel.obj.ofs[STEP_NEXT] = object_steps_usage[STEP_NEXT]++;
          object_steps[STEP_NEXT]->get_or_resize(
              vel.obj.ofs[STEP_NEXT]) = ob_handle.object_to_world(i);
        }
      }
    }

    /* Avoid drawing object that has no motions but were tagged as such. */
    if (step_ == STEP_CURRENT && has_motion == true && has_deform == false) {
      const float4x4 &obmat_curr = (*object_steps[STEP_CURRENT])[vel.obj.ofs[STEP_CURRENT]];
      const float4x4 &obmat_prev = (*object_steps[STEP_PREVIOUS])[vel.obj.ofs[STEP_PREVIOUS]];
      if (inst_.is_viewport() || inst_.is_dlss_guide_render) {
        any_instance_has_motion |= obmat_curr != obmat_prev;
      }
      else {
        const float4x4 &obmat_next = (*object_steps[STEP_NEXT])[vel.obj.ofs[STEP_NEXT]];
        any_instance_has_motion |= (obmat_curr != obmat_prev) || (obmat_curr != obmat_next);
      }
    }

#if 0
    if (!has_motion && !has_deform) {
      printf("Detected no motion on %s (%d)\n", ob_handle.object->id.name, i);
    }
    if (has_deform) {
      printf("Geometry Motion on %s (%d)\n", ob_handle.object->id.name, i);
    }
    if (has_motion) {
      printf("Object Motion on %s (%d)\n", ob_handle.object->id.name, i);
    }
#endif
  }

  return has_deform || any_instance_has_motion;
}

void VelocityModule::geometry_steps_fill()
{
  const double start_time = runtime_stats_enabled_ ? BLI_time_now_seconds() : 0.0;
  /* Offsets/lengths are signed shader integers; the copy API has unsigned byte offsets/sizes. */
  const uint64_t maximum = std::min({uint64_t(GPU_max_storage_buffer_size() / sizeof(float4)),
                                     uint64_t(std::numeric_limits<int>::max()),
                                     uint64_t(std::numeric_limits<uint>::max() / sizeof(float4))});
  uint64_t dst_ofs = 0;
  bool overflow = maximum < 16;
  for (VelocityGeometryData &geom : geometry_map.values()) {
    gpu::VertBuf *pos_buf = geom.pos_buf_get();
    if (!pos_buf) {
      continue;
    }
    const uint64_t src_len = GPU_vertbuf_get_vertex_len(pos_buf);
    if (src_len > maximum - dst_ofs) {
      overflow = true;
      break;
    }
    geom.len = int(src_len);
    geom.ofs = int(dst_ofs);
    dst_ofs += src_len;
  }

  /* Fallback when velocity buffer could not be allocated.  */
  if (overflow) {
    inst_.info_append_i18n(
        "Error: Could not allocate velocity geometry buffer. Only object and camera velocities "
        "will be used.");
    for (VelocityObjectData &vel : velocity_map.values()) {
      vel.geo.len[step_] = -1;
      vel.geo.ofs[step_] = -1;
      vel.id = 0;
    }
    geometry_map.clear();
    if (runtime_stats_enabled_) {
      runtime_stats_.snapshot_cpu_ms += (BLI_time_now_seconds() - start_time) * 1000.0;
    }
    return;
  }
  const int64_t old_capacity = geometry_steps[step_]->size();
  const int64_t capacity = velocity_buffer_capacity(
      int64_t(dst_ofs), old_capacity, 16, int64_t(maximum), geometry_low_usage_count_[step_]);
  geometry_steps[step_]->resize(capacity);
  if (runtime_stats_enabled_) {
    runtime_stats_.geometry_reallocations += capacity != old_capacity;
    runtime_stats_.geometry_shrinks += capacity < old_capacity;
    runtime_stats_.geometry_capacity_bytes = uint64_t(geometry_steps[0]->size() +
                                                      geometry_steps[1]->size() +
                                                      geometry_steps[2]->size()) *
                                             sizeof(float4);
    runtime_stats_.snapshot_vertices += dst_ofs;
    runtime_stats_.snapshot_copy_bytes += dst_ofs * sizeof(float4);
  }

  if (GPU_backend_get_type() == GPU_BACKEND_VULKAN) {
    GPU_storagebuf_usage_size_set(*geometry_steps[step_],
                                  std::max<uint64_t>(dst_ofs, 1) * sizeof(float4));
    if (dst_ofs == 0) {
      /* No shader will read this slot, but a zero-sized Vulkan descriptor is invalid. */
      const float4 dummy(0.0f);
      GPU_storagebuf_update(*geometry_steps[step_], &dummy);
      if (runtime_stats_enabled_) {
        runtime_stats_.upload_bytes += sizeof(dummy);
      }
    }
  }

  DRW_submission_start();

  PassSimple copy_ps("Velocity Copy Pass");
  copy_ps.init();
  copy_ps.state_set(DRW_STATE_NO_DRAW);
  copy_ps.shader_set(inst_.shaders.static_shader_get(VERTEX_COPY));
  copy_ps.bind_ssbo("out_buf", *geometry_steps[step_]);

  for (VelocityGeometryData &geom : geometry_map.values()) {
    gpu::VertBuf *pos_buf = geom.pos_buf_get();
    if (!pos_buf || geom.len == 0) {
      continue;
    }
    const GPUVertFormat *format = GPU_vertbuf_get_format(pos_buf);
    if (format->stride == 16) {
      GPU_storagebuf_copy_sub_from_vertbuf(*geometry_steps[step_],
                                           pos_buf,
                                           geom.ofs * sizeof(float4),
                                           0,
                                           geom.len * sizeof(float4));
    }
    else {
      BLI_assert(format->stride % 4 == 0);
      copy_ps.bind_ssbo("in_buf", pos_buf);
      copy_ps.push_constant("start_offset", geom.ofs);
      copy_ps.push_constant("vertex_stride", int(format->stride / 4));
      copy_ps.push_constant("vertex_count", geom.len);
      uint group_len_x = divide_ceil_u(geom.len, VERTEX_COPY_GROUP_SIZE);
      uint verts_per_thread = divide_ceil_u(group_len_x, GPU_max_work_group_count(0));
      copy_ps.dispatch(int3(group_len_x / verts_per_thread, 1, 1));
    }
  }

  copy_ps.barrier(GPU_BARRIER_SHADER_STORAGE);
  inst_.manager->submit(copy_ps);

  DRW_submission_end();

  /* Copy back the #VelocityGeometryIndex into #VelocityObjectData which are
   * indexed using persistent keys (unlike geometries which are indexed by volatile ID). */
  for (VelocityObjectData &vel : velocity_map.values()) {
    const VelocityGeometryData &geom = geometry_map.lookup_default(vel.id, VelocityGeometryData());
    vel.geo.len[step_] = geom.len;
    vel.geo.ofs[step_] = geom.ofs;
    /* Avoid reuse. */
    vel.id = 0;
  }

  geometry_map.clear();
  if (runtime_stats_enabled_) {
    runtime_stats_.snapshot_cpu_ms += (BLI_time_now_seconds() - start_time) * 1000.0;
  }
}

void VelocityModule::step_swap()
{
  auto swap_steps = [&](eVelocityStep step_a, eVelocityStep step_b) {
    std::swap(object_steps[step_a], object_steps[step_b]);
    std::swap(geometry_steps[step_a], geometry_steps[step_b]);
    std::swap(geometry_low_usage_count_[step_a], geometry_low_usage_count_[step_b]);
    std::swap(camera_steps[step_a], camera_steps[step_b]);
    std::swap(step_time[step_a], step_time[step_b]);
    std::swap(object_steps_usage[step_a], object_steps_usage[step_b]);

    for (VelocityObjectData &vel : velocity_map.values()) {
      vel.obj.ofs[step_a] = vel.obj.ofs[step_b];
      vel.obj.ofs[step_b] = uint(-1);
      vel.geo.ofs[step_a] = vel.geo.ofs[step_b];
      vel.geo.len[step_a] = vel.geo.len[step_b];
      vel.geo.ofs[step_b] = uint(-1);
      vel.geo.len[step_b] = uint(-1);
    }
  };

  if (inst_.is_viewport()) {
    geometry_steps_fill();
    /* For viewport we only use the last rendered redraw as previous frame.
     * We swap current with previous step at the end of a redraw.
     * We do not support motion blur as it is rendered to avoid conflicting motions
     * for temporal reprojection. */
    swap_steps(eVelocityStep::STEP_PREVIOUS, eVelocityStep::STEP_CURRENT);
  }
  else {
    /* Render case: The STEP_CURRENT is left untouched. */
    swap_steps(eVelocityStep::STEP_PREVIOUS, eVelocityStep::STEP_NEXT);
  }
}

void VelocityModule::begin_sync()
{
  step_ = STEP_CURRENT;
  step_camera_sync();
  object_steps_usage[step_] = 0;

  /* STEP_NEXT is not used for viewport. (See #131134) */
  BLI_assert(!inst_.is_viewport() || object_steps_usage[STEP_NEXT] == 0);
}

void VelocityModule::end_sync()
{
  Vector<ObjectKey, 0> deleted_obj;

  uint32_t max_resource_index_ = 0u;

  for (MapItem<ObjectKey, VelocityObjectData> item : velocity_map.items()) {
    if (item.value.obj.resource_index == uint32_t(-1)) {
      deleted_obj.append(item.key);
    }
    else {
      max_resource_index_ = max_uu(max_resource_index_, item.value.obj.resource_index);
    }
  }

  for (auto &key : deleted_obj) {
    velocity_map.remove(key);
  }

  const int64_t indirection_used = int64_t(max_resource_index_) + 1;
  const int64_t old_capacity = indirection_buf.size();
  const int64_t maximum = int64_t(std::min<uint64_t>(
      GPU_max_storage_buffer_size() / sizeof(VelocityIndex), std::numeric_limits<int64_t>::max()));
  BLI_assert(indirection_used <= maximum);
  /* If the scene's resource table itself exceeds the device limit, retain the previous exact-size
   * behavior. This must not turn the bounded power-of-two calculation into an overflow or loop. */
  const int64_t capacity = indirection_used > maximum ?
                               indirection_used :
                               velocity_buffer_capacity(indirection_used,
                                                        old_capacity,
                                                        std::min<int64_t>(128, maximum),
                                                        maximum,
                                                        indirection_low_usage_count_);
  indirection_buf.resize(capacity);
  indirection_initialized_size_ = std::min(indirection_initialized_size_, capacity);
  if (indirection_initialized_size_ < indirection_used) {
    /* Sparse resource indices may leave holes. Initialize each host slot once so the Vulkan
     * upload prefix is fully initialized without clearing the whole retained capacity each frame.
     */
    indirection_buf.as_span()
        .slice(indirection_initialized_size_, indirection_used - indirection_initialized_size_)
        .fill(VelocityIndex{});
    indirection_initialized_size_ = indirection_used;
  }
  if (runtime_stats_enabled_) {
    runtime_stats_.indirection_reallocations += capacity != old_capacity;
    runtime_stats_.indirection_shrinks += capacity < old_capacity;
    runtime_stats_.indirection_capacity_bytes = uint64_t(capacity) * sizeof(VelocityIndex);
  }

  /* Avoid uploading more data to the GPU as well as an extra level of
   * indirection on the GPU by copying back offsets the to VelocityIndex. */
  for (VelocityObjectData &vel : velocity_map.values()) {
    /* Disable deform if vertex count mismatch. */
    if (inst_.is_viewport()) {
      /* Current geometry step will be copied at the end of the frame.
       * Thus vel.geo.len[STEP_CURRENT] is not yet valid and the current length is manually
       * retrieved. */
      gpu::VertBuf *pos_buf =
          geometry_map.lookup_default(vel.id, VelocityGeometryData()).pos_buf_get();
      vel.geo.do_deform = pos_buf != nullptr &&
                          (vel.geo.len[STEP_PREVIOUS] == GPU_vertbuf_get_vertex_len(pos_buf));
    }
    else {
      if (inst_.is_dlss_guide_render) {
        /* Both next-step buffer bindings alias PREVIOUS in the guide path. */
        vel.geo.ofs[STEP_NEXT] = vel.geo.ofs[STEP_PREVIOUS];
        vel.geo.len[STEP_NEXT] = vel.geo.len[STEP_PREVIOUS];
      }
      vel.geo.do_deform = (vel.geo.len[STEP_CURRENT] != 0) &&
                          (vel.geo.len[STEP_CURRENT] == vel.geo.len[STEP_PREVIOUS]) &&
                          (vel.geo.len[STEP_CURRENT] == vel.geo.len[STEP_NEXT]);
    }
    indirection_buf[vel.obj.resource_index] = vel;
    /* Reset for next sync. */
    vel.obj.resource_index = uint(-1);
  }

  uint64_t upload_bytes = velocity_buffer_upload(*object_steps[STEP_PREVIOUS],
                                                 object_steps_usage[STEP_PREVIOUS]);
  if (!inst_.is_dlss_guide_render) {
    upload_bytes += velocity_buffer_upload(*object_steps[STEP_NEXT],
                                           object_steps_usage[STEP_NEXT]);
  }
  camera_steps[STEP_PREVIOUS]->push_update();
  camera_steps[STEP_CURRENT]->push_update();
  if (!inst_.is_dlss_guide_render) {
    camera_steps[STEP_NEXT]->push_update();
  }
  upload_bytes += velocity_buffer_upload(indirection_buf, indirection_used);
  if (runtime_stats_enabled_) {
    runtime_stats_.upload_bytes += upload_bytes;
  }
}

bool VelocityModule::object_has_velocity(const Object *ob)
{
#if 0
  RigidBodyOb *rbo = ob->rigidbody_object;
  /* Active rigidbody objects only, as only those are affected by sim. */
  const bool has_rigidbody = (rbo && (rbo->type == RBO_TYPE_ACTIVE));
  /* For now we assume dupli objects are moving. */
  const bool is_dupli = (ob->base_flag & BASE_FROM_DUPLI) != 0;
  const bool object_moves = is_dupli || has_rigidbody || BKE_object_moves_in_time(ob, true);
#else
  UNUSED_VARS(ob);
  /* BKE_object_moves_in_time does not work in some cases.
   * Better detect non moving object after evaluation. */
  const bool object_moves = true;
#endif
  return object_moves;
}

bool VelocityModule::object_is_deform(const Object *ob)
{
  RigidBodyOb *rbo = ob->rigidbody_object;
  /* Active rigidbody objects only, as only those are affected by sim. */
  const bool has_rigidbody = (rbo && (rbo->type == RBO_TYPE_ACTIVE));
  const bool is_deform = BKE_object_is_deform_modified(inst_.scene, const_cast<Object *>(ob)) ||
                         (has_rigidbody && (rbo->flag & RBO_FLAG_USE_DEFORM) != 0);

  return is_deform;
}

bool VelocityModule::camera_has_motion() const
{
  /* Only valid after sync. */
  if (inst_.is_viewport() || inst_.is_dlss_guide_render) {
    /* These paths have no next step. */
    return *camera_steps[STEP_PREVIOUS] != *camera_steps[STEP_CURRENT];
  }
  return *camera_steps[STEP_PREVIOUS] != *camera_steps[STEP_CURRENT] &&
         *camera_steps[STEP_NEXT] != *camera_steps[STEP_CURRENT];
}

bool VelocityModule::camera_changed_projection() const
{
  /* Only valid after sync. */
  if (inst_.is_viewport()) {
    return camera_steps[STEP_PREVIOUS]->type != camera_steps[STEP_CURRENT]->type;
  }
  /* Cannot happen in render mode since we set the type during the init phase. */
  return false;
}

float VelocityModule::step_time_delta_get(eVelocityStep start, eVelocityStep end) const
{
  return step_time[end] - step_time[start];
}

/** \} */

}  // namespace blender::eevee
