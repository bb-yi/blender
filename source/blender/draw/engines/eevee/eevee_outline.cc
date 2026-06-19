/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_base.h"

#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"

#include "GPU_capabilities.hh"
#include "GPU_material.hh"
#include "GPU_texture.hh"

#include "draw_cache.hh"

#include "eevee_instance.hh"
#include "eevee_outline.hh"

namespace blender::eevee {

/* Converts Outline Control's Line Width socket value to the geometry-space shell offset used by
 * the UMA-style duplicated inverted hull. This remains a calibration point until the legacy
 * geometry-node group scale is matched exactly. */
static constexpr float outline_shell_width_to_geometry_scale = 0.01f;

void OutlineModule::begin_sync()
{
  shell_ps_.init();
  shell_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_GREATER_EQUAL |
                      DRW_STATE_CULL_FRONT | DRW_STATE_BLEND_ALPHA_PREMUL |
                      DRW_STATE_CLIP_CONTROL_UNIT_RANGE);
  shell_ps_.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
  shell_ps_.bind_resources(inst_.uniform_data);
  shell_ps_.bind_resources(inst_.sampling);

  shell_boundary_ps_.init();
  shell_boundary_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA_PREMUL);
  shell_boundary_ps_.shader_set(inst_.shaders.static_shader_get(OUTLINE_SHELL_BOUNDARY));
  shell_boundary_ps_.bind_texture("shell_color_tx", &shell_color_tx_);
  shell_boundary_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  freestyle_edge_ps_.init();
  freestyle_edge_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_CLIP_CONTROL_UNIT_RANGE);
  freestyle_edge_ps_.shader_set(inst_.shaders.static_shader_get(OUTLINE_FREESTYLE));
  freestyle_edge_ps_.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
  freestyle_edge_ps_.bind_texture("outline_color_tx", &inst_.render_buffers.outline_color_tx);
  freestyle_edge_ps_.bind_texture("outline_info_tx", &inst_.render_buffers.outline_info_tx);
  freestyle_edge_ps_.bind_resources(inst_.uniform_data);
}

PassMain::Sub *OutlineModule::shell_add(blender::Material * /*blender_mat*/, GPUMaterial *gpumat)
{
  if (gpumat == nullptr) {
    return nullptr;
  }

  PassMain::Sub *pass = &shell_ps_.sub(GPU_material_get_name(gpumat));
  pass->material_set(*inst_.manager, gpumat, true);
  pass->push_constant("outline_shell_offset", outline_shell_width_to_geometry_scale);
  return pass;
}

void OutlineModule::sync_object(Object *ob, ResourceHandleRange res_handle)
{
  if (inst_.scene->eevee.use_outline == 0) {
    return;
  }
  if ((ob->base_flag & BASE_HOLDOUT) || (ob->visibility_flag & OB_HOLDOUT)) {
    return;
  }

  gpu::Batch *geom = DRW_cache_mesh_freestyle_edges_get(ob);
  if (geom != nullptr) {
    freestyle_edge_ps_.draw(geom, res_handle);
  }
}

