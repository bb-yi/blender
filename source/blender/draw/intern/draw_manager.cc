/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "DNA_light_types.h"
#include <climits>
#include <cstdlib>
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"

#include "BKE_main.hh"
#include "BKE_light.h"
#include "DEG_depsgraph_query.hh"

#include "BLI_hash_c.hh"
#include "BLI_color_types.hh"
#include "BKE_image.hh"
#include "BKE_image_gpu.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "BLI_math_base_c.hh"
#include "BLI_math_bits.hh"
#include "BLI_math_matrix.hh"
#include "GPU_capabilities.hh"
#include "WM_api.hh"
#include "WM_types.hh"
#include "BLI_task.hh"

#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "draw_common.hh"
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

  static const bool gpu_cache_enabled = [] {
    const char *value = std::getenv("BLENDER_INSTANCE_CACHE_GPU");
    return value == nullptr || !STREQ(value, "0");
  }();
  instance_buffer_reuse_enabled_ = gpu_cache_enabled &&
                                   ELEM(GPU_backend_get_type(),
                                        GPU_BACKEND_OPENGL,
                                        GPU_BACKEND_VULKAN) &&
                                   GPU_max_work_group_size(0) >= DRW_COMMAND_GROUP_SIZE;
  /* Preserve the original per-instance command path for final image and pass rendering.
   * Compaction can change tie-breaking for overlapping surfaces in Cryptomatte. */
  compact_instances_enabled_ = instance_buffer_reuse_enabled_ &&
                               !(DRWContext::is_active() && drw_get().is_image_render());
  compact_instance_count_ = instance_device_fallbacks_ = instance_material_fallbacks_ = 0;
  gpu_reused_instance_count_ = gpu_resource_upload_bytes_ = 0;
  instance_buffer_slot_ ^= 1;
  instance_ranges_.clear();

  matrix_buf.swap();
  bounds_buf.swap();
  infos_buf.swap();

  matrix_buf.current().trim_to_next_power_of_2(resource_len_);
  bounds_buf.current().trim_to_next_power_of_2(resource_len_);
  infos_buf.current().trim_to_next_power_of_2(resource_len_);
  InstanceBufferSlot &instance_slot = instance_buffer_slots_[instance_buffer_slot_];
  if (instance_slot.capacity != matrix_buf.current().size()) {
    instance_slot.ranges.clear();
  }
  attributes_buf.trim_to_next_power_of_2(attribute_len_);
  if (previous_attribute_capacity_ != attributes_buf.size()) {
    /* A subsequent allocation may grow back to the old size with an uninitialized GPU buffer. */
    previous_attribute_capacity_ = 0;
  }

  /* TODO: This means the reference is kept until further redraw or manager tear-down. Instead,
   * they should be released after each draw loop. But for now, mimics old DRW behavior. */
  for (gpu::Texture *texture : acquired_textures) {
    /* Decrease refcount and free if 0. */
    GPU_texture_free(texture);
  }

  acquired_textures.clear();
  deferred_textures_.clear();
  layer_attributes.clear();
  referenced_objects_.clear();
  light_shader_parameter_requests_.clear();
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
  /* Reserve headers for referenced objects and typed light shader parameters. */
  attribute_len_ = 2;
  attributes_buf.get_or_resize(0) = ObjectAttribute{};
  attributes_buf.get_or_resize(DRW_LIGHT_SHADER_PARAMETER_HEADER) = ObjectAttribute{};
  /* TODO(fclem): Resize buffers if too big, but with an hysteresis threshold. */

  this->object_active = object_active;

  /* Init the 0 resource. */
  resource_handle(float4x4::identity());
}

void Manager::instance_cache_stats(InstanceCacheStats &stats) const
{
  stats.device_fallbacks = instance_device_fallbacks_;
  stats.material_fallbacks = instance_material_fallbacks_;
  stats.compact_instances = compact_instance_count_;
  stats.gpu_reused_instances = gpu_reused_instance_count_;
  stats.gpu_upload_bytes = gpu_resource_upload_bytes_;
}

