/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "BKE_compositor.hh"
#include "BKE_mesh.hh"
#include "BKE_scene.hh"
#include "BLI_listbase.h"
#include <cmath>

#include "DEG_depsgraph_query.hh"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "GPU_texture.hh"

#include "eevee_instance.hh"
#include "eevee_render_texture.hh"

namespace blender::eevee {

static int render_texture_source_normalize(const int source)
{
  if (source == SCE_EEVEE_RENDER_TEXTURE_SOURCE_GRAYSCALE) {
    return SCE_EEVEE_RENDER_TEXTURE_SOURCE_COLOR;
  }
  return source;
}

static gpu::TextureFormat render_texture_gpu_format(const int format)
{
  switch (format) {
    case SCE_EEVEE_RENDER_TEXTURE_FORMAT_RGBA32F:
      return gpu::TextureFormat::SFLOAT_32_32_32_32;
    case SCE_EEVEE_RENDER_TEXTURE_FORMAT_R16F:
      return gpu::TextureFormat::SFLOAT_16;
    case SCE_EEVEE_RENDER_TEXTURE_FORMAT_R32F:
      return gpu::TextureFormat::SFLOAT_32;
    case SCE_EEVEE_RENDER_TEXTURE_FORMAT_RGBA16F:
    default:
      return gpu::TextureFormat::SFLOAT_16_16_16_16;
  }
}

static int render_texture_info_flags(const int source, const int format)
{
  return RENDER_TEXTURE_SLOT_VALID | (source << int(RENDER_TEXTURE_SLOT_SOURCE_SHIFT)) |
         (format << int(RENDER_TEXTURE_SLOT_FORMAT_SHIFT));
}

RenderTextureData RenderTextureModule::slot_default_data()
{
  RenderTextureData data = {};
  data.viewproj = float4x4::identity();
  data.prev_viewproj = float4x4::identity();
  data.camera_position = float4(0.0f);
  data.camera_axis_x = float4(0.0f);
  data.camera_axis_y = float4(0.0f);
  data.camera_axis_z = float4(0.0f);
  data.info = int4(-1, 0, 0, 0);
  return data;
}

void RenderTextureModule::slot_reset(const int slot_index)
{
  RuntimeSlot &slot = slots_[slot_index];
  slot.uid = -1;
  slot.camera = nullptr;
  slot.camera_orig = nullptr;
  slot.extent = int2(1);
  slot.source = SCE_EEVEE_RENDER_TEXTURE_SOURCE_COLOR;
  slot.format = SCE_EEVEE_RENDER_TEXTURE_FORMAT_RGBA16F;
  slot.update_mode = SCE_EEVEE_RENDER_TEXTURE_UPDATE_EVERY_FRAME;
  slot.manual_update = 0;
  slot.capture_pending = true;
  slot.active = false;
  slot.used = false;
  slot.compositor_output = false;
  slot.history_reset_pending = true;
  slot.needs_history = false;
  slot.requires_full_capture = false;
  slot.sync_camera_valid = false;
  slot.has_camera_data = false;
  slot.camera_data = {};
  slot.rt_buffer_opaque.reset();
  slot.rt_buffer_refract.reset();
  slot.buffers.reset();
  data_[slot_index] = slot_default_data();
}

uint8_t RenderTextureModule::slot_ensure_textures(const int slot_index)
{
  RuntimeSlot &slot = slots_[slot_index];
  const float4 clear_color(0.0f);
  const gpu::TextureFormat texture_format = render_texture_gpu_format(slot.format);
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ |
                                     GPU_TEXTURE_USAGE_SHADER_WRITE;

  const int2 extent = slot.active && slot.used ? slot.extent : int2(1);
  const bool recreated_current = slot.color_tx.current().ensure_2d(texture_format, extent, usage);
  const bool recreated_previous = slot.color_tx.previous().ensure_2d(
      texture_format, slot.needs_history && slot.used ? extent : int2(1), usage);

  if (recreated_current) {
    slot.color_tx.current().clear(clear_color);
  }
  if (recreated_previous) {
    slot.color_tx.previous().clear(clear_color);
  }

  return uint8_t(recreated_current ? SLOT_TEXTURE_CURRENT_RECREATED : 0u) |
         uint8_t(recreated_previous ? SLOT_TEXTURE_PREVIOUS_RECREATED : 0u);
}

void RenderTextureModule::init()
{
  for (int slot_index = 0; slot_index < RENDER_TEXTURE_SLOT_MAX; slot_index++) {
    /* Keep active captures and their history across viewport redraws. */
    if (slots_[slot_index].uid == -1) {
      data_[slot_index] = slot_default_data();
    }
    slot_ensure_textures(slot_index);
  }
  data_.push_update();
}

void RenderTextureModule::begin_sync()
{
  used_uids_.clear();
  capture_read_uids_.clear();
  capture_has_texture_dependencies_ = false;
  requires_full_capture_ = false;
  const uint64_t evaluation = DEG_get_update_count(inst_.depsgraph);
  const float time = DEG_get_ctime(inst_.depsgraph);
  const bool evaluation_changed = !has_evaluation_ || evaluation != last_evaluation_ ||
                                  time != last_time_;
  last_evaluation_ = evaluation;
  last_time_ = time;
  has_evaluation_ = true;

  Vector<SceneRenderTexture *, RENDER_TEXTURE_SLOT_MAX> enabled;
  for (SceneRenderTexture *render_texture = static_cast<SceneRenderTexture *>(
           inst_.scene->eevee.render_textures.first);
       render_texture != nullptr;
       render_texture = render_texture->next)
  {
    if (!render_texture->enabled || render_texture->camera == nullptr) {
      continue;
    }
    enabled.append(render_texture);
    if (enabled.size() == RENDER_TEXTURE_SLOT_MAX) {
      break;
    }
  }

  /* Reorder all surviving UIDs before initializing new entries, which might otherwise overwrite
   * a later entry's cached image. Keep feedback capture order identical to the scene list. */
  for (const int index : enabled.index_range()) {
    for (const int previous : IndexRange(RENDER_TEXTURE_SLOT_MAX)) {
      if (slots_[previous].uid != enabled[index]->uid) {
        continue;
      }
      if (index != previous) {
        /* Move by assignment to transfer Texture ownership safely. */
        RuntimeSlot displaced;
        displaced = std::move(slots_[index]);
        slots_[index] = std::move(slots_[previous]);
        slots_[previous] = std::move(displaced);
        std::swap(data_[index], data_[previous]);
      }
      break;
    }
  }

  int slot_index = 0;
  for (SceneRenderTexture *render_texture : enabled) {
    RuntimeSlot &slot = slots_[slot_index];
    const int2 extent(max_ii(render_texture->resolution_x, 1),
                      max_ii(render_texture->resolution_y, 1));
    const int source = render_texture_source_normalize(render_texture->source);
    const int update_mode = render_texture->update_mode;
    const bool reset_history = slot.uid != render_texture->uid ||
                               slot.camera_orig != render_texture->camera ||
                               slot.extent != extent || slot.source != source ||
                               slot.format != render_texture->format;
    const bool update_mode_changed = slot.update_mode != update_mode;
    const bool manual_update_requested = slot.manual_update != render_texture->manual_update;
    slot.uid = render_texture->uid;
    slot.camera = DEG_get_evaluated(inst_.depsgraph, render_texture->camera);
    slot.camera_orig = render_texture->camera;
    slot.extent = extent;
    slot.source = source;
    slot.format = render_texture->format;
    slot.update_mode = update_mode;
    slot.manual_update = render_texture->manual_update;
    slot.active = (slot.camera != nullptr);
    slot.compositor_output = render_texture->use_compositor_output && !inst_.is_pre_fx_render &&
                             !inst_.is_dlss_guide_render;
    slot.requires_full_capture = false;
    slot.sync_camera_valid = slot.active &&
                             camera_data_from_object(
                                 inst_.scene, slot.camera, slot.extent, slot.sync_camera_data);
    if (reset_history) {
      slot.history_reset_pending = true;
      slot.rt_buffer_opaque.reset();
      slot.rt_buffer_refract.reset();
      slot.has_camera_data = false;
      data_[slot_index] = slot_default_data();
    }
    slot.capture_pending |= reset_history || update_mode_changed || manual_update_requested;
    slot.capture_pending |= evaluation_changed &&
                            update_mode != SCE_EEVEE_RENDER_TEXTURE_UPDATE_MANUAL;

    data_[slot_index].info = int4(
        slot.uid, slot.extent.x, slot.extent.y, render_texture_info_flags(slot.source, slot.format));
    slot_index++;
  }

  /* Release inactive full-size captures, but retain valid dummy bindings. */
  for (; slot_index < RENDER_TEXTURE_SLOT_MAX; slot_index++) {
    slot_reset(slot_index);
    slot_ensure_textures(slot_index);
  }

  data_.push_update();
}

bool RenderTextureModule::foreach_enabled_camera(
    FunctionRef<void(const CameraData &)> callback) const
{
  for (const RuntimeSlot &slot : slots_) {
    if (slot.active) {
      if (!slot.sync_camera_valid) {
        return false;
      }
      callback(slot.sync_camera_data);
    }
  }
  return true;
}

void RenderTextureModule::material_sync(GPUMaterial *material,
                                        const bool is_capture_material,
                                        const bool is_object_material)
{
  if (material == nullptr) {
    return;
  }
  const Span<int> ids = GPU_material_render_texture_ids(material);
  for (const int uid : ids) {
    used_uids_.add(uid);
  }
  if (!is_capture_material) {
    return;
  }
  capture_has_texture_dependencies_ |= !ids.is_empty();
  for (const int uid : ids) {
    capture_read_uids_.add(uid);
  }
  if (!is_object_material) {
    requires_full_capture_ |= material_requires_full_capture(material);
  }
}

bool RenderTextureModule::material_requires_full_capture(GPUMaterial *material)
{
  if (material == nullptr) {
    return false;
  }

  constexpr eGPUMaterialFlag dependencies = GPU_MATFLAG_SHADER_TO_RGBA | GPU_MATFLAG_AO |
                                            GPU_MATFLAG_RAYCAST | GPU_MATFLAG_SHADER_INFO |
                                            GPU_MATFLAG_NPR | GPU_MATFLAG_NPR_REFRACTION |
                                            GPU_MATFLAG_NPR_FOREACH_LIGHT |
                                            GPU_MATFLAG_SCENE_COLOR |
                                            GPU_MATFLAG_SCREENSPACE_INFO |
                                            GPU_MATFLAG_LIGHTPROBE_ACCESS |
                                            GPU_MATFLAG_GLSL_LIGHT_ACCESS;
  bool full = (GPU_material_flag(material) & dependencies) != 0 ||
              GPU_material_stencil_value_used(material) || GPU_material_uses_hiz_data(material) ||
              GPU_material_has_glsl_light_shader_eval(material) ||
              GPU_material_has_depth_offset_output(material);
  if (const blender::Material *source = GPU_material_get_material(material)) {
    full |= source->stencil_enabled || source->stencil_mode != MA_STENCIL_STANDARD ||
            material_ztest_mode_get(*source) != MA_ZTEST_LESS_EQUAL;
  }
  return full;
}

void RenderTextureModule::object_sync(const ObjectHandle &object, const Material &material)
{
  /* Color always uses the complete pipeline. Once every relevant view needs that pipeline,
   * shared materials on subsequent objects must not pay for more dependency/bounds queries. */
  const auto needs_visibility_test = [](const RuntimeSlot &slot) {
    return slot.active && slot.source != SCE_EEVEE_RENDER_TEXTURE_SOURCE_COLOR &&
           !slot.requires_full_capture;
  };
  if (requires_full_capture_ || std::none_of(slots_.begin(), slots_.end(), needs_visibility_test))
  {
    return;
  }
  bool full = false;
  bool unbounded = object.object->type != OB_MESH || object.object->mode != OB_MODE_OBJECT ||
                   material.tessellation_level > 1 || material.outline_shell_bounds_inflation > 0;
  for (GPUMaterial *gpumat : {material.shading.gpumat,
                              material.prepass.gpumat,
                              material.npr.gpumat,
                              material.stencil.gpumat,
                              material.outline_shell_shading.gpumat,
                              material.volume_material.gpumat})
  {
    full |= material_requires_full_capture(gpumat);
    if (gpumat) {
      unbounded |= GPU_material_has_displacement_output(gpumat) ||
                   GPU_material_has_depth_offset_output(gpumat);
    }
  }
  if (!full) {
    return;
  }
  /* Match the evaluated mesh actually drawn, including modifiers and geometry nodes. */
  const auto bounds = unbounded ?
                          std::nullopt :
                          DRW_object_get_data_for_drawing<Mesh>(*object.object).bounds_min_max();
  for (RuntimeSlot &slot : slots_) {
    if (!needs_visibility_test(slot)) {
      continue;
    }
    bool visible = unbounded || !bounds || !slot.sync_camera_valid;
    if (!visible) {
      for (const int instance : IndexRange(object.instances_count())) {
        const float4x4 matrix = slot.sync_camera_data.persmat * object.object_to_world(instance);
        bool outside[4] = {true, true, true, true};
        bool uncertain = false;
        for (int corner = 0; corner < 8; corner++) {
          const float3 p((corner & 1) ? bounds->max.x : bounds->min.x,
                         (corner & 2) ? bounds->max.y : bounds->min.y,
                         (corner & 4) ? bounds->max.z : bounds->min.z);
          const float4 clip = matrix * float4(p, 1.0f);
          /* Crossings of the camera plane, deformation and missing bounds retain full capture.
           * Use only lateral planes: custom fragment depth must never invalidate this bound. */
          if (!(clip.w > 1e-5f) || !std::isfinite(clip.x) || !std::isfinite(clip.y)) {
            uncertain = true;
            break;
          }
          const float margin = 1e-4f * std::max(1.0f, std::abs(clip.w));
          outside[0] &= clip.x < -clip.w - margin;
          outside[1] &= clip.x > clip.w + margin;
          outside[2] &= clip.y < -clip.w - margin;
          outside[3] &= clip.y > clip.w + margin;
        }
        visible |= uncertain || !(outside[0] || outside[1] || outside[2] || outside[3]);
        if (visible) {
          break;
        }
      }
    }
    slot.requires_full_capture |= visible;
  }
}

void RenderTextureModule::end_sync()
{
  for (const int index : IndexRange(RENDER_TEXTURE_SLOT_MAX)) {
    RuntimeSlot &slot = slots_[index];
    const bool was_used = slot.used;
    slot.used = slot.active && (used_uids_.contains(slot.uid) || slot.compositor_output);
    slot.needs_history = capture_read_uids_.contains(slot.uid);
    slot.capture_pending |= slot.used && !was_used;
    slot.capture_pending |= inst_.is_viewport() && inst_.sampling.is_reset() &&
                            slot.update_mode != SCE_EEVEE_RENDER_TEXTURE_UPDATE_MANUAL;
    if (!slot.used) {
      slot.rt_buffer_opaque.reset();
      slot.rt_buffer_refract.reset();
      slot.has_camera_data = false;
      slot.buffers.reset();
      data_[index] = slot_default_data();
    }
    const uint8_t recreated = slot_ensure_textures(index);
    if (slot.history_reset_pending && slot.used) {
      if ((recreated & SLOT_TEXTURE_CURRENT_RECREATED) == 0) {
        slot.color_tx.current().clear(float4(0.0f));
      }
      if ((recreated & SLOT_TEXTURE_PREVIOUS_RECREATED) == 0) {
        slot.color_tx.previous().clear(float4(0.0f));
      }
      slot.history_reset_pending = false;
    }
    if (slot.used) {
      CLOG_INFO(&Instance::log,
                "RenderTexture output uid=%d current=%dx%d history=%dx%d",
                slot.uid,
                GPU_texture_width(slot.color_tx.current()),
                GPU_texture_height(slot.color_tx.current()),
                GPU_texture_width(slot.color_tx.previous()),
                GPU_texture_height(slot.color_tx.previous()));
      data_[index].info = int4(slot.uid,
                               slot.extent.x,
                               slot.extent.y,
                               render_texture_info_flags(slot.source, slot.format));
      /* A capture request also changes the main view's shader input. Scene time can advance
       * without any other time-dependent materials, so invalidate a converged Film here. */
      if (slot.capture_pending && inst_.is_viewport()) {
        inst_.sampling.reset();
      }
    }
  }
  data_.push_update();
}

void RenderTextureModule::slot_extract(const int slot_index, RenderBuffers &rbufs)
{
  RuntimeSlot &slot = slots_[slot_index];

  extract_ps_.init();
  switch (slot.format) {
    case SCE_EEVEE_RENDER_TEXTURE_FORMAT_R16F:
      extract_ps_.shader_set(inst_.shaders.static_shader_get(RENDER_TEXTURE_EXTRACT_R16F));
      break;
    case SCE_EEVEE_RENDER_TEXTURE_FORMAT_R32F:
      extract_ps_.shader_set(inst_.shaders.static_shader_get(RENDER_TEXTURE_EXTRACT_R32F));
      break;
    case SCE_EEVEE_RENDER_TEXTURE_FORMAT_RGBA32F:
      extract_ps_.shader_set(inst_.shaders.static_shader_get(RENDER_TEXTURE_EXTRACT_RGBA32F));
      break;
    case SCE_EEVEE_RENDER_TEXTURE_FORMAT_RGBA16F:
    default:
      extract_ps_.shader_set(inst_.shaders.static_shader_get(RENDER_TEXTURE_EXTRACT_RGBA16F));
      break;
  }
  extract_ps_.bind_texture("depth_tx", &rbufs.depth_tx);
  extract_ps_.bind_texture("combined_tx", &rbufs.combined_tx);
  extract_ps_.bind_texture("gbuf_header_tx", &inst_.gbuffer.header_tx);
  extract_ps_.bind_texture("gbuf_normal_tx", &inst_.gbuffer.normal_tx);
  extract_ps_.bind_image("output_img", &slot.color_tx.current());
  extract_ps_.push_constant("output_extent", slot.extent);
  extract_ps_.push_constant("output_type", slot.source);
  extract_ps_.dispatch(math::divide_ceil(slot.extent, int2(FILM_GROUP_SIZE)));
  extract_ps_.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  inst_.manager->submit(extract_ps_);
}

bool RenderTextureModule::slot_should_capture(const RuntimeSlot &slot) const
{
  if (!slot.active || !slot.used) {
    return false;
  }

  switch (slot.update_mode) {
    case SCE_EEVEE_RENDER_TEXTURE_UPDATE_EVERY_SAMPLE:
      return true;
    case SCE_EEVEE_RENDER_TEXTURE_UPDATE_MANUAL:
      return slot.capture_pending;
    case SCE_EEVEE_RENDER_TEXTURE_UPDATE_EVERY_FRAME:
    default:
      /* Scene edits and motion-blur evaluation steps tag a new capture in begin_sync(). A
       * converging viewport must not advance feedback once for every unrelated TAA sample. */
      return slot.capture_pending;
  }
}

void RenderTextureModule::slot_capture(const Span<int> capture_slots)
{
  const int slot_index = capture_slots[0];
  RuntimeSlot &slot = slots_[slot_index];
  if (!slot.active) {
    return;
  }

  CameraData rt_camera;
  if (!camera_data_from_object(inst_.scene, slot.camera, slot.extent, rt_camera)) {
    slot_reset(slot_index);
    slot_ensure_textures(slot_index);
    data_.push_update();
    return;
  }

  if (!slot.has_camera_data || slot.camera_data != rt_camera) {
    if (slot.rt_buffer_opaque) {
      slot.rt_buffer_opaque->invalidate_history();
    }
    if (slot.rt_buffer_refract) {
      slot.rt_buffer_refract->invalidate_history();
    }
  }
  slot.camera_data = rt_camera;
  slot.has_camera_data = true;

  const float4x4 camera_view_basis = math::transpose(rt_camera.viewmat);
  for (const int index : capture_slots) {
    RuntimeSlot &capture = slots_[index];
    if (capture.needs_history) {
      capture.color_tx.swap();
    }
    data_[index].prev_viewproj = data_[index].viewproj;
    data_[index].viewproj = rt_camera.persmat;
    data_[index].camera_position = float4(rt_camera.viewinv.location(), 1.0f);
    data_[index].camera_axis_x = float4(camera_view_basis.x_axis(), 0.0f);
    data_[index].camera_axis_y = float4(camera_view_basis.y_axis(), 0.0f);
    data_[index].camera_axis_z = float4(camera_view_basis.z_axis(), 0.0f);
    data_[index].info.w = render_texture_info_flags(capture.source, capture.format) |
                          RENDER_TEXTURE_SLOT_CAPTURING;
  }
  data_.push_update();

  const bool cache_render_buffers = slot.extent != inst_.film.render_extent_get();
  const bool cache_hiz = !inst_.hiz_buffer.storage_matches_extent(slot.extent);
  if (!cache_render_buffers && !cache_hiz) {
    slot.buffers.reset();
  }
  if ((cache_render_buffers || cache_hiz) &&
      (!slot.buffers || slot.buffers->extent != slot.extent))
  {
    slot.buffers.reset();
    for (const RuntimeSlot &other : slots_) {
      if (other.buffers && other.buffers->extent == slot.extent) {
        slot.buffers = other.buffers;
        break;
      }
    }
    if (!slot.buffers) {
      slot.buffers = std::make_shared<CaptureBuffers>();
      slot.buffers->extent = slot.extent;
    }
  }
  swap_capture_buffers(slot, cache_render_buffers, cache_hiz);
  for (const int index : capture_slots) {
    slots_[index].buffers = slot.buffers;
  }
  inst_.render_extent_override_set(slot.extent);
  inst_.lights.sync_render_extent(slot.extent);
  const int hiz_created = inst_.hiz_buffer.sync();
  inst_.camera.override(rt_camera, true);
  inst_.uniform_data.push_update();

  View main_view = {"RenderTexture.Main"};
  View render_view = {"RenderTexture.Render"};
  main_view.sync(rt_camera.viewmat, rt_camera.winmat);
  render_view.sync(rt_camera.viewmat, rt_camera.winmat);

  RenderBuffers &rbufs = inst_.render_buffers;
  const int buffers_created = rbufs.acquire(slot.extent);

  const bool full_capture = requires_full_capture_ ||
                            std::any_of(
                                capture_slots.begin(), capture_slots.end(), [&](int index) {
                                  return slots_[index].requires_full_capture;
                                });
  const bool depth_only = !full_capture && slot.source == SCE_EEVEE_RENDER_TEXTURE_SOURCE_DEPTH;
  const bool normal_only = !full_capture && slot.source == SCE_EEVEE_RENDER_TEXTURE_SOURCE_NORMAL;
  /* Empty hybrid subpasses still bind planar resources. Set them up before acquiring the
   * capture GBuffer because planar setup releases its own temporary GBuffer. */
  if (normal_only) {
    inst_.planar_probes.ensure_resources();
  }
  else if (!depth_only) {
    inst_.planar_probes.set_view(render_view, slot.extent);
  }

  combined_fb_.ensure(GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
                      GPU_ATTACHMENT_TEXTURE(rbufs.combined_tx));

  const bool with_raycast = inst_.pipelines.has_raycast;
  const bool with_prepass_normal = with_raycast || inst_.lights.needs_front_light_shader();
  prepass_fb_.ensure(
      GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
      with_prepass_normal ? GPU_ATTACHMENT_TEXTURE(rbufs.prepass_normal_tx) : GPU_ATTACHMENT_NONE,
      with_raycast ? GPU_ATTACHMENT_TEXTURE(rbufs.object_id_tx) : GPU_ATTACHMENT_NONE,
      GPU_ATTACHMENT_TEXTURE(rbufs.vector_tx));

  inst_.gbuffer.acquire(depth_only ? int2(1) : slot.extent,
                        inst_.pipelines.deferred.header_layer_count(),
                        inst_.pipelines.deferred.closure_layer_count(),
                        inst_.pipelines.deferred.normal_layer_count());
  if (!depth_only) {
    gbuffer_fb_.ensure(GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
                       GPU_ATTACHMENT_TEXTURE(rbufs.combined_tx),
                       GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.header_tx.layer_view(0), 0),
                       GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.normal_tx.layer_view(0), 0),
                       GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.closure_tx.layer_view(0), 0),
                       GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.closure_tx.layer_view(1), 0));
  }

  const float4 clear_velocity(inst_.velocity.camera_has_motion() ? VELOCITY_INVALID : 0.0f);
  GPU_texture_clear(rbufs.vector_tx, GPU_DATA_FLOAT, &clear_velocity);
  if (with_raycast) {
    rbufs.object_id_tx.clear(uint4(0));
  }
  if (with_prepass_normal) {
    rbufs.prepass_normal_tx.clear(float4(0.0f));
  }

  const double4 clear_color = double4(0.0, 0.0, 0.0, 1.0);
  GPU_framebuffer_bind(combined_fb_);
  GPU_framebuffer_clear_color_depth(combined_fb_, clear_color, inst_.film.depth.clear_value);

  inst_.lights.set_view(render_view, slot.extent);
  inst_.hiz_buffer.set_source(&inst_.render_buffers.depth_tx);

  CLOG_INFO(&Instance::log,
            "RenderTexture capture uid=%d outputs=%d path=%s extent=%dx%d storage_created=%d",
            slot.uid,
            int(capture_slots.size()),
            depth_only  ? "depth" :
            normal_only ? "normal" :
                          "color",
            slot.extent.x,
            slot.extent.y,
            hiz_created + buffers_created);
  inst_.gbuffer.header_tx.clear(uint4(0));
  if (depth_only || normal_only) {
    slot.rt_buffer_opaque.reset();
    slot.rt_buffer_refract.reset();
  }
  if (depth_only) {
    inst_.pipelines.deferred.render_prepass_only(render_view, prepass_fb_);
    inst_.pipelines.forward.render_prepass_only(render_view, prepass_fb_);
  }
  else if (normal_only) {
    inst_.pipelines.deferred.render_gbuffer_only(render_view, prepass_fb_, gbuffer_fb_);
  }
  else {
    if (!slot.rt_buffer_opaque) {
      slot.rt_buffer_opaque = std::make_unique<RayTraceBuffer>();
      slot.rt_buffer_refract = std::make_unique<RayTraceBuffer>();
    }
    inst_.pipelines.background.clear(render_view);
    inst_.volume.set_view(main_view, slot.extent, false);
    inst_.uniform_data.data.push_update();
    inst_.volume.draw_prepass(main_view);
    inst_.pipelines.background.render(render_view, combined_fb_);

    bool volume_compute_done = false;
    inst_.pipelines.deferred.render(main_view,
                                    render_view,
                                    prepass_fb_,
                                    combined_fb_,
                                    gbuffer_fb_,
                                    slot.extent,
                                    *slot.rt_buffer_opaque,
                                    *slot.rt_buffer_refract,
                                    volume_compute_done);

    if (!volume_compute_done) {
      inst_.volume.draw_compute(main_view, slot.extent);
    }
    inst_.volume.draw_resolve(main_view);
    inst_.ambient_occlusion.render_pass(render_view);
    inst_.pipelines.forward.render(
        render_view, rbufs.depth_tx, prepass_fb_, transparent_fb_, combined_fb_, slot.extent);
  }

  for (const int index : capture_slots) {
    slot_extract(index, rbufs);
    data_[index].info.w &= ~RENDER_TEXTURE_SLOT_CAPTURING;
    slots_[index].capture_pending = false;
  }

  inst_.gbuffer.release();
  rbufs.release();
  swap_capture_buffers(slot, cache_render_buffers, cache_hiz);
  for (const int index : capture_slots) {
    if (slots_[index].update_mode == SCE_EEVEE_RENDER_TEXTURE_UPDATE_MANUAL) {
      slots_[index].buffers.reset();
    }
  }

  data_.push_update();
}

