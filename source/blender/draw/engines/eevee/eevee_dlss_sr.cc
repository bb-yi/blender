/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_dlss_sr.hh"
#include <cstdlib>

#include "BKE_scene_runtime.hh"
#include "BLI_string.h"
#include "DEG_depsgraph_query.hh"
#include "DNA_camera_types.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"
#include "GPU_context.hh"
#include "GPU_texture.hh"

#include "eevee_instance.hh"

namespace blender::eevee {

const char *DlssSrModule::fallback_reason(const Scene &scene,
                                          const ViewLayer &layer,
                                          const View3D *v3d)
{
  if (scene.r.alphamode == R_ALPHAPREMUL) {
    return "Native: transparent background";
  }
  if (scene.r.scemode & R_MULTIVIEW) {
    return "Native: multiview/stereo";
  }
  for (const Base &base : layer.object_bases) {
    if ((base.flag & BASE_HOLDOUT) || (base.object && (base.object->visibility_flag & OB_HOLDOUT)))
    {
      return "Native: holdout alpha";
    }
  }
  if (v3d && v3d->shading.render_pass != EEVEE_RENDER_PASS_COMBINED) {
    return "Native: non-Combined preview";
  }
  if (scene.eevee.dlss5_mask_aov[0] && scene.eevee.dlss5_mask_aov_output) {
    return "Native: exact mask AOV output";
  }
  /* Film reconstructs auxiliary passes independently of DLSS color. Their presence,
   * compositor use, or EXR container does not require a native-resolution render. */
  return nullptr;
}

void DlssSrModule::publish_status(const char *status)
{
  Scene *scene = DEG_get_original(inst_.scene);
  if (scene && scene->runtime) {
    scene->runtime->eevee_performance.dlss_sr_status_publish(inst_.is_viewport(), status);
  }
}

void DlssSrModule::init(const int2 output_extent)
{
  const bool was_active = active_;
  active_ = false;
  const Scene &scene = *inst_.scene;
  const int quality = inst_.is_viewport() ? scene.eevee.dlss_sr_viewport_quality :
                                            scene.eevee.dlss_sr_render_quality;
  const float percentage = quality == SCE_EEVEE_DLSS_SR_CUSTOM ?
                               (inst_.is_viewport() ? scene.eevee.dlss_sr_viewport_percentage :
                                                      scene.eevee.dlss_sr_render_percentage) :
                               0.0f;
  const bool settings_changed = quality_ != quality || percentage_ != percentage ||
                                output_extent_ != output_extent;
  const bool query_settings = !was_active || settings_changed;
  if (settings_changed) {
    failed_ = false;
    reset_ = true;
  }
  quality_ = quality;
  percentage_ = percentage;
  output_extent_ = output_extent;
  if (!quality) {
    failed_ = false;
    reset_ = true;
    publish_status("Disabled");
    return;
  }
  if (failed_) {
    return;
  }
  if (GPU_backend_get_type() != GPU_BACKEND_VULKAN) {
    publish_status("Native: SR requires Windows Vulkan");
    return;
  }
  if (const char *reason = fallback_reason(scene, *inst_.view_layer, inst_.v3d)) {
    publish_status(reason);
    return;
  }
  /* EEVEE currently maps some unsupported panoramic camera types to perspective
   * internally. Preserve the user's camera-mode restriction as well. */
  const Object *camera = inst_.camera_eval_object;
  if (inst_.camera.is_panoramic() ||
      (camera && camera->type == OB_CAMERA &&
       reinterpret_cast<const blender::Camera *>(camera->data)->type == CAM_PANO))
  {
    publish_status("Native: panoramic projection");
    return;
  }
  if (!inst_.is_viewport()) {
    session_.reuse_for_offline_render();
  }
  if ((query_settings &&
       !session_.sr_optimal_settings(output_extent, quality, percentage, input_extent_)) ||
      !session_.ensure_resources(input_extent_, output_extent, input_extent_, {}, true, true))
  {
    publish_status(session_.status());
    failed_ = true;
    return;
  }
  active_ = true;
  reset_ |= !was_active;
}

gpu::Texture *DlssSrModule::process(gpu::Texture *color, gpu::Texture *outline, draw::View &view)
{
  if (!active_) {
    return nullptr;
  }
  const char *failure = std::getenv("BLENDER_DLSS_SR_TEST_FAILURE");
  if (failure && STREQ(failure, "after_first_sample") && evaluated_samples_ == 1) {
    failed_ = true;
    publish_status("Native retry: SR evaluation failed after first sample (test injection)");
    return nullptr;
  }
  const int overscan = inst_.film.render_overscan_get();
  convert_fb_.ensure(GPU_ATTACHMENT_NONE,
                     GPU_ATTACHMENT_TEXTURE(session_.color_texture()),
                     GPU_ATTACHMENT_TEXTURE(session_.depth_texture()),
                     GPU_ATTACHMENT_TEXTURE(session_.velocity_texture()));
  convert_ps_.init();
  convert_ps_.state_set(DRW_STATE_WRITE_COLOR);
  convert_ps_.framebuffer_set(&convert_fb_);
  convert_ps_.shader_set(inst_.shaders.static_shader_get(DLSS_SR_PREPARE));
  convert_ps_.bind_texture("color_tx", color);
  convert_ps_.bind_texture("outline_tx", outline ? outline : color);
  convert_ps_.push_constant("has_outline", outline != nullptr);
  convert_ps_.push_constant("guide_overscan", overscan);
  convert_ps_.bind_texture("depth_tx", inst_.render_buffers.depth_tx);
  convert_ps_.bind_texture("velocity_tx", inst_.render_buffers.vector_tx);
  inst_.velocity.bind_resources(convert_ps_);
  convert_ps_.push_constant("use_motion", inst_.is_viewport());
  convert_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  inst_.manager->submit(convert_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_FRAMEBUFFER);

  Dlss5D3D12Frame frame;
  frame.color = session_.color_texture();
  frame.depth = session_.depth_texture();
  frame.velocity = session_.velocity_texture();
  frame.input_extent = input_extent_;
  frame.output_extent = output_extent_;
  frame.guide_extent = input_extent_;
  /* NGX expects the applied projection offset in input-pixel coordinates. The shared
   * textures retain EEVEE's coordinate orientation, including Y; do not negate the jitter. */
  frame.jitter = inst_.film.pixel_jitter_get();
  /* MotionBlurModule invalidates history only when shutter time changes. Lens jitter still
   * changes the view each sample and cannot use the offline zero-motion history. */
  frame.reset_history = reset_ || inst_.dlss5_reset() ||
                        (!inst_.is_viewport() && inst_.depth_of_field.jitter_enabled());
  frame.exposure_scale = 1.0f;
  if (!session_.copy_inputs_and_evaluate(frame, false, false) || !session_.wait_for_output()) {
    failed_ = true;
    reset_ = true;
    publish_status(session_.status());
    return nullptr;
  }
  reset_ = false;
  evaluated_samples_++;
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  char status[200];
  SNPRINTF(status,
           "Active %dx%d -> %dx%d | %d samples | SR GPU %.2f ms",
           input_extent_.x,
           input_extent_.y,
           output_extent_.x,
           output_extent_.y,
           evaluated_samples_,
           session_.gpu_time_ms());
  publish_status(status);
  return session_.output_texture();
}

void DlssSrModule::readback_complete()
{
  if (active_ && !failed_) {
    char status[200];
    SNPRINTF(status,
             "Completed %dx%d -> %dx%d | %d samples | SR GPU %.2f ms",
             input_extent_.x,
             input_extent_.y,
             output_extent_.x,
             output_extent_.y,
             evaluated_samples_,
             session_.gpu_time_ms());
    publish_status(status);
  }
}

}  // namespace blender::eevee
