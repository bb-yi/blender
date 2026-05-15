/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "BLI_listbase.h"

#include "DEG_depsgraph_query.hh"

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
  slots_[slot_index].uid = -1;
  slots_[slot_index].camera = nullptr;
  slots_[slot_index].extent = int2(1);
  slots_[slot_index].source = SCE_EEVEE_RENDER_TEXTURE_SOURCE_COLOR;
  slots_[slot_index].format = SCE_EEVEE_RENDER_TEXTURE_FORMAT_RGBA16F;
  slots_[slot_index].active = false;
  data_[slot_index] = slot_default_data();
}

void RenderTextureModule::slot_ensure_textures(const int slot_index)
{
  RuntimeSlot &slot = slots_[slot_index];
  const float4 clear_color(0.0f);
  const gpu::TextureFormat texture_format = render_texture_gpu_format(slot.format);

  const bool recreated_current = slot.color_tx.current().ensure_2d(
      texture_format, slot.extent, GPU_TEXTURE_USAGE_GENERAL);
  const bool recreated_previous = slot.color_tx.previous().ensure_2d(
      texture_format, slot.extent, GPU_TEXTURE_USAGE_GENERAL);

  if (recreated_current) {
    slot.color_tx.current().clear(clear_color);
  }
  if (recreated_previous) {
    slot.color_tx.previous().clear(clear_color);
  }
}

void RenderTextureModule::init()
{
  for (int slot_index = 0; slot_index < RENDER_TEXTURE_SLOT_MAX; slot_index++) {
    slot_reset(slot_index);
    slot_ensure_textures(slot_index);
  }
  data_.push_update();
}

