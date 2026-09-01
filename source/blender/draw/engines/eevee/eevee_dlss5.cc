/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_dlss5.hh"

#include "CLG_log.h"

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
                                           gpu::Texture *destination)
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
  hdr_reconstruct_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  inst_.manager->submit(hdr_reconstruct_ps_);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_FRAMEBUFFER);
  return true;
}

bool Dlss5Module::prepare_velocity(gpu::Texture *source,
                                   gpu::Texture *destination,
                                   const int2 extent)
{
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
  velocity_convert_ps_.push_constant("scale", float2(extent));
  velocity_convert_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  inst_.manager->submit(velocity_convert_ps_);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_FRAMEBUFFER);
  return true;
}

gpu::Texture *Dlss5Module::process(const Dlss5FrameInputs &inputs)
{
  last_display_texture_ = nullptr;

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
    return inputs.color;
  }
  if (inst_.scene == nullptr || inst_.scene->eevee.dlss5_mode != SCE_EEVEE_DLSSNR) {
    if (d3d12_session_->available()) {
      d3d12_session_->reset();
    }
    retry_blocked_ = false;
    active_reported_ = false;
    failure_reported_ = false;
    return inputs.color;
  }
  if (inputs.color == nullptr || inputs.base_color == nullptr || inputs.depth == nullptr ||
      inputs.velocity == nullptr ||
      inputs.input_extent != inputs.output_extent)
  {
    return inputs.color;
  }

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

  if (!d3d12_session_->ensure_resources(inputs.input_extent, inputs.output_extent)) {
    retry_blocked_ = true;
    retry_input_extent_ = inputs.input_extent;
    retry_output_extent_ = inputs.output_extent;
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
    if (!prepare_display_color(inputs.color, d3d12_session_->color_texture())) {
      CLOG_WARN(&Instance::log, "DLSS5 scene-linear to display color conversion is unavailable");
      return inputs.color;
    }
  }

  gpu::Shader *depth_shader = inst_.shaders.static_shader_get(DLSS5_DEPTH_CONVERT);
  if (depth_shader == nullptr) {
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
  depth_convert_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  inst_.manager->submit(depth_convert_ps_);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_FRAMEBUFFER);

  if (!prepare_velocity(inputs.velocity, d3d12_session_->velocity_texture(), inputs.input_extent)) {
    CLOG_WARN(&Instance::log, "DLSS5 velocity conversion is unavailable");
    return inputs.color;
  }

  const Dlss5D3D12Frame frame = {
      d3d12_session_->color_texture(),
      d3d12_session_->depth_texture(),
      d3d12_session_->velocity_texture(),
      inputs.input_extent,
      inputs.output_extent,
      inputs.jitter,
      inputs.reset_history,
      false,
      inputs.depth_is_reverse_z,
      true,
      inputs.exposure_scale,
      {
          inst_.scene->eevee.dlss5_intensity,
          inst_.scene->eevee.dlss5_local_tone_strength,
          inst_.scene->eevee.dlss5_local_structure_strength,
          inst_.scene->eevee.dlss5_skin_structure_strength,
          inst_.scene->eevee.dlss5_use_auto_mask != 0,
          inst_.scene->eevee.dlss5_ui_correction != 0,
      },
  };
  if (!d3d12_session_->copy_inputs_and_evaluate(
          frame, !inputs.color_is_scene_linear, false))
  {
    retry_blocked_ = true;
    retry_input_extent_ = inputs.input_extent;
    retry_output_extent_ = inputs.output_extent;
    if (!failure_reported_) {
      CLOG_WARN(&Instance::log, "DLSS5 disabled until settings change: %s",
                d3d12_session_->status());
      failure_reported_ = true;
    }
    d3d12_session_->reset();
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
  if (!d3d12_session_->wait_for_output()) {
    CLOG_WARN(&Instance::log, "DLSS5 Vulkan output wait failed");
    return inputs.color;
  }
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  if (!reconstruct_scene_linear(d3d12_session_->output_texture(),
                                d3d12_session_->color_texture(),
                                inputs.base_color,
                                scene_linear_output_tx_.gpu_texture()))
  {
    CLOG_WARN(&Instance::log, "DLSS5 HDR-preserving output reconstruction is unavailable");
    return inputs.color;
  }
  last_display_texture_ = scene_linear_output_tx_.gpu_texture();
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
