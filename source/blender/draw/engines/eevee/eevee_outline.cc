/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_base.h"

#include "DNA_layer_types.h"
#include "DNA_object_types.h"

#include "GPU_capabilities.hh"
#include "GPU_debug.hh"
#include "GPU_material.hh"
#include "GPU_texture.hh"

#include "draw_cache.hh"

#include "eevee_instance.hh"
#include "eevee_outline.hh"

namespace blender::eevee {

void OutlineModule::begin_sync()
{
  freestyle_edge_ps_.init();
  freestyle_edge_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_CLIP_CONTROL_UNIT_RANGE);
  freestyle_edge_ps_.bind_texture(OUTLINE_DEPTH_TEX_SLOT, &inst_.render_buffers.depth_tx);
  freestyle_edge_ps_.bind_texture(OUTLINE_COLOR_TEX_SLOT, &inst_.render_buffers.outline_color_tx);
  freestyle_edge_ps_.bind_texture(OUTLINE_INFO_TEX_SLOT, &inst_.render_buffers.outline_info_tx);
  freestyle_edge_ps_.bind_resources(inst_.uniform_data);
  freestyle_edge_ps_.bind_resources(inst_.sampling);
  freestyle_edge_ps_.bind_resources(inst_.render_textures);
  freestyle_edge_ps_.bind_resources(inst_.lights);
}

PassMain::Sub *OutlineModule::material_add(GPUMaterial *gpumat)
{
  if (GPU_material_get_shader(gpumat) == nullptr) {
    return nullptr;
  }
  PassMain::Sub &pass = freestyle_edge_ps_.sub(GPU_material_get_name(gpumat));
  pass.material_set(*inst_.manager, gpumat, true, inst_.anisotropic_filtering);
  pass.push_constant("outline_extent", &outline_extent_);
  return &pass;
}

void OutlineModule::sync_object(Object *ob,
                                ResourceHandleRange res_handle,
                                PassMain::Sub *pass,
                                const int material)
{
  if (inst_.scene->eevee.use_outline == 0 || pass == nullptr) {
    return;
  }
  if ((ob->base_flag & BASE_HOLDOUT) || (ob->visibility_flag & OB_HOLDOUT)) {
    return;
  }

  gpu::Batch *geom = DRW_cache_mesh_freestyle_edges_get(ob, material);
  if (geom != nullptr) {
    pass->draw(geom, res_handle);
  }
}

