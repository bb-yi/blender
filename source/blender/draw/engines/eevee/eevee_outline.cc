/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GPU_capabilities.hh"

#include "eevee_instance.hh"
#include "eevee_outline.hh"

namespace blender::eevee {

void OutlineModule::sync()
{
  const bool has_outline_materials = inst_.materials.has_visible_outline_materials();
  enabled_ = has_outline_materials && GPU_max_images() > OUTLINE_INFO_SLOT;

  if (!enabled_) {
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

  expand_ps_.init();
  expand_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA_PREMUL);
  expand_ps_.shader_set(inst_.shaders.static_shader_get(OUTLINE_EXPAND));
  expand_ps_.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
  expand_ps_.bind_texture("outline_seed_tx", &edge_seed_tx_);
  expand_ps_.bind_texture("outline_color_tx", &inst_.render_buffers.outline_color_tx);
  expand_ps_.bind_texture("outline_info_tx", &inst_.render_buffers.outline_info_tx);
  expand_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
}

void OutlineModule::render(View &view, Framebuffer &combined_fb, int2 extent)
{
  if (!enabled_) {
    return;
  }

  edge_seed_tx_.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, GPU_TEXTURE_USAGE_GENERAL);
  edge_seed_tx_.clear(float4(0.0f));

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  framebuffer_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(edge_seed_tx_));
  detect_ps_.framebuffer_set(&framebuffer_);
  GPU_framebuffer_bind(framebuffer_);
  inst_.manager->submit(detect_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  expand_ps_.framebuffer_set(&combined_fb);
  GPU_framebuffer_bind(combined_fb);
  inst_.manager->submit(expand_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  edge_seed_tx_.release();
}

}  // namespace blender::eevee