void OutlineModule::sync()
{
  const bool previous_enabled = enabled_;
  const bool previous_use_in_combined = use_in_combined_;
  const bool scene_outline_enabled = inst_.scene->eevee.use_outline != 0;
  const bool has_outline_materials = scene_outline_enabled &&
                                     inst_.materials.has_visible_outline_materials();
  const bool public_pass_enabled =
      ((inst_.view_layer->eevee.render_passes & EEVEE_RENDER_PASS_OUTLINE) != 0) ||
      inst_.render_buffers.data.outline_id != -1;
  const bool use_in_combined = !public_pass_enabled;
  enabled_ = scene_outline_enabled && has_outline_materials &&
             (GPU_max_images() > OUTLINE_INFO_SLOT) && (use_in_combined || public_pass_enabled);
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
  detect_ps_.bind_texture("prepass_normal_tx", &inst_.render_buffers.prepass_normal_tx);
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
  resolve_ps_.bind_texture("vector_tx", &inst_.render_buffers.vector_tx);
  gpu::Texture **outline_occlusion_depth_ref =
      inst_.pipelines.forward.has_outline_occluders() ? &occlusion_depth_tx_ :
                                                        &inst_.render_buffers.depth_tx;
  resolve_ps_.bind_texture("outline_occlusion_depth_tx", outline_occlusion_depth_ref);
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

  if (inst_.pipelines.forward.has_outline_occluders()) {
    occlusion_depth_tx_.acquire(extent,
                                gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8,
                                GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
    GPU_texture_copy(occlusion_depth_tx_, inst_.render_buffers.depth_tx);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH |
                       GPU_BARRIER_FRAMEBUFFER);

    occlusion_fb_.ensure(GPU_ATTACHMENT_TEXTURE(occlusion_depth_tx_));
    inst_.pipelines.forward.render_outline_occlusion(view, occlusion_fb_);
    GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
  }
  else {
    occlusion_depth_tx_.release();
  }

  edge_seed_tx_.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, GPU_TEXTURE_USAGE_GENERAL);
  edge_seed_tx_.clear(float4(0.0f));

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  /* Detect pass: find edge pixels. */
  detect_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(edge_seed_tx_));
  detect_ps_.framebuffer_set(&detect_fb_);
  GPU_framebuffer_bind(detect_fb_);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  drw.submit(detect_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);

  /* Freestyle edge pass: render marked edges into the seed buffer. */
  freestyle_edge_ps_.framebuffer_set(&detect_fb_);
  GPU_framebuffer_bind(detect_fb_);
  drw.submit(freestyle_edge_ps_, view);
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
  resolved_depth_tx_.acquire(extent,
                             gpu::TextureFormat::SFLOAT_32,
                             GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
  resolved_velocity_tx_.acquire(extent,
                                inst_.render_buffers.vector_tx_format(),
                                GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
  const bool do_motion_vectors_swizzle = inst_.render_buffers.vector_tx_format() ==
                                         gpu::TextureFormat::SFLOAT_16_16;
  if (do_motion_vectors_swizzle) {
    GPU_texture_swizzle_set(resolved_velocity_tx_, "rgrg");
  }
  resolved_outline_tx_.clear(float4(0.0f));
  resolved_depth_tx_.clear(float4(0.0f));
  resolved_velocity_tx_.clear(float4(0.0f));
  shell_color_tx_.acquire(extent,
                          gpu::TextureFormat::SFLOAT_16_16_16_16,
                          GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
  shell_color_tx_.clear(float4(0.0f));
  resolve_fb_.ensure(GPU_ATTACHMENT_NONE,
                     GPU_ATTACHMENT_TEXTURE(resolved_outline_tx_),
                     GPU_ATTACHMENT_TEXTURE(resolved_depth_tx_),
                     GPU_ATTACHMENT_TEXTURE(resolved_velocity_tx_));
  resolve_ps_.framebuffer_set(&resolve_fb_);
  GPU_framebuffer_bind(resolve_fb_);
  drw.submit(resolve_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  shell_fb_.ensure(GPU_ATTACHMENT_TEXTURE(inst_.render_buffers.depth_tx),
                   GPU_ATTACHMENT_TEXTURE(shell_color_tx_));
  shell_ps_.framebuffer_set(&shell_fb_);
  GPU_framebuffer_bind(shell_fb_);
  drw.submit(shell_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  shell_boundary_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(resolved_outline_tx_));
  shell_boundary_ps_.framebuffer_set(&shell_boundary_fb_);
  GPU_framebuffer_bind(shell_boundary_fb_);
  drw.submit(shell_boundary_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  edge_seed_tx_.release();
  shell_color_tx_.release();
  jfa_tx_.current().release();
  jfa_tx_.previous().release();
  occlusion_depth_tx_.release();
}

void OutlineModule::release_result()
{
  const bool do_motion_vectors_swizzle = inst_.render_buffers.vector_tx_format() ==
                                         gpu::TextureFormat::SFLOAT_16_16;
  if (do_motion_vectors_swizzle && resolved_velocity_tx_.is_valid()) {
    GPU_texture_swizzle_set(resolved_velocity_tx_, "rgba");
  }
  resolved_outline_tx_.release();
  resolved_depth_tx_.release();
  resolved_velocity_tx_.release();
  shell_color_tx_.release();
  occlusion_depth_tx_.release();
}

}  // namespace blender::eevee