void OutlineModule::sync()
{
  const bool previous_enabled = enabled_;
  const bool previous_use_in_combined = use_in_combined_;
  const float previous_resolution_scale = resolution_scale_;
  const int previous_surface_identity = surface_identity_;
  surface_identity_ = inst_.scene->eevee.outline_surface_identity;
  const int resolution = inst_.scene->eevee.outline_viewport_resolution;
  resolution_scale_ = inst_.is_viewport() && resolution >= 1 && resolution <= 3 ?
                          float(4 - resolution) * 0.25f :
                          1.0f;
  const bool reduced = resolution_scale_ != 1.0f;
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
      (previous_enabled != enabled_ || previous_use_in_combined != use_in_combined_ ||
       previous_resolution_scale != resolution_scale_ ||
       previous_surface_identity != surface_identity_))
  {
    inst_.sampling.reset();
  }

  if (!enabled_) {
    release_result();
    return;
  }

  detect_ps_.init();
  detect_ps_.state_set(DRW_STATE_WRITE_COLOR);
  detect_ps_.shader_set(
      inst_.shaders.static_shader_get(reduced ? OUTLINE_DETECT_SCALED : OUTLINE_DETECT));
  if (reduced) {
    detect_ps_.push_constant("outline_extent", &outline_extent_);
  }
  detect_ps_.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
  detect_ps_.bind_texture("prepass_normal_tx", &inst_.render_buffers.prepass_normal_tx);
  detect_ps_.bind_texture("outline_color_tx", &inst_.render_buffers.outline_color_tx);
  detect_ps_.bind_texture("outline_info_tx", &inst_.render_buffers.outline_info_tx);
  detect_ps_.bind_resources(inst_.uniform_data);
  detect_ps_.bind_resources(inst_.gbuffer);
  detect_ps_.draw_procedural_indirect(GPU_PRIM_TRIS, original_detect_draw_buf_);
  /* Both programs use the exact same resource interface. Keep its bindings across the switch. */
  detect_ps_.shader_set(inst_.shaders.static_shader_get(reduced ? OUTLINE_DETECT_SCALED_CACHED :
                                                                  OUTLINE_DETECT_CACHED));
  if (reduced) {
    /* OpenGL uniforms belong to the selected program, unlike the shared texture bindings. */
    detect_ps_.push_constant("outline_extent", &outline_extent_);
  }
  detect_ps_.draw_procedural_indirect(GPU_PRIM_TRIS, cached_detect_draw_buf_);

  if (!packed_jfa_support_checked_) {
    /* Probe both storage/sampling usage and integer color-attachment support once per instance. */
    Texture probe = {"Outline.PackedFormatProbe"};
    probe.ensure_2d(gpu::TextureFormat::UINT_32_32, int2(1), GPU_TEXTURE_USAGE_GENERAL);
    if (probe.is_valid()) {
      Framebuffer probe_fb = {"Outline.PackedFormatProbe.FB"};
      probe_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(probe));
      char error[256];
      packed_jfa_supported_ = GPU_framebuffer_check_valid(probe_fb, error);
    }
    packed_jfa_support_checked_ = true;
  }

  for (const int source : IndexRange(2)) {
    PassSimple &classify = source ? source_classify_ps_ : classify_ps_;
    classify.init();
    classify.shader_set(inst_.shaders.static_shader_get(OUTLINE_CLASSIFY));
    classify.bind_texture("outline_seed_tx", &edge_seed_tx_.current());
    classify.bind_texture("outline_info_tx", &inst_.render_buffers.outline_info_tx);
    classify.bind_ssbo("outline_flags", &classification_buf_);
    classify.push_constant("classify_source", source);
    classify.dispatch(source ? &source_dispatch_size_ : &jfa_dispatch_size_);
    classify.barrier(GPU_BARRIER_SHADER_STORAGE);

    PassSimple &dispatch = source ? source_dispatch_ps_ : dispatch_ps_;
    dispatch.init();
    dispatch.shader_set(inst_.shaders.static_shader_get(OUTLINE_DISPATCH));
    dispatch.bind_ssbo("outline_flags", &classification_buf_);
    dispatch.bind_ssbo("factor_blur_draw_buf", &factor_blur_draw_buf_);
    dispatch.bind_ssbo("jfa_init_draw_buf", &jfa_init_draw_buf_);
    dispatch.bind_ssbo("jfa_dispatch_buf", &jfa_dispatch_buf_);
    dispatch.bind_ssbo("original_detect_draw_buf", &original_detect_draw_buf_);
    dispatch.bind_ssbo("cached_detect_draw_buf", &cached_detect_draw_buf_);
    dispatch.push_constant("classify_source", source);
    dispatch.push_constant("jfa_dispatch_size", &jfa_dispatch_size_);
    dispatch.dispatch(1);
    dispatch.barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_COMMAND);
  }

  /* Factor blur: smooth the per-seed width-variation factor along the contour (8-neighbour seed
   * connectivity), removing the per-Voronoi-cell drawn-radius sawtooth. Ping-pongs the seed buffer;
   * reads previous(), writes current(). Only .r is blurred, .a (full width) is passed through. */
  factor_blur_ps_.init();
  factor_blur_ps_.state_set(DRW_STATE_WRITE_COLOR);
  factor_blur_ps_.shader_set(
      inst_.shaders.static_shader_get(reduced ? OUTLINE_FACTOR_BLUR_SCALED : OUTLINE_FACTOR_BLUR));
  if (reduced) {
    factor_blur_ps_.push_constant("outline_extent", &outline_extent_);
  }
  factor_blur_ps_.bind_texture("outline_seed_tx", &edge_seed_tx_.previous());
  factor_blur_ps_.bind_texture("outline_info_tx", &inst_.render_buffers.outline_info_tx);
  factor_blur_ps_.draw_procedural_indirect(GPU_PRIM_TRIS, factor_blur_draw_buf_);

  for (const int variant : IndexRange(2)) {
    const bool packed = variant != 0;
    PassSimple &init = jfa_init_ps_[variant];
    init.init();
    init.state_set(DRW_STATE_WRITE_COLOR);
    init.shader_set(
        inst_.shaders.static_shader_get(packed ? OUTLINE_JFA_INIT_PACKED : OUTLINE_JFA_INIT));
    init.bind_texture("outline_seed_tx", &edge_seed_tx_.current());
    init.draw_procedural_indirect(GPU_PRIM_TRIS, jfa_init_draw_buf_);

    const int visible = surface_identity_ == 1 ? 1 : 0;
    {
      PassSimple &step = jfa_step_ps_[variant + visible * 2];
      step.init();
      const eShaderType shader = visible ? (packed ? OUTLINE_JFA_STEP_VISIBLE_PACKED :
                                                     OUTLINE_JFA_STEP_VISIBLE) :
                                           (packed ? OUTLINE_JFA_STEP_PACKED : OUTLINE_JFA_STEP);
      step.shader_set(inst_.shaders.static_shader_get(shader));
      step.bind_image("jfa_in_img", &jfa_tx_.previous());
      step.bind_image("jfa_out_img", &jfa_tx_.current());
      if (!packed) {
        step.bind_texture("outline_seed_tx", &edge_seed_tx_.current());
      }
      if (visible) {
        step.bind_texture("outline_info_tx", &inst_.render_buffers.outline_info_tx);
        step.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
        step.bind_texture("outline_occlusion_depth_tx", &outline_occlusion_depth_tx_);
        step.push_constant("use_outline_occlusion_depth", &use_outline_occlusion_depth_, 1);
      }
      step.push_constant("jfa_step_size", &jfa_step_size_, 1);
      step.dispatch(jfa_dispatch_buf_);
      step.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    }

    PassSimple &resolve = resolve_ps_[variant];
    resolve.init();
    resolve.state_set(DRW_STATE_WRITE_COLOR);
    const eShaderType resolve_shader = reduced ?
                                           (packed ? OUTLINE_RESOLVE_SCALED_PACKED :
                                                     OUTLINE_RESOLVE_SCALED) :
                                           (packed ? OUTLINE_RESOLVE_PACKED : OUTLINE_RESOLVE);
    resolve.shader_set(inst_.shaders.static_shader_get(resolve_shader));
    if (reduced) {
      resolve.push_constant("outline_extent", &outline_extent_);
    }
    resolve.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
    resolve.bind_texture("vector_tx", &inst_.render_buffers.vector_tx);
    resolve.bind_texture("outline_occlusion_depth_tx", &outline_occlusion_depth_tx_);
    resolve.push_constant("use_outline_occlusion_depth", &use_outline_occlusion_depth_, 1);
    resolve.bind_texture("outline_seed_tx", &edge_seed_tx_.current());
    resolve.bind_texture("outline_color_tx", &inst_.render_buffers.outline_color_tx);
    resolve.bind_texture("outline_info_tx", &inst_.render_buffers.outline_info_tx);
    resolve.bind_texture("jfa_tx", &jfa_tx_.previous());
    resolve.bind_ssbo("outline_flags", &classification_buf_);
    resolve.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }

  if (reduced) {
    upsample_ps_.init();
    upsample_ps_.state_set(DRW_STATE_WRITE_COLOR);
    upsample_ps_.shader_set(inst_.shaders.static_shader_get(OUTLINE_UPSAMPLE));
    upsample_ps_.bind_texture("reduced_color_tx", &reduced_outline_tx_);
    upsample_ps_.bind_texture("reduced_depth_tx", &reduced_depth_tx_);
    upsample_ps_.bind_texture("reduced_velocity_tx", &reduced_velocity_tx_);
    upsample_ps_.bind_texture("reduced_id_tx", &reduced_id_tx_);
    upsample_ps_.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
    upsample_ps_.bind_texture("outline_info_tx", &inst_.render_buffers.outline_info_tx);
    upsample_ps_.bind_texture("outline_occlusion_depth_tx", &outline_occlusion_depth_tx_);
    upsample_ps_.push_constant("use_outline_occlusion_depth", &use_outline_occlusion_depth_, 1);
    upsample_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }
}