ResourceHandleRange Manager::instance_material_fallback(ResourceHandleRange handle)
{
  if (handle.uses_compaction()) {
    instance_material_fallbacks_++;
    compact_instance_count_ -= handle.id_range().size();
    invalidate_instance_range(handle);
  }
  return handle.without_compaction();
}

void Manager::invalidate_instance_range(ResourceHandleRange handle)
{
  const int64_t start = ResourceID(handle.id_range().start()).index();
  auto found = std::ranges::lower_bound(
      instance_ranges_, start, {}, [](const CachedInstanceRange &range) {
        return range.indices.start();
      });
  const int64_t end = start + handle.id_range().size();
  for (; found != instance_ranges_.end() && found->indices.start() < end; ++found) {
    found->eligible = false;
  }
}

bool Manager::CachedInstanceRange::matches(const CachedInstanceRange &previous) const
{
  if (!eligible || !previous.eligible || version != previous.version ||
      indices != previous.indices || shadow_normal_offset != previous.shadow_normal_offset ||
      attribute_offset != previous.attribute_offset ||
      attributes_per_instance != previous.attributes_per_instance)
  {
    return false;
  }
  if (info.orco_add != previous.info.orco_add || info.orco_mul != previous.info.orco_mul ||
      info.ob_color != previous.info.ob_color || info.index != previous.info.index ||
      info.flag != previous.info.flag ||
      info.light_and_shadow_set_membership != previous.info.light_and_shadow_set_membership ||
      info.shadow_terminator_geometry_offset != previous.info.shadow_terminator_geometry_offset ||
      bounds.bounding_sphere.w != previous.bounds.bounding_sphere.w)
  {
    return false;
  }
  if (drw_bounds_corners_are_valid(bounds)) {
    for (const int i : IndexRange(4)) {
      if (bounds.bounding_corners[i] != previous.bounds.bounding_corners[i]) {
        return false;
      }
    }
  }
  return true;
}

void Manager::record_instance_attribute_layout(ResourceHandleRange handle)
{
  const int64_t start = ResourceID(handle.id_range().start()).index();
  auto it = std::ranges::lower_bound(instance_ranges_, start, {},
      [](const CachedInstanceRange &range) { return range.indices.start(); });
  const int64_t end = start + handle.id_range().size();
  for (; it != instance_ranges_.end() && it->indices.start() < end; ++it) {
    const Span<ObjectInfos> infos = infos_buf.current().as_span().slice(it->indices);
    const uint len = infos.first().object_attrs_len;
    const uint base = infos.first().object_attrs_offset;
    it->attribute_offset = len ? int(base) : -1;
    it->attributes_per_instance = len;
    for (const int i : infos.index_range()) {
      if (infos[i].object_attrs_len != len || (len && infos[i].object_attrs_offset != base + i * len)) {
        it->eligible = false;
        break;
      }
    }
  }
}