void RenderTextureModule::swap_capture_buffers(RuntimeSlot &slot,
                                               const bool render_buffers,
                                               const bool hiz)
{
  if (render_buffers) {
    Texture::swap(inst_.render_buffers.depth_tx, slot.buffers->depth_tx);
    Texture::swap(inst_.render_buffers.rp_color_tx, slot.buffers->rp_color_tx);
    Texture::swap(inst_.render_buffers.rp_value_tx, slot.buffers->rp_value_tx);
  }
  if (hiz) {
    inst_.hiz_buffer.swap_textures(slot.buffers->hiz_tx);
  }
}

void RenderTextureModule::render()
{
  /* Retain pending captures until their materials are ready. In particular, Manual must not
   * permanently cache a fallback material or a texture that is still loading. */
  if (inst_.materials.queued_shaders_count > 0 || inst_.materials.queued_textures_count > 0) {
    return;
  }

  if (std::none_of(slots_.begin(), slots_.end(), [&](const RuntimeSlot &slot) {
        return slot_should_capture(slot);
      }))
  {
    return;
  }

  const CameraData main_camera = inst_.camera.data_get();
  const bool main_is_camera_object = inst_.camera.is_camera_object();
  std::array<bool, RENDER_TEXTURE_SLOT_MAX> captured = {};
  /* Temporal buffers belong to individual slots. Sharing only the extracted image would leave
   * followers without history when a different update cadence changes the next capture group. */
  const auto can_batch = [&](const RuntimeSlot &slot) {
    return !inst_.raytracing.use_raytracing() ||
           (!requires_full_capture_ && !slot.requires_full_capture &&
            (slot.source == SCE_EEVEE_RENDER_TEXTURE_SOURCE_DEPTH ||
             slot.source == SCE_EEVEE_RENDER_TEXTURE_SOURCE_NORMAL));
  };

  for (int slot_index = 0; slot_index < RENDER_TEXTURE_SLOT_MAX; slot_index++) {
    if (captured[slot_index] || !slot_should_capture(slots_[slot_index])) {
      continue;
    }
    Vector<int, RENDER_TEXTURE_SLOT_MAX> captures;
    captures.append(slot_index);
    if (!capture_has_texture_dependencies_ && can_batch(slots_[slot_index])) {
      /* Captures that sample Render Textures keep their original serial feedback ordering. */
      for (int index = slot_index + 1; index < RENDER_TEXTURE_SLOT_MAX; index++) {
        const RuntimeSlot &slot = slots_[index];
        if (!captured[index] && slot_should_capture(slot) && can_batch(slot) &&
            slot.camera == slots_[slot_index].camera && slot.extent == slots_[slot_index].extent)
        {
          captures.append(index);
        }
      }
      /* One color render also supplies depth and GBuffer normals. A normal render supplies depth.
       */
      const auto priority = [&](const int index) {
        return slots_[index].source == SCE_EEVEE_RENDER_TEXTURE_SOURCE_COLOR  ? 2 :
               slots_[index].source == SCE_EEVEE_RENDER_TEXTURE_SOURCE_NORMAL ? 1 :
                                                                                0;
      };
      const auto leader = std::max_element(
          captures.begin(), captures.end(), [&](const int a, const int b) {
            return priority(a) < priority(b);
          });
      std::swap(captures[0], *leader);
    }
    slot_capture(captures);
    for (const int index : captures) {
      captured[index] = true;
    }
  }

  inst_.render_extent_override_clear();
  inst_.lights.sync_render_extent(inst_.film.render_extent_get());
  inst_.hiz_buffer.sync();
  inst_.camera.override(main_camera, main_is_camera_object);
  inst_.uniform_data.push_update();
}