void OutlineModule::render(View &view, int2 extent)
{
  if (!enabled_) {
    return;
  }

  auto &drw = *inst_.manager;
  GPU_debug_group_begin("Outline");
  const bool reduced = resolution_scale_ != 1.0f;
  outline_extent_ = math::max(int2(1), (extent * int(resolution_scale_ * 4.0f) + int2(3)) / 4);
  const int group_size = OUTLINE_JFA_STEP_GROUP_SIZE;
  source_dispatch_size_ = int3(
      (extent.x + group_size - 1) / group_size, (extent.y + group_size - 1) / group_size, 1);
  jfa_dispatch_size_ = int3((outline_extent_.x + group_size - 1) / group_size,
                            (outline_extent_.y + group_size - 1) / group_size,
                            1);
  const int variant = packed_jfa_supported_ &&
                              math::max(outline_extent_.x, outline_extent_.y) <= 65535 ?
                          1 :
                          0;

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  use_outline_occlusion_depth_ = inst_.pipelines.forward.has_outline_occluders() ? 1 : 0;
  outline_occlusion_depth_tx_ = inst_.render_buffers.depth_tx;
  if (use_outline_occlusion_depth_ != 0) {
    GPU_debug_group_begin("Outline.Occlusion");
    occlusion_depth_tx_.acquire_2d(extent,
                                   gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8,
                                   GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
    occlusion_fb_.ensure(GPU_ATTACHMENT_TEXTURE(occlusion_depth_tx_));
    occlusion_fb_.bind();
    occlusion_fb_.clear_depth(inst_.film.depth.clear_value);
    inst_.pipelines.forward.render_outline_occlusion(view, occlusion_fb_);
    GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
    outline_occlusion_depth_tx_ = occlusion_depth_tx_;
    GPU_debug_group_end();
  }
  else {
    occlusion_depth_tx_.release();
  }

  edge_seed_tx_.current().acquire_2d(
      outline_extent_, gpu::TextureFormat::SFLOAT_16_16_16_16, GPU_TEXTURE_USAGE_GENERAL);
  edge_seed_tx_.previous().acquire_2d(
      outline_extent_, gpu::TextureFormat::SFLOAT_16_16_16_16, GPU_TEXTURE_USAGE_GENERAL);

  classification_buf_.clear_to_zero();
  GPU_memory_barrier(GPU_BARRIER_BUFFER_UPDATE | GPU_BARRIER_SHADER_STORAGE);
  drw.submit(source_classify_ps_);
  drw.submit(source_dispatch_ps_);

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  /* Detect pass: find edge pixels. */
  detect_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(edge_seed_tx_.current()));
  GPU_framebuffer_bind(detect_fb_);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  detect_ps_.framebuffer_set(&detect_fb_);
  drw.submit(detect_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);

  /* Freestyle edge pass: render marked edges into the seed buffer. */
  freestyle_edge_ps_.framebuffer_set(&detect_fb_);
  GPU_framebuffer_bind(detect_fb_);
  drw.submit(freestyle_edge_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  /* Classify actual FP16 seeds after Freestyle has overwritten any detected seeds. */
  drw.submit(classify_ps_);
  drw.submit(dispatch_ps_);

  /* Factor blur ping-pong: smooth the width-variation factor along the contour. The detect/
   * freestyle result is in current(); each iteration swaps so blur reads previous() (the latest
   * result, bound at sync) and writes current(), then we leave the final result in current() for
   * jfa_init/resolve. An even iteration count returns the result to the original current(). */
  const int factor_blur_iterations = 6;
  for (int i = 0; i < factor_blur_iterations; i++) {
    edge_seed_tx_.swap();
    factor_blur_fb_.ensure(GPU_ATTACHMENT_NONE,
                           GPU_ATTACHMENT_TEXTURE(edge_seed_tx_.current()));
    factor_blur_ps_.framebuffer_set(&factor_blur_fb_);
    GPU_framebuffer_bind(factor_blur_fb_);
    drw.submit(factor_blur_ps_, view);
    GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
  }

  /* JFA init: seed the coordinate table from edge pixels. */
  const gpu::TextureFormat jfa_format = variant != 0 ? gpu::TextureFormat::UINT_32_32 :
                                                       gpu::TextureFormat::SFLOAT_32_32;
  jfa_tx_.current().acquire_2d(outline_extent_, jfa_format, GPU_TEXTURE_USAGE_GENERAL);
  jfa_tx_.previous().acquire_2d(outline_extent_, jfa_format, GPU_TEXTURE_USAGE_GENERAL);

  jfa_init_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(jfa_tx_.current()));
  jfa_init_ps_[variant].framebuffer_set(&jfa_init_fb_);
  GPU_framebuffer_bind(jfa_init_fb_);
  drw.submit(jfa_init_ps_[variant], view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_SHADER_IMAGE_ACCESS);

  /* JFA passes: flood coordinates outward. */
  const int max_dim = math::max(outline_extent_.x, outline_extent_.y);
  int step_size = power_of_2_max_i(max_dim) / 2;

  /* 1+JFA variant: initial step_size=1 pass for accuracy. */
  const int step_variant = variant + (surface_identity_ == 1 ? 2 : 0);
  jfa_tx_.swap();
  jfa_step_size_ = 1;
  drw.submit(jfa_step_ps_[step_variant]);

  while (step_size >= 1) {
    jfa_tx_.swap();
    jfa_step_size_ = step_size;
    drw.submit(jfa_step_ps_[step_variant]);
    step_size /= 2;
  }

  /* Resolve pass: look up nearest seed and output the outline source result. */
  /* After the last swap+submit, the result is in jfa_tx_.current().
   * But resolve_ps_ reads from jfa_tx_.previous() (bound at sync time).
   * We need one more swap so previous() points to the final result. */
  jfa_tx_.swap();

  resolved_outline_tx_.acquire_2d(extent,
                                  gpu::TextureFormat::SFLOAT_16_16_16_16,
                                  GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
  resolved_depth_tx_.acquire_2d(extent,
                                gpu::TextureFormat::SFLOAT_32,
                                GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
  resolved_velocity_tx_.acquire_2d(extent,
                                   inst_.render_buffers.vector_tx_format(),
                                   GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
  const bool do_motion_vectors_swizzle = inst_.render_buffers.vector_tx_format() ==
                                         gpu::TextureFormat::SFLOAT_16_16;
  if (do_motion_vectors_swizzle) {
    GPU_texture_swizzle_set(resolved_velocity_tx_, "rgrg");
  }
  if (reduced) {
    const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ;
    reduced_outline_tx_.acquire_2d(outline_extent_, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
    reduced_depth_tx_.acquire_2d(outline_extent_, gpu::TextureFormat::SFLOAT_32, usage);
    reduced_velocity_tx_.acquire_2d(
        outline_extent_, inst_.render_buffers.vector_tx_format(), usage);
    reduced_id_tx_.acquire_2d(outline_extent_, gpu::TextureFormat::UINT_32, usage);
    if (do_motion_vectors_swizzle) {
      GPU_texture_swizzle_set(reduced_velocity_tx_, "rgrg");
    }
    resolve_fb_.ensure(GPU_ATTACHMENT_NONE,
                       GPU_ATTACHMENT_TEXTURE(reduced_outline_tx_),
                       GPU_ATTACHMENT_TEXTURE(reduced_depth_tx_),
                       GPU_ATTACHMENT_TEXTURE(reduced_velocity_tx_),
                       GPU_ATTACHMENT_TEXTURE(reduced_id_tx_));
  }
  else {
    resolve_fb_.ensure(GPU_ATTACHMENT_NONE,
                       GPU_ATTACHMENT_TEXTURE(resolved_outline_tx_),
                       GPU_ATTACHMENT_TEXTURE(resolved_depth_tx_),
                       GPU_ATTACHMENT_TEXTURE(resolved_velocity_tx_));
  }
  resolve_ps_[variant].framebuffer_set(&resolve_fb_);
  GPU_framebuffer_bind(resolve_fb_);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  drw.submit(resolve_ps_[variant], view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  if (reduced) {
    upsample_fb_.ensure(GPU_ATTACHMENT_NONE,
                        GPU_ATTACHMENT_TEXTURE(resolved_outline_tx_),
                        GPU_ATTACHMENT_TEXTURE(resolved_depth_tx_),
                        GPU_ATTACHMENT_TEXTURE(resolved_velocity_tx_));
    upsample_ps_.framebuffer_set(&upsample_fb_);
    GPU_framebuffer_bind(upsample_fb_);
    drw.submit(upsample_ps_, view);
    GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
    GPU_texture_swizzle_set(reduced_velocity_tx_, "rgba");
    reduced_outline_tx_.release();
    reduced_depth_tx_.release();
    reduced_velocity_tx_.release();
    reduced_id_tx_.release();
  }

  edge_seed_tx_.current().release();
  edge_seed_tx_.previous().release();
  jfa_tx_.current().release();
  jfa_tx_.previous().release();
  occlusion_depth_tx_.release();
  outline_occlusion_depth_tx_ = nullptr;
  use_outline_occlusion_depth_ = 0;
  GPU_debug_group_end();
}

void OutlineModule::release_result()
{
  if (resolved_velocity_tx_.is_valid()) {
    GPU_texture_swizzle_set(resolved_velocity_tx_, "rgba");
  }
  resolved_outline_tx_.release();
  resolved_depth_tx_.release();
  resolved_velocity_tx_.release();
  occlusion_depth_tx_.release();
  outline_occlusion_depth_tx_ = nullptr;
  use_outline_occlusion_depth_ = 0;
}

}  // namespace blender::eevee