bool Manager::extract_shared_instance_attributes(ResourceHandleRange handle,
                                                 const ObjectRef &ref,
                                                 Span<const GPUUniformAttr *> attributes)
{
  const InstanceBatch &batch = *ref.instance_batch();
  if (!batch.shared_attribute_owner || batch.size() != handle.id_range().size()) {
    return false;
  }

  const bke::AttributeAccessor instance_attributes =
      batch.instances[0]->instance_data[0]->get_instances()->attributes();
  struct Source {
    ObjectAttribute value;
    VArray<ColorGeometry4f> array;
    Span<ColorGeometry4f> span;
  };
  Vector<Source, 8> sources;
  for (const GPUUniformAttr *attribute : attributes) {
    if (attribute->use_dupli) {
      VArray<ColorGeometry4f> array = *instance_attributes.lookup<ColorGeometry4f>(attribute->name);
      if (array) {
        const Span<ColorGeometry4f> span = array.is_span() ? array.get_internal_span() :
                                                            Span<ColorGeometry4f>();
        sources.append({ObjectAttribute{0, 0, 0, 0, attribute->hash_code}, std::move(array), span});
        continue;
      }
    }
    /* With one attribute layer and no particle system, the remaining lookup uses the same
     * parent, object and data-block for the whole batch. */
    ObjectAttribute shared{};
    if (shared.sync(ref, *attribute, 0)) {
      sources.append({shared, {}, {}});
    }
  }

  const int64_t attr_count = sources.size();
  const uint64_t next_len = uint64_t(attribute_len_) + uint64_t(batch.size()) * attr_count;
  if (next_len > INT_MAX || (next_len > uint64_t(attributes_buf.size()) &&
                            power_of_2_max_u(uint(next_len)) >
                                GPU_max_storage_buffer_size() / sizeof(ObjectAttribute)))
  {
    return false;
  }
  if (attr_count != 0) {
    attributes_buf.get_or_resize(int64_t(next_len) - 1);
  }
  const int64_t start = ResourceID(handle.id_range().start()).index();
  const MutableSpan<ObjectInfos> infos = infos_buf.current().as_span().slice(start, batch.size());
  threading::parallel_for(infos.index_range(), 2048, [&](const IndexRange range) {
    for (const int instance : range) {
      const int point = batch.instances[instance]->instance_idx[0];
      const int64_t offset = attribute_len_ + instance * attr_count;
      infos[instance].object_attrs_offset = uint(offset);
      infos[instance].object_attrs_len = uint(attr_count);
      for (const int i : sources.index_range()) {
        const Source &source = sources[i];
        ObjectAttribute value = source.value;
        if (source.array) {
          const ColorGeometry4f color = !source.span.is_empty() ? source.span[point] :
                                                                 source.array[point];
          value.data_x = color.r;
          value.data_y = color.g;
          value.data_z = color.b;
          value.data_w = color.a;
        }
        attributes_buf[offset + i] = value;
      }
    }
  });
  attribute_len_ = uint(next_len);
  return true;
}

void Manager::extract_object_attributes(ResourceHandleRange handle,
                                        const ObjectRef &ref,
                                        Span<const GPUUniformAttr *> attributes)
{
  const InstanceBatch *batch = ref.instance_batch();
  if (batch && attributes.is_empty()) {
    return;
  }
  if (!batch || !extract_shared_instance_attributes(handle, ref, attributes)) {
    int instance_index = 0;
    for (ResourceID resource_id : handle.id_range()) {
      ObjectInfos &infos = infos_buf.current().get_or_resize(resource_id.index());
      infos.object_attrs_offset = attribute_len_;
      infos.object_attrs_len = 0;
      for (const GPUUniformAttr *attr : attributes) {
        if (attributes_buf.get_or_resize(attribute_len_).sync(ref, *attr, instance_index)) {
          infos.object_attrs_len++;
          attribute_len_++;
        }
      }
      instance_index++;
    }
  }
  if (batch) {
    record_instance_attribute_layout(handle);
  }
}

void Manager::upload_attributes()
{
  const Span<ObjectAttribute> current = attributes_buf.as_span().take_front(attribute_len_);
  if (!instance_buffer_reuse_enabled_ || current.size() < 1024) {
    attributes_buf.push_update();
    previous_attributes_.clear_and_shrink();
    previous_attribute_capacity_ = 0;
    return;
  }
  Vector<IndexRange> dirty;
  const bool reusable = previous_attribute_capacity_ == attributes_buf.size();
  if (reusable) {
    for (int start = 0; start < current.size(); start += 512) {
      const IndexRange range = current.index_range().drop_front(start).take_front(512);
      if (range.one_after_last() <= previous_attributes_.size() &&
          std::memcmp(current.data() + start, previous_attributes_.data() + start,
                      range.size() * sizeof(ObjectAttribute)) == 0) {
        continue;
      }
      if (!dirty.is_empty() && dirty.last().one_after_last() == start) {
        dirty.last() = IndexRange(dirty.last().start(), dirty.last().size() + range.size());
      }
      else {
        dirty.append(range);
      }
    }
  }
  bool partial = reusable && dirty.size() <= 64;
  if (partial) {
    for (const IndexRange range : dirty) {
      if (!GPU_storagebuf_update_sub(attributes_buf, range.start() * sizeof(ObjectAttribute),
                                     range.size() * sizeof(ObjectAttribute), current.data() + range.start())) {
        partial = false;
        break;
      }
    }
  }
  const bool uploaded = partial ||
                        GPU_storagebuf_update_sub(
                            attributes_buf, 0, current.size_in_bytes(), current.data());
  if (!uploaded) {
    attributes_buf.push_update();
    /* The legacy upload may clear its destination on an allocation failure. Retry next sync. */
    previous_attribute_capacity_ = 0;
    return;
  }
  previous_attributes_.resize(current.size());
  if (!partial) {
    previous_attributes_.as_mutable_span().copy_from(current);
  }
  else {
    for (const IndexRange range : dirty) {
      previous_attributes_.as_mutable_span().slice(range).copy_from(current.slice(range));
    }
  }
  previous_attribute_capacity_ = attributes_buf.size();
}

