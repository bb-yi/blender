/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_dlss5.hh"

#include "CLG_log.h"
#include "BKE_scene_runtime.hh"
#include "DEG_depsgraph_query.hh"
#include "BLI_string.h"

#include "GPU_context.hh"
#include "GPU_texture.hh"

#include "eevee_shader.hh"
#include "eevee_instance.hh"

namespace blender::eevee {

Dlss5Module::Dlss5Module(Instance &inst)
    : inst_(inst), d3d12_session_(std::make_unique<Dlss5D3D12Session>())
{
}

Dlss5Module::~Dlss5Module() = default;

bool Dlss5Module::prepare_display_color(gpu::Texture *source, gpu::Texture *destination)
{
  gpu::Shader *shader = inst_.shaders.static_shader_get(DLSS5_COLOR_CONVERT);
  if (shader == nullptr || source == nullptr || destination == nullptr) {
    return false;
  }

  display_color_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(destination));
  color_convert_ps_.init();
  color_convert_ps_.state_set(DRW_STATE_WRITE_COLOR);
  color_convert_ps_.framebuffer_set(&display_color_fb_);
  color_convert_ps_.shader_set(shader);
  color_convert_ps_.bind_texture("color_tx", &source);
  color_convert_ps_.push_constant("inverse", color_convert_inverse_);
  color_convert_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  inst_.manager->submit(color_convert_ps_);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_FRAMEBUFFER);
  return true;
}

bool Dlss5Module::reconstruct_scene_linear(gpu::Texture *source,
                                           gpu::Texture *input,
                                           gpu::Texture *original,
                                           gpu::Texture *destination,
                                           const float intensity)
{
  gpu::Shader *shader = inst_.shaders.static_shader_get(DLSS5_HDR_RECONSTRUCT);
  if (shader == nullptr || source == nullptr || input == nullptr || original == nullptr ||
      destination == nullptr)
  {
    return false;
  }

  hdr_reconstruct_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(destination));
  hdr_reconstruct_ps_.init();
  hdr_reconstruct_ps_.state_set(DRW_STATE_WRITE_COLOR);
  hdr_reconstruct_ps_.framebuffer_set(&hdr_reconstruct_fb_);
  hdr_reconstruct_ps_.shader_set(shader);
  hdr_reconstruct_ps_.bind_texture("color_tx", &source);
  hdr_reconstruct_ps_.bind_texture("source_tx", &input);
  hdr_reconstruct_ps_.bind_texture("original_tx", &original);
  hdr_reconstruct_ps_.push_constant("resolve_intensity", intensity);
  hdr_reconstruct_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  inst_.manager->submit(hdr_reconstruct_ps_);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_FRAMEBUFFER);
  return true;
}

bool Dlss5Module::prepare_velocity(const Dlss5FrameInputs &inputs,
                                   gpu::Texture *destination, draw::View &view)
{
  gpu::Texture *source = inputs.velocity;
  gpu::Texture *depth = inputs.depth;
  gpu::Shader *shader = inst_.shaders.static_shader_get(DLSS5_VELOCITY_CONVERT);
  if (shader == nullptr || source == nullptr || destination == nullptr) {
    return false;
  }

  velocity_convert_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(destination));
  velocity_convert_ps_.init();
  velocity_convert_ps_.state_set(DRW_STATE_WRITE_COLOR);
  velocity_convert_ps_.framebuffer_set(&velocity_convert_fb_);
  velocity_convert_ps_.shader_set(shader);
  const GPUSamplerState no_filter = GPUSamplerState::default_sampler();
  velocity_convert_ps_.bind_texture("velocity_tx", &source, no_filter);
  velocity_convert_ps_.bind_texture("depth_tx", &depth, no_filter);
  inst_.velocity.bind_resources(velocity_convert_ps_);
  velocity_convert_ps_.push_constant("guide_overscan", inputs.guide_overscan);
  velocity_convert_ps_.push_constant("guide_scale", inputs.guide_scale);
  velocity_convert_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  inst_.manager->submit(velocity_convert_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_FRAMEBUFFER);
  return true;
}

void Dlss5Module::publish_status(const bool viewport, const char *status)
{
  Scene *scene = DEG_get_original(inst_.scene);
  if (scene != nullptr && scene->runtime != nullptr) {
    scene->runtime->eevee_performance.dlss5_status_publish(viewport, status);
  }
}