void RenderTextureModule::begin_sync()
{
  for (int slot_index = 0; slot_index < RENDER_TEXTURE_SLOT_MAX; slot_index++) {
    slot_reset(slot_index);
  }

  int slot_index = 0;
  for (SceneRenderTexture *render_texture = static_cast<SceneRenderTexture *>(
           inst_.scene->eevee.render_textures.first);
       render_texture != nullptr;
       render_texture = render_texture->next)
  {
    if (slot_index >= RENDER_TEXTURE_SLOT_MAX) {
      break;
    }
    if (!render_texture->enabled || render_texture->camera == nullptr) {
      continue;
    }

    RuntimeSlot &slot = slots_[slot_index];
    slot.uid = render_texture->uid;
    slot.camera = DEG_get_evaluated(inst_.depsgraph, render_texture->camera);
    slot.extent = int2(max_ii(render_texture->resolution_x, 1),
                       max_ii(render_texture->resolution_y, 1));
    slot.source = render_texture_source_normalize(render_texture->source);
    slot.format = render_texture->format;
    slot.active = (slot.camera != nullptr);

    data_[slot_index].info = int4(
        slot.uid, slot.extent.x, slot.extent.y, render_texture_info_flags(slot.source, slot.format));
    slot_index++;
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

void RenderTextureModule::slot_capture(const int slot_index)
{
  RuntimeSlot &slot = slots_[slot_index];
  if (!slot.active) {
    return;
  }

  CameraData rt_camera;
  if (!camera_data_from_object(inst_.scene, slot.camera, slot.extent, rt_camera)) {
    slot_reset(slot_index);
    data_.push_update();
    return;
  }

  slot.color_tx.swap();
  slot_ensure_textures(slot_index);

  data_[slot_index].prev_viewproj = data_[slot_index].viewproj;
  data_[slot_index].viewproj = rt_camera.persmat;
  const float4x4 camera_view_basis = math::transpose(rt_camera.viewmat);
  data_[slot_index].camera_position = float4(rt_camera.viewinv.location(), 1.0f);
  data_[slot_index].camera_axis_x = float4(camera_view_basis.x_axis(), 0.0f);
  data_[slot_index].camera_axis_y = float4(camera_view_basis.y_axis(), 0.0f);
  data_[slot_index].camera_axis_z = float4(camera_view_basis.z_axis(), 0.0f);
  data_[slot_index].info = int4(slot.uid,
                                slot.extent.x,
                                slot.extent.y,
                                render_texture_info_flags(slot.source, slot.format) |
                                    RENDER_TEXTURE_SLOT_CAPTURING);
  data_.push_update();

  inst_.render_extent_override_set(slot.extent);
  inst_.lights.sync_render_extent(slot.extent);
  inst_.hiz_buffer.sync();
  inst_.camera.override(rt_camera, true);
  inst_.uniform_data.push_update();

  View main_view = {"RenderTexture.Main"};
  View render_view = {"RenderTexture.Render"};
  main_view.sync(rt_camera.viewmat, rt_camera.winmat);
  render_view.sync(rt_camera.viewmat, rt_camera.winmat);

  RenderBuffers &rbufs = inst_.render_buffers;
  rbufs.acquire(slot.extent);

  inst_.planar_probes.set_view(render_view, slot.extent);

  combined_fb_.ensure(GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
                      GPU_ATTACHMENT_TEXTURE(rbufs.combined_tx));

  const bool with_raycast = inst_.pipelines.has_raycast;
  const bool with_prepass_normal = with_raycast || inst_.lights.needs_front_light_shader();
  prepass_fb_.ensure(
      GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
      with_prepass_normal ? GPU_ATTACHMENT_TEXTURE(rbufs.prepass_normal_tx) : GPU_ATTACHMENT_NONE,
      with_raycast ? GPU_ATTACHMENT_TEXTURE(rbufs.object_id_tx) : GPU_ATTACHMENT_NONE,
      GPU_ATTACHMENT_TEXTURE(rbufs.vector_tx));

  inst_.gbuffer.acquire(slot.extent,
                        inst_.pipelines.deferred.header_layer_count(),
                        inst_.pipelines.deferred.closure_layer_count(),
                        inst_.pipelines.deferred.normal_layer_count());
  gbuffer_fb_.ensure(GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
                     GPU_ATTACHMENT_TEXTURE(rbufs.combined_tx),
                     GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.header_tx.layer_view(0), 0),
                     GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.normal_tx.layer_view(0), 0),
                     GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.closure_tx.layer_view(0), 0),
                     GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.closure_tx.layer_view(1), 0));

  const float4 clear_velocity(inst_.velocity.camera_has_motion() ? VELOCITY_INVALID : 0.0f);
  GPU_texture_clear(rbufs.vector_tx, GPU_DATA_FLOAT, &clear_velocity);
  if (with_raycast) {
    rbufs.object_id_tx.clear(uint4(0));
  }
  if (with_prepass_normal) {
    rbufs.prepass_normal_tx.clear(float4(0.0f));
  }

  const float4 clear_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
  GPU_framebuffer_bind(combined_fb_);
  GPU_framebuffer_clear_color_depth(combined_fb_, clear_color, inst_.film.depth.clear_value);
  inst_.pipelines.background.clear(render_view);

  inst_.lights.set_view(render_view, slot.extent);
  inst_.hiz_buffer.set_source(&inst_.render_buffers.depth_tx);

  inst_.volume.draw_prepass(main_view);
  inst_.pipelines.deferred.render(main_view,
                                  render_view,
                                  prepass_fb_,
                                  combined_fb_,
                                  gbuffer_fb_,
                                  slot.extent,
                                  rt_buffer_opaque_,
                                  rt_buffer_refract_);
  inst_.pipelines.background.render(render_view, combined_fb_);

  inst_.gbuffer.release();

  inst_.volume.draw_compute(main_view, slot.extent);
  inst_.volume.draw_resolve(main_view);
  inst_.ambient_occlusion.render_pass(render_view);
  inst_.pipelines.forward.render(
      render_view, rbufs.depth_tx, prepass_fb_, transparent_fb_, combined_fb_, slot.extent);

  slot_extract(slot_index, rbufs);

  rbufs.release();

  data_[slot_index].info.w &= ~RENDER_TEXTURE_SLOT_CAPTURING;
  data_.push_update();
}

void RenderTextureModule::render()
{
  const CameraData main_camera = inst_.camera.data_get();
  const bool main_is_camera_object = inst_.camera.is_camera_object();

  for (int slot_index = 0; slot_index < RENDER_TEXTURE_SLOT_MAX; slot_index++) {
    if (!slots_[slot_index].active) {
      continue;
    }
    slot_capture(slot_index);
  }

  inst_.render_extent_override_clear();
  inst_.lights.sync_render_extent(inst_.film.render_extent_get());
  inst_.hiz_buffer.sync();
  inst_.camera.override(main_camera, main_is_camera_object);
  inst_.uniform_data.push_update();
}

}  // namespace blender::eevee