void RenderTextureModule::slot_export(RuntimeSlot &slot, gpu::Texture *output, int2 sample_offset)
{
  const int2 extent(GPU_texture_width(output), GPU_texture_height(output));
  const bool single_channel = ELEM(
      slot.format, SCE_EEVEE_RENDER_TEXTURE_FORMAT_R16F, SCE_EEVEE_RENDER_TEXTURE_FORMAT_R32F);
  const bool value_output = GPU_texture_format(output) == gpu::TextureFormat::SFLOAT_32;
  PassSimple pass("RenderTexture.Export");
  pass.init();
  pass.shader_set(inst_.shaders.static_shader_get(value_output ? RENDER_TEXTURE_EXPORT_VALUE :
                                                                 RENDER_TEXTURE_EXPORT_COLOR));
  const GPUSamplerState sampler = slot.source == SCE_EEVEE_RENDER_TEXTURE_SOURCE_DEPTH ?
                                      GPUSamplerState{GPU_SAMPLER_FILTERING_DEFAULT} :
                                      GPUSamplerState{GPU_SAMPLER_FILTERING_LINEAR};
  pass.bind_texture("capture_tx", slot.color_tx.current(), sampler);
  pass.bind_image("output_img", output);
  pass.push_constant("output_extent", extent);
  pass.push_constant("sample_offset", sample_offset);
  pass.push_constant("display_extent", inst_.film.display_extent_get());
  pass.push_constant("film_offset", inst_.film.film_offset_get());
  pass.push_constant("film_extent", inst_.film.film_extent_get());
  pass.push_constant("single_channel", single_channel);
  pass.push_constant("invert_alpha",
                     !single_channel && slot.source == SCE_EEVEE_RENDER_TEXTURE_SOURCE_COLOR);
  pass.dispatch(math::divide_ceil(extent, int2(FILM_GROUP_SIZE)));
  pass.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_TEXTURE_UPDATE);
  inst_.manager->submit(pass);
}

