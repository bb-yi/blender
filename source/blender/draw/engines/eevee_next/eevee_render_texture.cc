/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "BLI_listbase.h"

#include "DEG_depsgraph_query.hh"

#include "DNA_scene_types.h"

#include "GPU_texture.hh"

#include "eevee_instance.hh"
#include "eevee_render_texture.hh"

namespace blender::eevee {

RenderTextureData RenderTextureModule::slot_default_data()
{
  RenderTextureData data = {};
  data.viewproj = float4x4::identity();
  data.prev_viewproj = float4x4::identity();
  data.info = int4(-1, 0, 0, 0);
  return data;
}

void RenderTextureModule::slot_reset(const int slot_index)
{
  slots_[slot_index].uid = -1;
  slots_[slot_index].camera = nullptr;
  slots_[slot_index].extent = int2(1);
  slots_[slot_index].active = false;
  data_[slot_index] = slot_default_data();
}

void RenderTextureModule::slot_ensure_textures(const int slot_index)
{
  RuntimeSlot &slot = slots_[slot_index];
  const float4 clear_color(0.0f);

  const bool recreated_current = slot.color_tx.current().ensure_2d(
      GPU_RGBA16F, slot.extent, GPU_TEXTURE_USAGE_GENERAL);
  const bool recreated_previous = slot.color_tx.previous().ensure_2d(
      GPU_RGBA16F, slot.extent, GPU_TEXTURE_USAGE_GENERAL);

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
  LISTBASE_FOREACH (SceneRenderTexture *, render_texture, &inst_.scene->eevee.render_textures) {
    if (slot_index >= RENDER_TEXTURE_SLOT_MAX) {
      break;
    }
    if (!render_texture->enabled || render_texture->camera == nullptr) {
      continue;
    }

    RuntimeSlot &slot = slots_[slot_index];
    slot.uid = render_texture->uid;
    slot.camera = DEG_get_evaluated_object(inst_.depsgraph, render_texture->camera);
    slot.extent = int2(max_ii(render_texture->resolution_x, 1),
                       max_ii(render_texture->resolution_y, 1));
    slot.active = true;

    data_[slot_index].info = int4(slot.uid, slot.extent.x, slot.extent.y, RENDER_TEXTURE_SLOT_VALID);
    slot_index++;
  }

  data_.push_update();
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
  data_[slot_index].info = int4(
      slot.uid, slot.extent.x, slot.extent.y, RENDER_TEXTURE_SLOT_VALID | RENDER_TEXTURE_SLOT_CAPTURING);
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

  prepass_fb_.ensure(GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
                     GPU_ATTACHMENT_TEXTURE(rbufs.vector_tx));
  combined_fb_.ensure(GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
                      GPU_ATTACHMENT_TEXTURE(rbufs.combined_tx));

  inst_.gbuffer.acquire(slot.extent,
                        inst_.pipelines.deferred.closure_layer_count(),
                        inst_.pipelines.deferred.normal_layer_count());
  gbuffer_fb_.ensure(GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
                     GPU_ATTACHMENT_TEXTURE(rbufs.combined_tx),
                     GPU_ATTACHMENT_TEXTURE(inst_.gbuffer.header_tx),
                     GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.normal_tx.layer_view(0), 0),
                     GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.closure_tx.layer_view(0), 0),
                     GPU_ATTACHMENT_TEXTURE_LAYER(inst_.gbuffer.closure_tx.layer_view(1), 0));

  const float4 clear_velocity(inst_.velocity.camera_has_motion() ? VELOCITY_INVALID : 0.0f);
  GPU_framebuffer_bind(prepass_fb_);
  GPU_framebuffer_clear_color(prepass_fb_, clear_velocity);

  const float4 clear_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
  GPU_framebuffer_bind(combined_fb_);
  GPU_framebuffer_clear_color_depth(combined_fb_, clear_color, 1.0f);
  inst_.pipelines.background.clear(render_view);

  inst_.lights.set_view(render_view, slot.extent);
  inst_.hiz_buffer.set_source(&inst_.render_buffers.depth_tx);

  inst_.volume.draw_prepass(main_view);
  inst_.pipelines.background.render(render_view, combined_fb_);
  inst_.pipelines.deferred.render(main_view,
                                  render_view,
                                  prepass_fb_,
                                  combined_fb_,
                                  gbuffer_fb_,
                                  slot.extent,
                                  rt_buffer_opaque_,
                                  rt_buffer_refract_);
  inst_.gbuffer.release();

  inst_.volume.draw_compute(main_view, slot.extent);
  inst_.ambient_occlusion.render_pass(render_view);
  inst_.pipelines.forward.render(render_view, prepass_fb_, combined_fb_, slot.extent);

  GPU_texture_copy(slot.color_tx.current(), rbufs.combined_tx);

  rbufs.release();

  data_[slot_index].info.w &= ~RENDER_TEXTURE_SLOT_CAPTURING;
  data_.push_update();
}

void RenderTextureModule::render()
{
  CameraData main_camera = inst_.camera.data_get();
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
