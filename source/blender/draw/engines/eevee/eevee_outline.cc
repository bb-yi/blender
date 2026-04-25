/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_base.h"

#include "GPU_capabilities.hh"
#include "GPU_texture.hh"

#include "eevee_instance.hh"
#include "eevee_outline.hh"

namespace blender::eevee {

void OutlineModule::sync()
{
  const bool previous_enabled = enabled_;
  const bool previous_use_in_combined = use_in_combined_;
  const bool has_outline_materials = inst_.materials.has_visible_outline_materials();
  const bool public_pass_enabled =
      ((inst_.view_layer->eevee.render_passes & EEVEE_RENDER_PASS_OUTLINE) != 0) ||
      inst_.render_buffers.data.outline_id != -1;
  const bool use_in_combined = !public_pass_enabled;
  enabled_ = has_outline_materials && (GPU_max_images() > OUTLINE_INFO_SLOT) &&
             (use_in_combined || public_pass_enabled);
  use_in_combined_ = use_in_combined;

  if (inst_.is_viewport() &&
      (previous_enabled != enabled_ || previous_use_in_combined != use_in_combined_))
  {
    inst_.sampling.reset();
  }

  if (!enabled_) {
    release_result();
    return;
  }

  detect_ps_.init();
  detect_ps_.state_set(DRW_STATE_WRITE_COLOR);
  detect_ps_.shader_set(inst_.shaders.static_shader_get(OUTLINE_DETECT));
  detect_ps_.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
  detect_ps_.bind_texture("outline_color_tx", &inst_.render_buffers.outline_color_tx);
  detect_ps_.bind_texture("outline_info_tx", &inst_.render_buffers.outline_info_tx);
  detect_ps_.bind_resources(inst_.uniform_data);
  detect_ps_.bind_resources(inst_.gbuffer);
  detect_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  jfa_init_ps_.init();
  jfa_init_ps_.state_set(DRW_STATE_WRITE_COLOR);
  jfa_init_ps_.shader_set(inst_.shaders.static_shader_get(OUTLINE_JFA_INIT));
  jfa_init_ps_.bind_texture("outline_seed_tx", &edge_seed_tx_);
  jfa_init_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  jfa_step_ps_.init();
  jfa_step_ps_.shader_set(inst_.shaders.static_shader_get(OUTLINE_JFA_STEP));
  jfa_step_ps_.bind_image("jfa_in_img", &jfa_tx_.previous());
  jfa_step_ps_.bind_image("jfa_out_img", &jfa_tx_.current());
  jfa_step_ps_.push_constant("jfa_step_size", &jfa_step_size_, 1);
  jfa_step_ps_.dispatch(&jfa_dispatch_size_);
  jfa_step_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);

  resolve_ps_.init();
  resolve_ps_.state_set(DRW_STATE_WRITE_COLOR);
  resolve_ps_.shader_set(inst_.shaders.static_shader_get(OUTLINE_RESOLVE));
  resolve_ps_.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
  resolve_ps_.bind_texture("outline_seed_tx", &edge_seed_tx_);
  resolve_ps_.bind_texture("outline_color_tx", &inst_.render_buffers.outline_color_tx);
  resolve_ps_.bind_texture("outline_info_tx", &inst_.render_buffers.outline_info_tx);
  resolve_ps_.bind_texture("jfa_tx", &jfa_tx_.previous());
  resolve_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
}

void OutlineModule::render(View &view, int2 extent)
{
  if (!enabled_) {
    return;
  }

  auto &drw = *inst_.manager;

  edge_seed_tx_.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, GPU_TEXTURE_USAGE_GENERAL);
  edge_seed_tx_.clear(float4(0.0f));

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  /* Detect pass: find edge pixels. */
  detect_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(edge_seed_tx_));
  detect_ps_.framebuffer_set(&detect_fb_);
  GPU_framebuffer_bind(detect_fb_);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  drw.submit(detect_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  /* JFA init: seed the coordinate table from edge pixels. */
  jfa_tx_.current().acquire(extent, gpu::TextureFormat::SFLOAT_32_32, GPU_TEXTURE_USAGE_GENERAL);
  jfa_tx_.previous().acquire(extent, gpu::TextureFormat::SFLOAT_32_32, GPU_TEXTURE_USAGE_GENERAL);
  jfa_tx_.current().clear(float4(-1e10f));

  jfa_init_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(jfa_tx_.current()));
  jfa_init_ps_.framebuffer_set(&jfa_init_fb_);
  GPU_framebuffer_bind(jfa_init_fb_);
  drw.submit(jfa_init_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);

  /* JFA passes: flood coordinates outward. */
  const int group_size = OUTLINE_JFA_STEP_GROUP_SIZE;
  jfa_dispatch_size_ = int3(
      (extent.x + group_size - 1) / group_size, (extent.y + group_size - 1) / group_size, 1);

  const int max_dim = math::max(extent.x, extent.y);
  int step_size = power_of_2_max_i(max_dim) / 2;

  /* 1+JFA variant: initial step_size=1 pass for accuracy. */
  jfa_tx_.swap();
  jfa_tx_.current().clear(float4(-1e10f));
  jfa_step_size_ = 1;
  drw.submit(jfa_step_ps_);

  while (step_size >= 1) {
    jfa_tx_.swap();
    jfa_tx_.current().clear(float4(-1e10f));
    jfa_step_size_ = step_size;
    drw.submit(jfa_step_ps_);
    step_size /= 2;
  }

  /* Resolve pass: look up nearest seed and output the outline source result. */
  /* After the last swap+submit, the result is in jfa_tx_.current().
   * But resolve_ps_ reads from jfa_tx_.previous() (bound at sync time).
   * We need one more swap so previous() points to the final result. */
  jfa_tx_.swap();

  resolved_outline_tx_.acquire(extent,
                               gpu::TextureFormat::SFLOAT_16_16_16_16,
                               GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
  resolved_outline_tx_.clear(float4(0.0f));
  resolve_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(resolved_outline_tx_));
  resolve_ps_.framebuffer_set(&resolve_fb_);
  GPU_framebuffer_bind(resolve_fb_);
  drw.submit(resolve_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  edge_seed_tx_.release();
  jfa_tx_.current().release();
  jfa_tx_.previous().release();
}

void OutlineModule::release_result()
{
  resolved_outline_tx_.release();
}

}  // namespace blender::eevee