float *RenderTextureModule::read_pass(const SceneRenderTexture &render_texture,
                                      const int2 extent,
                                      const int channels)
{
  /* Render Result owns the pass layout. Keep the readback allocation consistent with its
   * channel count when capture storage settings change. */
  if (!ELEM(channels, 1, 4)) {
    return nullptr;
  }
  for (RuntimeSlot &slot : slots_) {
    if (slot.uid != render_texture.uid || !slot.used || !slot.compositor_output) {
      continue;
    }
    TextureFromPool output("RenderTexture.Export");
    output.acquire_2d(extent,
                      channels == 1 ? gpu::TextureFormat::SFLOAT_32 :
                                      gpu::TextureFormat::SFLOAT_32_32_32_32,
                      GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE |
                          GPU_TEXTURE_USAGE_HOST_READ);
    slot_export(slot, output, inst_.film.film_offset_get());
    return static_cast<float *>(GPU_texture_read(output, GPU_DATA_FLOAT, 0));
  }
  return nullptr;
}

void RenderTextureModule::write_viewport_compositor_passes()
{
  if (inst_.is_pre_fx_render || inst_.is_dlss_guide_render ||
      std::none_of(slots_.begin(), slots_.end(), [](const RuntimeSlot &slot) {
        return slot.used && slot.compositor_output;
      }))
  {
    return;
  }
  const Set<std::string> needed = bke::compositor::get_used_passes(*inst_.scene, inst_.view_layer);
  for (const SceneRenderTexture *render_texture =
           static_cast<const SceneRenderTexture *>(inst_.scene->eevee.render_textures.first);
       render_texture;
       render_texture = render_texture->next)
  {
    if (!render_texture->use_compositor_output) {
      continue;
    }
    const std::string name = BKE_scene_render_texture_pass_name(*render_texture);
    if (!needed.contains(name)) {
      continue;
    }
    for (RuntimeSlot &slot : slots_) {
      if (slot.uid != render_texture->uid || !slot.used || !slot.compositor_output) {
        continue;
      }
      TextureFromPool &output = DRW_viewport_pass_texture_get(name.c_str());
      output.acquire_2d(inst_.film.display_extent_get(),
                        BKE_scene_render_texture_pass_is_value(*render_texture) ?
                            gpu::TextureFormat::SFLOAT_32 :
                            gpu::TextureFormat::SFLOAT_32_32_32_32,
                        GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE);
      slot_export(slot, output, int2(0));
      break;
    }
  }
}

}  // namespace blender::eevee