gpu::Texture *Dlss5Module::process(const Dlss5FrameInputs &inputs, draw::View &view)
{
  last_display_texture_ = nullptr;
  if (inst_.scene == nullptr || inst_.scene->eevee.dlss5_mode != SCE_EEVEE_DLSSNR ||
      inst_.scene->eevee.dlss5_intensity == 0.0f)
  {
    retry_blocked_ = false;
    force_history_reset_ = true;
    return inputs.color;
  }

  if (!reported_) {
    CLOG_INFO(&Instance::log,
              "DLSS5 adapter: backend=%s input=%dx%d output=%dx%d viewport=%d",
              GPU_backend_get_name(),
              inputs.input_extent.x,
              inputs.input_extent.y,
              inputs.output_extent.x,
              inputs.output_extent.y,
              inputs.is_viewport);
    CLOG_INFO(&Instance::log,
              "DLSS5 contract: color=%s depth=%s velocity=%s color_scene_linear=%d "
              "depth_reverse_z=%d velocity_packed=%d velocity_pixel_space=%d exposure_scale=%f",
              GPU_texture_format_name(GPU_texture_format(inputs.color)),
              GPU_texture_format_name(GPU_texture_format(inputs.depth)),
              GPU_texture_format_name(GPU_texture_format(inputs.velocity)),
              inputs.color_is_scene_linear,
              inputs.depth_is_reverse_z,
              inputs.velocity_is_packed,
              inputs.velocity_is_pixel_space,
              inputs.exposure_scale);
    reported_ = true;
  }

  if (GPU_backend_get_type() != GPU_BACKEND_VULKAN) {
    publish_status(inputs.is_viewport, "Unavailable: Vulkan backend required");
    static bool logged_backend_skip = false;
    if (!logged_backend_skip) {
      logged_backend_skip = true;
      CLOG_WARN(&Instance::log,
                "DLSS5 skipped: backend=%s (need Vulkan and restart)",
                GPU_backend_get_name());
    }
    return inputs.color;
  }
  if (inst_.scene == nullptr || inst_.scene->eevee.dlss5_mode != SCE_EEVEE_DLSSNR) {
    retry_blocked_ = false;
    active_reported_ = false;
    failure_reported_ = false;
    return inputs.color;
  }
  if (inputs.color == nullptr || inputs.base_color == nullptr || inputs.depth == nullptr ||
      inputs.velocity == nullptr ||
      inputs.input_extent.x < 32 || inputs.input_extent.y < 32 ||
      inputs.output_extent.x < 32 || inputs.output_extent.y < 32 ||
      inputs.input_extent != inputs.output_extent)
  {
    return inputs.color;
  }
  const int2 guide_extent = inputs.guide_extent.x > 0 && inputs.guide_extent.y > 0 ?
                                inputs.guide_extent :
                                int2(GPU_texture_width(inputs.depth), GPU_texture_height(inputs.depth));
  if (guide_extent.x < 32 || guide_extent.y < 32) {
    return inputs.color;
  }
  const Dlss5NRSettings settings = {
      inst_.scene->eevee.dlss5_intensity,
      inst_.scene->eevee.dlss5_local_tone_strength,
      inst_.scene->eevee.dlss5_local_structure_strength,
      inst_.scene->eevee.dlss5_skin_structure_strength,
      inst_.scene->eevee.dlss5_use_auto_mask != 0,
      inst_.scene->eevee.dlss5_ui_correction != 0,
      int(inst_.scene->eevee.dlss5_style),
  };

  if (inst_.dlss5_settings_changed() ||
      (retry_blocked_ &&
       (retry_input_extent_ != inputs.input_extent || retry_output_extent_ != inputs.output_extent)))
  {
    retry_blocked_ = false;
    failure_reported_ = false;
  }
  if (retry_blocked_) {
    return inputs.color;
  }

  if (!d3d12_session_->ensure_resources(inputs.input_extent,
                                        inputs.output_extent,
                                        inputs.output_extent,
                                        settings,
                                        false,
                                        inputs.depth_is_reverse_z))
  {
    retry_blocked_ = true;
    retry_input_extent_ = inputs.input_extent;
    retry_output_extent_ = inputs.output_extent;
    publish_status(inputs.is_viewport, d3d12_session_->status());
    if (!failure_reported_) {
      CLOG_WARN(&Instance::log, "DLSS5 disabled until settings change: %s",
                d3d12_session_->status());
      failure_reported_ = true;
    }
    return inputs.color;
  }

  scene_linear_output_tx_.ensure_2d(
      gpu::TextureFormat::SFLOAT_16_16_16_16,
      inputs.output_extent,
      GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT);
  if (scene_linear_output_tx_.gpu_texture() == nullptr) {
    CLOG_WARN(&Instance::log, "DLSS5 scene-linear output texture is unavailable");
    return inputs.color;
  }

  if (inputs.color_is_scene_linear) {
    color_convert_inverse_ = false;
    if (!prepare_display_color(inputs.color, d3d12_session_->color_texture()))
    {
      CLOG_WARN(&Instance::log, "DLSS5 scene-linear to display color conversion is unavailable");
      return inputs.color;
    }
  }

  gpu::Shader *depth_shader = inst_.shaders.static_shader_get(DLSS5_DEPTH_CONVERT);
  if (depth_shader == nullptr) {
    publish_status(inputs.is_viewport, "Unavailable: depth conversion shader failed");
    CLOG_WARN(&Instance::log, "DLSS5 depth conversion shader is unavailable");
    return inputs.color;
  }

  gpu::Texture *depth_input = inputs.depth;
  depth_convert_fb_.ensure(GPU_ATTACHMENT_NONE,
                           GPU_ATTACHMENT_TEXTURE(d3d12_session_->depth_texture()));
  depth_convert_ps_.init();
  depth_convert_ps_.state_set(DRW_STATE_WRITE_COLOR);
  depth_convert_ps_.framebuffer_set(&depth_convert_fb_);
  depth_convert_ps_.shader_set(depth_shader);
  depth_convert_ps_.bind_texture("depth_tx", &depth_input);
  depth_convert_ps_.push_constant("guide_overscan", inputs.guide_overscan);
  depth_convert_ps_.push_constant("guide_scale", inputs.guide_scale);
  depth_convert_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  inst_.manager->submit(depth_convert_ps_);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_FRAMEBUFFER);

  if (!prepare_velocity(inputs, d3d12_session_->velocity_texture(), view))
  {
    publish_status(inputs.is_viewport, "Unavailable: motion conversion shader failed");
    CLOG_WARN(&Instance::log, "DLSS5 velocity conversion is unavailable");
    return inputs.color;
  }

  const Dlss5D3D12Frame frame = {
      d3d12_session_->color_texture(),
      d3d12_session_->depth_texture(),
      d3d12_session_->velocity_texture(),
      inputs.input_extent,
      inputs.output_extent,
      inputs.output_extent,
      inputs.jitter,
      inputs.reset_history || force_history_reset_,
      false,
      inputs.depth_is_reverse_z,
      true,
      inputs.exposure_scale,
      settings,
  };
  if (!d3d12_session_->copy_inputs_and_evaluate(
          frame, !inputs.color_is_scene_linear, false))
  {
    publish_status(inputs.is_viewport, d3d12_session_->status());
    if (!failure_reported_) {
      CLOG_WARN(&Instance::log, "DLSS5 evaluate skipped: %s", d3d12_session_->status());
      failure_reported_ = true;
    }
    /* Never FreeLibrary here. OptiScaler keeps NGX loaded; unloading 165MB per
     * failed viewport sample is what made enable feel like a hang. */
    return inputs.color;
  }

  if (!active_reported_) {
    CLOG_INFO(&Instance::log,
              "DLSS5 active: target=%s input=%dx%d output=%dx%d",
              inputs.is_viewport ? "viewport" : "render",
              inputs.input_extent.x,
              inputs.input_extent.y,
              inputs.output_extent.x,
              inputs.output_extent.y);
    active_reported_ = true;
  }
  retry_blocked_ = false;
  failure_reported_ = false;
  /* Order the shared output on the Vulkan queue without a host fence wait. */
  if (!d3d12_session_->wait_for_output()) {
    return inputs.color;
  }
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  if (!reconstruct_scene_linear(d3d12_session_->output_texture(),
                                d3d12_session_->color_texture(),
                                inputs.base_color,
                                scene_linear_output_tx_.gpu_texture(),
                                1.0f))
  {
    CLOG_WARN(&Instance::log, "DLSS5 HDR-preserving output reconstruction is unavailable");
    return inputs.color;
  }
  last_display_texture_ = scene_linear_output_tx_.gpu_texture();
  force_history_reset_ = false;
  const double gpu_ms = d3d12_session_->gpu_time_ms();
  char status[160];
  if (gpu_ms >= 0.0) {
    SNPRINTF(status, "Active %dx%d | recent NR GPU %.2f ms", inputs.output_extent.x,
             inputs.output_extent.y, gpu_ms);
  }
  else {
    SNPRINTF(status, "Active %dx%d | GPU timing pending", inputs.output_extent.x,
             inputs.output_extent.y);
  }
  publish_status(inputs.is_viewport, status);
  return last_display_texture_;
}

void Dlss5Module::warmup()
{
  d3d12_session_->warmup();
}

void Dlss5Module::render_readback_complete()
{
  if (display_texture() == nullptr) {
    return;
  }
  char status[160];
  const double gpu_ms = d3d12_session_->gpu_time_ms();
  if (gpu_ms >= 0.0) {
    SNPRINTF(status, "Completed | NR GPU %.2f ms", gpu_ms);
  }
  else {
    SNPRINTF(status, "%s", "Completed | GPU timing unavailable");
  }
  publish_status(false, status);
}

gpu::Texture *Dlss5Module::display_texture() const
{
  if (inst_.scene == nullptr || inst_.scene->eevee.dlss5_mode != SCE_EEVEE_DLSSNR ||
      inst_.scene->eevee.dlss5_intensity == 0.0f)
  {
    return nullptr;
  }
  return last_display_texture_;
}

bool Dlss5Module::available() const
{
  return d3d12_session_->available();
}

const char *Dlss5Module::status() const
{
  return d3d12_session_->status();
}

}  // namespace blender::eevee