Vector<IndexRange> Manager::upload_resources()
{
  Vector<IndexRange> dirty;
  InstanceBufferSlot &slot = instance_buffer_slots_[instance_buffer_slot_];
  /* Test only. Reproduce the zero-filled fallback without exhausting the workstation's VRAM. */
  const bool simulate_failure = instance_buffer_reuse_enabled_ && !instance_ranges_.is_empty() &&
                                std::getenv("BLENDER_DRAW_TEST_INSTANCE_UPLOAD_FAILURE");
  const auto upload = [simulate_failure](auto &buffer, IndexRange range) {
    const size_t stride = sizeof(*buffer.data());
    return !simulate_failure && GPU_storagebuf_update_sub(
        buffer, range.start() * stride, range.size() * stride, buffer.data() + range.start());
  };

  int64_t reused = 0;
  int64_t start = 0;
  if (instance_buffer_reuse_enabled_ && slot.capacity == matrix_buf.current().size()) {
    auto previous = slot.ranges.begin();
    for (const CachedInstanceRange &range : instance_ranges_) {
      while (previous != slot.ranges.end() && previous->indices.start() < range.indices.start()) {
        previous++;
      }
      if (previous == slot.ranges.end() || !range.matches(*previous)) {
        continue;
      }
      if (start < range.indices.start()) {
        dirty.append(IndexRange(start, range.indices.start() - start));
      }
      start = range.indices.one_after_last();
      reused += range.indices.size();
    }
  }
  if (start < resource_len_) {
    dirty.append(IndexRange(start, resource_len_ - start));
  }

  /* If there are many small gaps, one transfer is cheaper than many staging allocations. */
  bool partial = reused > 0 && dirty.size() <= 64;
  if (partial) {
    for (const IndexRange range : dirty) {
      if (!upload(matrix_buf.current(), range) || !upload(bounds_buf.current(), range) ||
          !upload(infos_buf.current(), range))
      {
        partial = false;
        break;
      }
    }
  }
  constexpr size_t resource_stride = sizeof(ObjectMatrices) + sizeof(ObjectBounds) +
                                     sizeof(ObjectInfos);
  bool upload_confirmed = partial;
  if (partial) {
    gpu_reused_instance_count_ = reused;
    gpu_resource_upload_bytes_ = (resource_len_ - reused) * resource_stride;
  }
  else {
    const IndexRange used(resource_len_);
    /* Use a checked transfer even when the live range fills the allocation. */
    upload_confirmed = instance_buffer_reuse_enabled_ && !instance_ranges_.is_empty() &&
                       upload(matrix_buf.current(), used) && upload(bounds_buf.current(), used) &&
                       upload(infos_buf.current(), used);
    if (upload_confirmed) {
      gpu_resource_upload_bytes_ = used.size() * resource_stride;
    }
    else {
      if (simulate_failure) {
        matrix_buf.current().clear_to_zero();
        bounds_buf.current().clear_to_zero();
        infos_buf.current().clear_to_zero();
      }
      else {
        matrix_buf.current().push_update();
        bounds_buf.current().push_update();
        infos_buf.current().push_update();
      }
      gpu_resource_upload_bytes_ = matrix_buf.current().size() * resource_stride;
    }
    dirty.clear();
    dirty.append(used);
  }
  slot.capacity = matrix_buf.current().size();
  if (upload_confirmed) {
    slot.ranges = std::move(instance_ranges_);
  }
  else {
    slot.ranges.clear();
    instance_ranges_.clear();
  }
  return dirty;
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

void Manager::sync_light_shader_parameters()
{
  if (light_shader_parameter_requests_.is_empty()) {
    return;
  }
  Map<uint64_t, std::string> names;
  Set<std::pair<uint32_t, uint64_t>> requests;
  for (const auto &request : light_shader_parameter_requests_) {
    std::string &name = names.lookup_or_add(request.key, request.name);
    if (name != request.name) {
      WM_global_report(RPT_ERROR, "Light shader parameter name hash collision; rename one parameter");
      return;
    }
    requests.add({request.object_uid, request.key});
  }
  struct Record {
    uint32_t uid;
    uint64_t key;
    int type;
    float4 value;
  };
  Vector<Record> records;
  const auto &ctx = drw_get();
  Main *bmain = DEG_get_bmain(ctx.depsgraph);
  if (!bmain) {
    return;
  }
  for (Object &object : bmain->objects) {
    if (object.type != OB_LAMP || object.id.session_uid == 0) {
      continue;
    }
    Object *evaluated = referenced_object_evaluated(ctx.depsgraph, &object);
    if (!evaluated || !evaluated->data || evaluated->type != OB_LAMP) {
      continue;
    }
    const Light &light = *id_cast<Light *>(evaluated->data);
    for (const LightShaderParameter &parameter : light.shader_parameters) {
      const uint64_t key = GPU_light_shader_parameter_key(parameter.name);
      if (!requests.contains({0u, key}) && !requests.contains({object.id.session_uid, key})) {
        continue;
      }
      /* Comparing full CPU-side names also rejects collisions with unrequested parameters. */
      if (names.lookup(key) != parameter.name) {
        WM_global_report(RPT_ERROR, "Light shader parameter name hash collision; rename one parameter");
        return;
      }
      float4 value;
      if (BKE_light_shader_parameter_value(parameter, value)) {
        records.append({object.id.session_uid, key, int(parameter.type), value});
      }
    }
  }
  if (records.is_empty()) {
    return;
  }
  /* Two lanes per hash slot, load factor <= 1/2. Check the actual rounded allocation size. */
  uint64_t table_size = 1;
  while (table_size < uint64_t(records.size()) * 2) {
    table_size <<= 1;
  }
  const uint64_t table_end = uint64_t(attribute_len_) + 2 * table_size;
  uint64_t allocated_lanes = 1;
  while (allocated_lanes < table_end) {
    allocated_lanes <<= 1;
  }
  if (table_end > uint64_t(INT_MAX) ||
      allocated_lanes > GPU_max_storage_buffer_size() / sizeof(ObjectAttribute))
  {
    WM_global_report(RPT_ERROR, "Light shader parameter table exceeds GPU storage buffer capacity");
    return;
  }
  const uint offset = attribute_len_;
  for (uint i = offset; i < uint(table_end); i++) {
    attributes_buf.get_or_resize(i) = ObjectAttribute{};
  }
  attribute_len_ = uint(table_end);
  for (const Record &record : records) {
    const uint lo = uint(record.key);
    const uint hi = uint(record.key >> 32);
    uint slot = ((record.uid * 1664525u) ^ lo ^ hi) & uint(table_size - 1);
    while (attributes_buf[offset + slot * 2].hash_code != 0u) {
      slot = (slot + 1u) & uint(table_size - 1);
    }
    attributes_buf[offset + slot * 2] = referenced_object_attribute(
        float4(uint_as_float(lo), uint_as_float(hi), uint_as_float(record.type), 0.0f), record.uid);
    attributes_buf[offset + slot * 2 + 1] = referenced_object_attribute(record.value);
  }
  attributes_buf[DRW_LIGHT_SHADER_PARAMETER_HEADER] = referenced_object_attribute(
      float4(uint_as_float(offset), uint_as_float(uint(table_size)), 0.0f, 0.0f),
      DRW_LIGHT_SHADER_PARAMETER_MAGIC);
}

void Manager::load_deferred_textures()
{
  if (deferred_textures_.is_empty()) {
    return;
  }

  GPU_debug_group_begin("Texture Loading");

  /* Load files from disk in a multithreaded manner. Allow better parallelism. */
  threading::parallel_for(deferred_textures_.index_range(), 1, [&](const IndexRange range) {
    for (const int i : range) {
      DeferredTexture &deferred = *deferred_textures_[i];
      BKE_image_get_tile(deferred.image, 0);
      threading::isolate_task([&]() {
        ImBuf *imbuf = BKE_image_acquire_ibuf(deferred.image, deferred.image_user, nullptr);
        BKE_image_release_ibuf(deferred.image, imbuf, nullptr);
      });
    }
  });

  /* Avoid any leftover bind before GPU texture creation which could cause assert
   * about missing specialization constants. */
  GPU_shader_unbind();

  /* Upload to the GPU (create gpu::Texture). This part still requires a valid GPU context and
   * is not easily parallelized. */
  for (std::unique_ptr<DeferredTexture> &deferred : deferred_textures_) {
    const ImageGPUTextures textures = deferred->use_cube_strip ?
        BKE_image_acquire_gpu_material_cube_texture(deferred->image, deferred->image_user, false) :
        deferred->use_3d_lut_strip ?
        BKE_image_acquire_gpu_material_3d_lut_texture(deferred->image,
            deferred->image_user, deferred->lut_dimensions.x, deferred->lut_dimensions.y,
            deferred->lut_dimensions.z, false) :
        BKE_image_acquire_gpu_material_texture(
            deferred->image, deferred->image_user, deferred->use_tile_mapping, false);
    deferred->texture = textures.texture;
    deferred->tile_mapping = textures.tile_mapping;
    hold_texture(textures.texture);
    hold_texture(textures.tile_mapping);
  }

  GPU_debug_group_end();
}

void Manager::end_sync()
{
  GPU_debug_group_begin("Manager.end_sync");

  load_deferred_textures();

  sync_layer_attributes();
  sync_referenced_objects();
  sync_light_shader_parameters();

  const Vector<IndexRange> dirty_resources = upload_resources();
  upload_attributes();
  layer_attributes_buf.push_update();

  /* Useful for debugging the following resource finalize. But will trigger the drawing of the GPU
   * debug draw/print buffers for every frame. Not nice for performance. */
  // debug_bind();

  DRW_submission_start();

  /* Dispatch compute to finalize the resources on GPU. Save a bit of CPU time. */
  gpu::Shader *shader = DRW_shader_draw_resource_finalize_get();
  GPU_shader_bind(shader);
  GPU_storagebuf_bind(matrix_buf.current(), GPU_shader_get_ssbo_binding(shader, "matrix_buf"));
  GPU_storagebuf_bind(bounds_buf.current(), GPU_shader_get_ssbo_binding(shader, "bounds_buf"));
  GPU_storagebuf_bind(infos_buf.current(), GPU_shader_get_ssbo_binding(shader, "infos_buf"));
  for (const IndexRange range : dirty_resources) {
    GPU_shader_uniform_1i(shader, "resource_start", range.start());
    GPU_shader_uniform_1i(shader, "resource_len", range.size());
    GPU_compute_dispatch(shader, divide_ceil_u(range.size(), DRW_FINALIZE_GROUP_SIZE), 1, 1);
  }
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  use_cluster_bounds_ = compact_instances_enabled_ && compact_instance_count_ >= 4096;
  if (use_cluster_bounds_) {
    const uint count = divide_ceil_u(resource_len_, DRW_VISIBILITY_GROUP_SIZE);
    cluster_bounds_.resize(count);
    gpu::Shader *cluster_shader = DRW_shader_draw_resource_cluster_finalize_get();
    GPU_shader_bind(cluster_shader);
    GPU_shader_uniform_1i(cluster_shader, "resource_len", resource_len_);
    GPU_storagebuf_bind(bounds_buf.current(), GPU_shader_get_ssbo_binding(cluster_shader, "bounds_buf"));
    GPU_storagebuf_bind(cluster_bounds_, GPU_shader_get_ssbo_binding(cluster_shader, "clusters"));
    GPU_compute_dispatch(cluster_shader, count, 1, 1);
    GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  }

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
  view.cluster_bounds_ = cluster_bounds_;
  view.use_cluster_bounds_ = use_cluster_bounds_;
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
