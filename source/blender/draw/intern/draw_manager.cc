/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "DNA_light_types.h"
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"

#include "BKE_main.hh"
#include "DEG_depsgraph_query.hh"

#include "BLI_hash.h"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "BLI_math_base.h"
#include "BLI_math_bits.h"
#include "BLI_math_matrix.hh"
#include "GPU_capabilities.hh"
#include "GPU_compute.hh"

#include "draw_context_private.hh"
#include "draw_debug.hh"
#include "draw_defines.hh"
#include "draw_manager.hh"
#include "draw_pass.hh"
#include "draw_shader.hh"

namespace blender::draw {

std::atomic<uint32_t> Manager::global_sync_counter_ = 1;

namespace {

static ObjectAttribute referenced_object_attribute(const float4 &value, const uint hash_code = 0)
{
  ObjectAttribute attribute{};
  attribute.data_x = value.x;
  attribute.data_y = value.y;
  attribute.data_z = value.z;
  attribute.data_w = value.w;
  attribute.hash_code = hash_code;
  return attribute;
}

static int referenced_light_type(const Light &light)
{
  switch (light.type) {
    case LA_LOCAL:
      return 0;
    case LA_SUN:
      return 1;
    case LA_SPOT:
      return 2;
    case LA_AREA:
      return 3;
    default:
      return -1;
  }
}

static void referenced_light_values(const Object &object,
                                    const Light &light,
                                    float &r_radius,
                                    float &r_spot_size,
                                    float &r_sun_angle)
{
  const float4x4 object_to_world = object.object_to_world();
  r_radius = 0.0f;
  r_spot_size = 0.0f;
  r_sun_angle = 0.0f;

  const float3 scale = {
      math::length(object_to_world.x_axis()),
      math::length(object_to_world.y_axis()),
      math::length(object_to_world.z_axis()),
  };
  const float max_scale = max_ff(max_ff(scale.x, scale.y), scale.z);

  switch (light.type) {
    case LA_LOCAL:
      r_radius = light.radius * max_scale;
      break;
    case LA_SUN:
      r_radius = light.sun_angle;
      r_sun_angle = light.sun_angle;
      break;
    case LA_SPOT:
      r_radius = light.radius * max_scale;
      r_spot_size = light.spotsize;
      break;
    case LA_AREA: {
      const bool is_irregular = ELEM(light.area_shape, LA_AREA_RECT, LA_AREA_ELLIPSE);
      const float size_x = light.area_size * scale.x;
      const float size_y = (is_irregular ? light.area_sizey : light.area_size) * scale.y;
      r_radius = 0.5f * max_ff(size_x, size_y);
      break;
    }
  }
}

static Object *referenced_object_evaluated(Depsgraph *depsgraph, Object *object)
{
  if (depsgraph == nullptr || object == nullptr) {
    return nullptr;
  }
  Object *object_eval = DEG_get_evaluated(depsgraph, object);
  return object_eval != nullptr ? object_eval : object;
}

}  // namespace

Manager::~Manager()
{
  for (gpu::Texture *texture : acquired_textures) {
    /* Decrease refcount and free if 0. */
    GPU_texture_free(texture);
  }
}

void Manager::begin_sync(Object *object_active)
{
  /* Add 2 to always have a non-null number even in case of overflow. */
  sync_counter_ = (global_sync_counter_ += 2);

  matrix_buf.swap();
  bounds_buf.swap();
  infos_buf.swap();

  matrix_buf.current().trim_to_next_power_of_2(resource_len_);
  bounds_buf.current().trim_to_next_power_of_2(resource_len_);
  infos_buf.current().trim_to_next_power_of_2(resource_len_);
  attributes_buf.trim_to_next_power_of_2(attribute_len_);

  /* TODO: This means the reference is kept until further redraw or manager tear-down. Instead,
   * they should be released after each draw loop. But for now, mimics old DRW behavior. */
  for (gpu::Texture *texture : acquired_textures) {
    /* Decrease refcount and free if 0. */
    GPU_texture_free(texture);
  }

  acquired_textures.clear();
  layer_attributes.clear();
  referenced_objects_.clear();
  referenced_object_table_offset_ = 0;
  referenced_object_table_size_ = 0;

/* For some reason, if this uninitialized data pattern was enabled (ie release asserts enabled),
 * The viewport just gives up rendering objects on ARM64 devices. Possibly Mesa GLOn12-related. */
#if !defined(NDEBUG) && !defined(_M_ARM64)
  /* Detect uninitialized data. */
  memset(matrix_buf.current().data(),
         0xF0,
         matrix_buf.current().size() * sizeof(*matrix_buf.current().data()));
  memset(bounds_buf.current().data(),
         0xF0,
         matrix_buf.current().size() * sizeof(*bounds_buf.current().data()));
  memset(infos_buf.current().data(),
         0xF0,
         matrix_buf.current().size() * sizeof(*infos_buf.current().data()));
#endif
  resource_len_ = 0;
  /* Lane zero is reserved for the referenced-object channel header. */
  attribute_len_ = 1;
  attributes_buf.get_or_resize(0) = ObjectAttribute{};
  /* TODO(fclem): Resize buffers if too big, but with an hysteresis threshold. */

  this->object_active = object_active;

  /* Init the 0 resource. */
  resource_handle(float4x4::identity());
}

void Manager::sync_layer_attributes()
{
  /* Sort the attribute IDs - the shaders use binary search. */
  Vector<uint32_t> id_list;

  id_list.reserve(layer_attributes.size());

  for (uint32_t id : layer_attributes.keys()) {
    id_list.append(id);
  }

  std::ranges::sort(id_list);

  /* Look up the attributes. */
  int count = 0, size = layer_attributes_buf.end() - layer_attributes_buf.begin();

  for (uint32_t id : id_list) {
    if (layer_attributes_buf[count].sync(
            drw_get().scene, drw_get().view_layer, layer_attributes.lookup(id)))
    {
      /* Check if the buffer is full. */
      if (++count == size) {
        break;
      }
    }
  }

  layer_attributes_buf[0].buffer_length = count;
}

void Manager::sync_referenced_objects()
{
  const uint reference_count = uint(referenced_objects_.size());
  uint table_size = 0;
  if (reference_count > 0) {
    table_size = 1;
    while (table_size < reference_count * 2u && table_size < (1u << 20)) {
      table_size <<= 1;
    }
    if (table_size < reference_count) {
      table_size = 0;
    }
  }

  referenced_object_table_offset_ = attribute_len_;
  if (table_size > 0) {
    const uint table_end = referenced_object_table_offset_ +
                           table_size * DRW_REFERENCED_OBJECT_DATA_RECORD_STRIDE;
    const size_t allocated_bytes = size_t(power_of_2_max_u(table_end)) *
                                   sizeof(ObjectAttribute);
    if (allocated_bytes > GPU_max_storage_buffer_size()) {
      /* Do not let the shared attribute buffer exceed the backend SSBO limit. */
      table_size = 0;
    }
  }
  referenced_object_table_size_ = table_size;
  if (table_size > 0) {
    const uint table_end = referenced_object_table_offset_ +
                           table_size * DRW_REFERENCED_OBJECT_DATA_RECORD_STRIDE;
    for (uint index = referenced_object_table_offset_; index < table_end; index++) {
      attributes_buf.get_or_resize(index) = ObjectAttribute{};
    }
    attribute_len_ = table_end;
  }

  ObjectAttribute &header = attributes_buf.get_or_resize(0);
  header = referenced_object_attribute(
      float4(uint_as_float(DRW_REFERENCED_OBJECT_DATA_ABI_VERSION),
             uint_as_float(referenced_object_table_offset_),
             uint_as_float(referenced_object_table_size_),
             uint_as_float(DRW_REFERENCED_OBJECT_DATA_RECORD_STRIDE)),
      DRW_REFERENCED_OBJECT_DATA_MAGIC);

  Depsgraph *depsgraph = drw_get().depsgraph;
  if (table_size == 0 || depsgraph == nullptr) {
    return;
  }
  Main *bmain = DEG_get_bmain(depsgraph);
  if (bmain == nullptr) {
    return;
  }

  /* Resolve all requested UIDs in one Main traversal. The previous per-reference lookup scanned
   * the complete object list for every material reference, making sync cost O(references * objects).
   * Keeping this map local also makes deletion/remap safe: no stale GPUMaterial pointer is touched. */
  Map<uint32_t, Object *> referenced_object_sources;
  referenced_object_sources.reserve(reference_count);
  for (Object &object : bmain->objects) {
    const uint32_t session_uid = object.id.session_uid;
    if (referenced_objects_.contains(session_uid)) {
      referenced_object_sources.add(session_uid, &object);
      if (referenced_object_sources.size() == reference_count) {
        break;
      }
    }
  }

  for (const auto item : referenced_objects_.items()) {
    const uint32_t session_uid = item.key;
    const GPUReferencedObject &request = item.value;
    Object *object = referenced_object_sources.lookup_default(session_uid, nullptr);
    Object *object_eval = referenced_object_evaluated(depsgraph, object);
    if (object_eval == nullptr) {
      continue;
    }

    const uint slot_start = BLI_hash_int(session_uid) & (table_size - 1);
    uint slot = slot_start;
    bool inserted = false;
    for (uint probe = 0; probe < table_size; probe++) {
      ObjectAttribute &metadata = attributes_buf.get_or_resize(
          referenced_object_table_offset_ + slot * DRW_REFERENCED_OBJECT_DATA_RECORD_STRIDE);
      if (metadata.hash_code == 0 || metadata.hash_code == session_uid) {
        metadata.hash_code = session_uid;
        const int light_type = (object_eval->type == OB_LAMP && object_eval->data != nullptr) ?
                                   referenced_light_type(*id_cast<Light *>(object_eval->data)) :
                                   -1;
        metadata.data_x = uint_as_float(uint(object_eval->type));
        metadata.data_y = int_as_float(light_type);
        metadata.data_z = uint_as_float(
            uint(object_eval->visibility_flag & (OB_HIDE_VIEWPORT | OB_HIDE_RENDER)));
        metadata.data_w = uint_as_float(uint(request.flags));

        const uint record_offset = referenced_object_table_offset_ +
                                   slot * DRW_REFERENCED_OBJECT_DATA_RECORD_STRIDE;
        attributes_buf.get_or_resize(record_offset + 6) = referenced_object_attribute(
            float4(1.0f, 1.0f, 1.0f, 0.0f));
        if (request.flags & GPU_REFERENCED_OBJECT_DATA_TRANSFORM) {
          const float4x4 &object_to_world = object_eval->object_to_world();
          for (int column = 0; column < 4; column++) {
            attributes_buf.get_or_resize(record_offset + 1 + column) =
                referenced_object_attribute(object_to_world[column]);
          }

          float3 location;
          math::EulerXYZ rotation;
          float3 scale;
          math::to_loc_rot_scale<true>(object_to_world, location, rotation, scale);
          attributes_buf.get_or_resize(record_offset + 5) = referenced_object_attribute(
              float4(float3(rotation), 0.0f));
          attributes_buf.get_or_resize(record_offset + 6) = referenced_object_attribute(
              float4(scale, 0.0f));
        }
        if (request.flags & GPU_REFERENCED_OBJECT_DATA_COLOR) {
          attributes_buf.get_or_resize(record_offset + 7) = referenced_object_attribute(
              float4(object_eval->color[0],
                    object_eval->color[1],
                    object_eval->color[2],
                    object_eval->color[3]));
        }

        float visible = (object_eval->visibility_flag & (OB_HIDE_VIEWPORT | OB_HIDE_RENDER)) == 0 ?
                            1.0f :
                            0.0f;
        if (request.flags & GPU_REFERENCED_OBJECT_DATA_LIGHT && object_eval->type == OB_LAMP &&
            object_eval->data != nullptr)
        {
          const Light &light = *id_cast<Light *>(object_eval->data);
          attributes_buf.get_or_resize(record_offset + 8) = referenced_object_attribute(
              float4(light.r, light.g, light.b, light.energy));
          float radius;
          float spot_size;
          float sun_angle;
          referenced_light_values(*object_eval, light, radius, spot_size, sun_angle);
          attributes_buf.get_or_resize(record_offset + 9) = referenced_object_attribute(
              float4(radius, spot_size, sun_angle, visible));
        }
        else if (request.flags & GPU_REFERENCED_OBJECT_DATA_VISIBILITY) {
          attributes_buf.get_or_resize(record_offset + 9).data_w = visible;
        }

        inserted = true;
        break;
      }
      slot = (slot + 1) & (table_size - 1);
    }
    if (!inserted) {
      /* A full table is a safe default rather than an out-of-bounds write. */
      continue;
    }
  }
}

void Manager::end_sync()
{
  GPU_debug_group_begin("Manager.end_sync");

  sync_layer_attributes();
  sync_referenced_objects();

  matrix_buf.current().push_update();
  bounds_buf.current().push_update();
  infos_buf.current().push_update();
  attributes_buf.push_update();
  layer_attributes_buf.push_update();

  /* Useful for debugging the following resource finalize. But will trigger the drawing of the GPU
   * debug draw/print buffers for every frame. Not nice for performance. */
  // debug_bind();

  DRW_submission_start();

  /* Dispatch compute to finalize the resources on GPU. Save a bit of CPU time. */
  uint thread_groups = divide_ceil_u(resource_len_, DRW_FINALIZE_GROUP_SIZE);
  gpu::Shader *shader = DRW_shader_draw_resource_finalize_get();
  GPU_shader_bind(shader);
  GPU_shader_uniform_1i(shader, "resource_len", resource_len_);
  GPU_storagebuf_bind(matrix_buf.current(), GPU_shader_get_ssbo_binding(shader, "matrix_buf"));
  GPU_storagebuf_bind(bounds_buf.current(), GPU_shader_get_ssbo_binding(shader, "bounds_buf"));
  GPU_storagebuf_bind(infos_buf.current(), GPU_shader_get_ssbo_binding(shader, "infos_buf"));
  GPU_compute_dispatch(shader, thread_groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  DRW_submission_end();

  GPU_debug_group_end();
}

void Manager::debug_bind()
{
  gpu::StorageBuf *gpu_buf = DebugDraw::get().gpu_draw_buf_get();
  if (gpu_buf == nullptr) {
    return;
  }
  GPU_storagebuf_bind(gpu_buf, DRW_DEBUG_DRAW_SLOT);
#ifndef DISABLE_DEBUG_SHADER_PRINT_BARRIER
  /* Add a barrier to allow multiple shader writing to the same buffer. */
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
#endif
}

void Manager::resource_bind()
{
  GPU_storagebuf_bind(matrix_buf.current(), DRW_OBJ_MAT_SLOT);
  GPU_storagebuf_bind(infos_buf.current(), DRW_OBJ_INFOS_SLOT);
  GPU_storagebuf_bind(attributes_buf, DRW_OBJ_ATTR_SLOT);
  GPU_uniformbuf_bind(layer_attributes_buf, DRW_LAYER_ATTR_UBO_SLOT);
}

uint64_t Manager::fingerprint_get()
{
  /* Covers new sync cycle, added resources and different #Manager. */
  return sync_counter_ | (uint64_t(resource_len_) << 32);
}

ResourceHandleRange Manager::unique_handle_for_sculpt(const ObjectRef &ref)
{
  if (ref.sculpt_handle_.is_valid()) {
    return ref.sculpt_handle_;
  }
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*ref.object);
  const Bounds<float3> bounds = bke::pbvh::bounds_get(pbvh);
  const float3 center = math::midpoint(bounds.min, bounds.max);
  const float3 half_extent = bounds.max - center;
  /* WORKAROUND: Instead of breaking const correctness everywhere, we only break it for this. */
  const_cast<ObjectRef &>(ref).sculpt_handle_ = resource_handle(
      ref, nullptr, &center, &half_extent);
  return ref.sculpt_handle_;
}

void Manager::compute_visibility(View &view)
{
  bool freeze_culling = (USER_DEVELOPER_TOOL_TEST(&U, use_viewport_debug) && drw_get().v3d &&
                         (drw_get().v3d->debug_flag & V3D_DEBUG_FREEZE_CULLING) != 0);

  BLI_assert_msg(view.manager_fingerprint_ != this->fingerprint_get(),
                 "Resources did not changed, no need to update");

  view.manager_fingerprint_ = this->fingerprint_get();

  view.bind();
  view.compute_visibility(
      bounds_buf.current(), infos_buf.current(), resource_len_, freeze_culling);
}

void Manager::ensure_visibility(View &view)
{
  if (view.manager_fingerprint_ != this->fingerprint_get()) {
    compute_visibility(view);
  }
}

void Manager::generate_commands(PassMain &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  BLI_assert_msg((pass.manager_fingerprint_ != this->fingerprint_get()) ||
                     (pass.view_fingerprint_ != view.fingerprint_get()),
                 "Resources and view did not changed no need to update");
  BLI_assert_msg((view.manager_fingerprint_ == this->fingerprint_get()) &&
                     (view.fingerprint_get() != 0),
                 "Resources or view changed, but compute_visibility was not called");

  pass.manager_fingerprint_ = this->fingerprint_get();
  pass.view_fingerprint_ = view.fingerprint_get();

  pass.draw_commands_buf_.generate_commands(pass.headers_,
                                            pass.commands_,
                                            view.get_visibility_buffer(),
                                            view.visibility_word_per_draw(),
                                            view.view_len_,
                                            pass.use_custom_ids);
}

void Manager::generate_commands(PassSortable &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  pass.sort();
  generate_commands(static_cast<PassMain &>(pass), view);
}

void Manager::generate_commands(PassSimple &pass)
{
  if (pass.is_empty()) {
    return;
  }

  BLI_assert_msg(pass.manager_fingerprint_ != this->fingerprint_get(),
                 "Resources did not changed since last generate_command, no need to update");
  pass.manager_fingerprint_ = this->fingerprint_get();

  pass.draw_commands_buf_.generate_commands(pass.headers_, pass.commands_, pass.sub_passes_);
}

void Manager::warm_shader_specialization(PassMain &pass)
{
  if (pass.is_empty()) {
    return;
  }
  command::RecordingState state;
  pass.warm_shader_specialization(state);
}

void Manager::warm_shader_specialization(PassSimple &pass)
{
  if (pass.is_empty()) {
    return;
  }
  command::RecordingState state;
  pass.warm_shader_specialization(state);
}

void Manager::submit_only(PassMain &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  BLI_assert_msg(view.manager_fingerprint_ != 0, "compute_visibility was not called on this view");
  BLI_assert_msg(view.manager_fingerprint_ == this->fingerprint_get(),
                 "Resources changed since last compute_visibility");
  BLI_assert_msg(pass.manager_fingerprint_ != 0, "generate_command was not called on this pass");
  BLI_assert_msg(pass.manager_fingerprint_ == this->fingerprint_get(),
                 "Resources changed since last generate_command");
  /* The function generate_commands needs to be called for each view this pass is going to be
   * submitted with. This is because the commands are stored inside the pass and not per view. */
  BLI_assert_msg(pass.view_fingerprint_ == view.fingerprint_get(),
                 "View have changed since last generate_commands or "
                 "submitting with a different view");

  debug_bind();

  command::RecordingState state;
  state.inverted_view = view.is_inverted();

  view.bind();
  pass.draw_commands_buf_.bind(state);

  resource_bind();

  pass.submit(state);

  state.cleanup();
}

void Manager::submit(PassMain &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  if (view.manager_fingerprint_ != this->fingerprint_get()) {
    compute_visibility(view);
  }

  if (pass.manager_fingerprint_ != this->fingerprint_get() ||
      pass.view_fingerprint_ != view.fingerprint_get())
  {
    generate_commands(pass, view);
  }

  this->submit_only(pass, view);
}

void Manager::submit(PassSortable &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  pass.sort();

  this->submit(static_cast<PassMain &>(pass), view);
}

void Manager::submit(PassSimple &pass, bool inverted_view)
{
  if (pass.is_empty()) {
    return;
  }

  if (!pass.has_generated_commands()) {
    generate_commands(pass);
  }

  debug_bind();

  command::RecordingState state;
  state.inverted_view = inverted_view;

  pass.draw_commands_buf_.bind(state);

  resource_bind();

  pass.submit(state);

  state.cleanup();
}

void Manager::submit(PassSimple &pass, View &view)
{
  if (pass.is_empty()) {
    return;
  }

  debug_bind();

  view.bind();

  this->submit(pass, view.is_inverted());
}

Manager::SubmitDebugOutput Manager::submit_debug(PassSimple &pass, View &view)
{
  submit(pass, view);

  pass.draw_commands_buf_.resource_id_buf_.read();

  Manager::SubmitDebugOutput output;
  output.resource_id = {pass.draw_commands_buf_.resource_id_buf_.data(),
                        pass.draw_commands_buf_.resource_id_count_};
  /* There is no visibility data for PassSimple. */
  output.visibility = {static_cast<uint *>(view.get_visibility_buffer().data()), 0};
  return output;
}

Manager::SubmitDebugOutput Manager::submit_debug(PassMain &pass, View &view)
{
  submit(pass, view);

  GPU_memory_barrier(GPU_BARRIER_BUFFER_UPDATE);

  pass.draw_commands_buf_.resource_id_buf_.read();
  view.get_visibility_buffer().read();

  Manager::SubmitDebugOutput output;
  output.resource_id = {pass.draw_commands_buf_.resource_id_buf_.data(),
                        pass.draw_commands_buf_.resource_id_count_};
  output.visibility = {static_cast<uint *>(view.get_visibility_buffer().data()),
                       divide_ceil_u(resource_len_, 32)};
  return output;
}

Manager::DataDebugOutput Manager::data_debug()
{
  matrix_buf.current().read();
  bounds_buf.current().read();
  infos_buf.current().read();

  Manager::DataDebugOutput output;
  output.matrices = {matrix_buf.current().data(), resource_len_};
  output.bounds = {bounds_buf.current().data(), resource_len_};
  output.infos = {infos_buf.current().data(), resource_len_};
  return output;
}

}  // namespace blender::draw
