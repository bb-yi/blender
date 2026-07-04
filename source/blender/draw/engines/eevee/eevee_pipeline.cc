/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Shading passes contain draw-calls specific to shading pipelines.
 * They are to be shared across views.
 * This file is only for shading passes. Other passes are declared in their own module.
 */

#include "BLI_bounds.hh"
#include "GPU_capabilities.hh"

#include "eevee_instance.hh"
#include "eevee_pipeline.hh"
#include "eevee_shadow.hh"

#include "GPU_debug.hh"

#include "draw_common.hh"

namespace blender::eevee {

static eMaterialCullMethod material_surface_cull_method(const blender::Material *material)
{
  return material != nullptr ? blender::material_surface_cull_method_get(*material) :
                               MA_SURFACE_CULL_NONE;
}

static float material_stencil_order(const blender::Material *material)
{
  return material != nullptr ? float(material->stencil_order) : 0.0f;
}

static bool material_color_write_enabled(const blender::Material *material)
{
  return material == nullptr || blender::material_color_write_get(*material);
}

static bool material_depth_write_enabled(const blender::Material *material)
{
  return material == nullptr || blender::material_depth_write_get(*material);
}

static DRWState material_write_state(const blender::Material *material,
                                     const bool color_default,
                                     const bool depth_default)
{
  DRWState state = DRW_STATE_NO_DRAW;
  if (color_default && material_color_write_enabled(material)) {
    state |= DRW_STATE_WRITE_COLOR;
  }
  if (depth_default && material_depth_write_enabled(material)) {
    state |= DRW_STATE_WRITE_DEPTH;
  }
  return state;
}

static DRWState material_surface_cull_state(const eMaterialCullMethod cull_method)
{
  switch (cull_method) {
    case MA_SURFACE_CULL_BACK:
      return DRW_STATE_CULL_BACK;
    case MA_SURFACE_CULL_FRONT:
      return DRW_STATE_CULL_FRONT;
    case MA_SURFACE_CULL_NONE:
    default:
      return DRW_STATE_NO_DRAW;
  }
}

static eMaterialZTestMode material_ztest_mode(const blender::Material *material)
{
  return material != nullptr ? blender::material_ztest_mode_get(*material) : MA_ZTEST_LESS_EQUAL;
}

static DRWState material_ztest_state(const eMaterialZTestMode ztest_mode,
                                     const DRWState default_depth_state)
{
  const bool default_is_greater = (default_depth_state & DRW_STATE_DEPTH_TEST_ENABLED) ==
                                  DRW_STATE_DEPTH_GREATER_EQUAL;
  switch (ztest_mode) {
    case MA_ZTEST_LESS:
      return default_is_greater ? DRW_STATE_DEPTH_GREATER : DRW_STATE_DEPTH_LESS;
    case MA_ZTEST_GREATER:
      return default_is_greater ? DRW_STATE_DEPTH_LESS : DRW_STATE_DEPTH_GREATER;
    case MA_ZTEST_GREATER_EQUAL:
      return default_is_greater ? DRW_STATE_DEPTH_LESS_EQUAL : DRW_STATE_DEPTH_GREATER_EQUAL;
    case MA_ZTEST_EQUAL:
      return DRW_STATE_DEPTH_EQUAL;
    case MA_ZTEST_NOT_EQUAL:
      return DRW_STATE_DEPTH_NOT_EQUAL;
    case MA_ZTEST_ALWAYS:
      return DRW_STATE_DEPTH_ALWAYS;
    case MA_ZTEST_NEVER:
      return DRW_STATE_DEPTH_NEVER;
    case MA_ZTEST_LESS_EQUAL:
    default:
      return default_depth_state & DRW_STATE_DEPTH_TEST_ENABLED;
  }
}

static DRWState material_ztest_state_replace(DRWState state,
                                             const eMaterialZTestMode ztest_mode,
                                             const DRWState default_depth_state)
{
  return (state & ~DRW_STATE_DEPTH_TEST_ENABLED) |
         material_ztest_state(ztest_mode, default_depth_state);
}

template<typename PassType>
static PassType *material_surface_cull_pass_get(PassType *double_sided_ps,
                                                PassType *back_cull_ps,
                                                PassType *front_cull_ps,
                                                const blender::Material *material)
{
  switch (material_surface_cull_method(material)) {
    case MA_SURFACE_CULL_BACK:
      return back_cull_ps;
    case MA_SURFACE_CULL_FRONT:
      return front_cull_ps;
    case MA_SURFACE_CULL_NONE:
    default:
      return double_sided_ps;
  }
}

static bool material_needs_front_light_shader_resources(const GPUMaterial *gpumat,
                                                        const eClosureBits closure_bits)
{
  /* The raw GLSL light flag is an early registration hint. The precise evaluated-light marker is
   * only available after the material create info has been amended. */
  return (closure_bits & CLOSURE_SHADER_TO_RGBA) != 0 ||
         GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) ||
         GPU_material_has_glsl_light_shader_eval(gpumat) ||
         GPU_material_flag_get(gpumat, GPU_MATFLAG_GLSL_LIGHT_ACCESS);
}

static bool material_needs_lightprobe_resources(const GPUMaterial *gpumat)
{
  return GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) ||
         GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR_FOREACH_LIGHT) ||
         GPU_material_flag_get(gpumat, GPU_MATFLAG_LIGHTPROBE_ACCESS);
}

static DRWState material_stencil_drw_test_state(GPUStencilTest test)
{
  switch (test) {
    case GPU_STENCIL_ALWAYS:
      return DRW_STATE_STENCIL_ALWAYS;
    case GPU_STENCIL_EQUAL:
      return DRW_STATE_STENCIL_EQUAL;
    case GPU_STENCIL_NEQUAL:
      return DRW_STATE_STENCIL_NEQUAL;
    default:
      /* Full comparison is applied by state_stencil_test(). */
      return DRW_STATE_STENCIL_ALWAYS;
  }
}

static GPUStencilOpType material_stencil_op_type(eMaterialStencilOp op)
{
  switch (op) {
    case MA_STENCIL_OP_ZERO:
      return GPU_STENCIL_OP_ZERO;
    case MA_STENCIL_OP_REPLACE:
      return GPU_STENCIL_OP_REPLACE_VALUE;
    case MA_STENCIL_OP_INCREMENT_CLAMP:
      return GPU_STENCIL_OP_INCREMENT_CLAMP;
    case MA_STENCIL_OP_DECREMENT_CLAMP:
      return GPU_STENCIL_OP_DECREMENT_CLAMP;
    case MA_STENCIL_OP_INVERT:
      return GPU_STENCIL_OP_INVERT;
    case MA_STENCIL_OP_INCREMENT_WRAP:
      return GPU_STENCIL_OP_INCREMENT_WRAP;
    case MA_STENCIL_OP_DECREMENT_WRAP:
      return GPU_STENCIL_OP_DECREMENT_WRAP;
    case MA_STENCIL_OP_KEEP:
    default:
      return GPU_STENCIL_OP_KEEP;
  }
}

MaterialStencilState material_stencil_state_get(const blender::Material *material)
{
  MaterialStencilState state;
  if (material == nullptr || !material->stencil_enabled) {
    return state;
  }

  state.enabled = true;
  state.reference = uint8_t(material->stencil_reference & EEVEE_STENCIL_USER_MASK);
  state.read_mask = uint8_t(material->stencil_read_mask & EEVEE_STENCIL_USER_MASK);
  state.write_mask = uint8_t(material->stencil_write_mask & EEVEE_STENCIL_USER_MASK);
  state.pass = material_stencil_op_type(eMaterialStencilOp(material->stencil_pass_op));
  state.fail = material_stencil_op_type(eMaterialStencilOp(material->stencil_fail_op));
  state.zfail = material_stencil_op_type(eMaterialStencilOp(material->stencil_zfail_op));

  switch (eMaterialStencilTest(material->stencil_test)) {
    case MA_STENCIL_NEVER:
      state.test = GPU_STENCIL_NEVER;
      break;
    case MA_STENCIL_EQUAL:
      state.test = GPU_STENCIL_EQUAL;
      break;
    case MA_STENCIL_NOT_EQUAL:
      state.test = GPU_STENCIL_NEQUAL;
      break;
    case MA_STENCIL_LESS:
      state.test = GPU_STENCIL_LESS;
      break;
    case MA_STENCIL_LESS_EQUAL:
      state.test = GPU_STENCIL_LEQUAL;
      break;
    case MA_STENCIL_GREATER:
      state.test = GPU_STENCIL_GREATER;
      break;
    case MA_STENCIL_GREATER_EQUAL:
      state.test = GPU_STENCIL_GEQUAL;
      break;
    case MA_STENCIL_ALWAYS:
    default:
      state.test = GPU_STENCIL_ALWAYS;
      break;
  }
  state.test_state = material_stencil_drw_test_state(state.test);
  return state;
}

bool material_stencil_state_writes(const MaterialStencilState &state)
{
  return state.enabled && state.write_mask != 0u &&
         (state.pass != GPU_STENCIL_OP_KEEP || state.fail != GPU_STENCIL_OP_KEEP ||
          state.zfail != GPU_STENCIL_OP_KEEP);
}

static PassMain::Sub *material_stencil_pass_add(PassSortable &stencil_ps,
                                                Instance &inst,
                                                blender::Material *blender_mat,
                                                GPUMaterial *gpumat,
                                                bool has_motion,
                                                bool force_write_id)
{
  BLI_assert_msg(GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT) == false,
                 "Transparent stencil writers are not supported in v1.");

  MaterialStencilState stencil = material_stencil_state_get(blender_mat);
  if (!material_stencil_state_writes(stencil)) {
    return nullptr;
  }

  PassMain::Sub *pass = &stencil_ps.sub(GPU_material_get_name(gpumat),
                                        material_stencil_order(blender_mat));
  pass->bind_texture(RBUFS_UTILITY_TEX_SLOT, inst.pipelines.utility_tx);
  pass->bind_resources(inst.uniform_data);
  pass->bind_resources(inst.velocity);
  pass->bind_resources(inst.sampling);
  pass->bind_resources(inst.render_textures);
  const bool write_id = force_write_id || GPU_material_flag_get(gpumat, GPU_MATFLAG_RAYCAST);
  pass->subpass_transition(GPU_ATTACHMENT_WRITE,
                           {GPU_ATTACHMENT_WRITE, /* normal */
                            write_id ? GPU_ATTACHMENT_WRITE : GPU_ATTACHMENT_IGNORE,
                            has_motion ? GPU_ATTACHMENT_WRITE : GPU_ATTACHMENT_IGNORE});
  DRWState state = material_write_state(blender_mat, false, true) |
                   DRW_STATE_WRITE_STENCIL | DRW_STATE_CLIP_CONTROL_UNIT_RANGE |
                   inst.film.depth.test_state |
                   material_surface_cull_state(material_surface_cull_method(blender_mat));
  state = material_ztest_state_replace(
      state, material_ztest_mode(blender_mat), inst.film.depth.test_state);
  pass->state_set(state);
  pass->state_stencil_op(stencil.fail, stencil.zfail, stencil.pass);
  pass->state_stencil(stencil.write_mask, stencil.reference, stencil.read_mask);
  pass->state_stencil_test(stencil.test);
  pass->material_set(*inst.manager, gpumat, true);
  pass->push_constant("surface_cull_mode", int(material_surface_cull_method(blender_mat)));

  return pass;
}

static void material_stencil_test_only_state_set(PassMain::Sub &pass,
                                                 const blender::Material *blender_mat)
{
  const MaterialStencilState stencil = material_stencil_state_get(blender_mat);
  if (!stencil.enabled) {
    return;
  }

  pass.state_stencil_op(GPU_STENCIL_OP_KEEP, GPU_STENCIL_OP_KEEP, GPU_STENCIL_OP_KEEP);
  pass.state_stencil(0x0u, stencil.reference, stencil.read_mask);
  pass.state_stencil_test(stencil.test);
}

static bool material_uses_depth_offset_lighting_data(const blender::Material *material,
                                                     GPUMaterial *gpumat)
{
  return material != nullptr && gpumat != nullptr &&
         material->depth_offset_affect_lighting == 0 &&
         GPU_material_has_depth_offset_output(gpumat);
}

/* -------------------------------------------------------------------- */
/** \name World Pipeline
 *
 * Used to draw background.
 * \{ */

void BackgroundPipeline::sync(GPUMaterial *gpumat,
                              const float background_opacity,
                              const float background_blur)
{
  Manager &manager = *inst_.manager;
  RenderBuffers &rbufs = inst_.render_buffers;
  gpu::Shader *shader = (gpumat != nullptr) ? GPU_material_get_shader(gpumat) : nullptr;

  clear_ps_.init();
  clear_ps_.state_set(DRW_STATE_WRITE_COLOR);
  clear_ps_.shader_set(inst_.shaders.static_shader_get(RENDERPASS_CLEAR));
  /* RenderPasses & AOVs. Cleared by background (even if bad practice). */
  clear_ps_.bind_image("rp_color_img", &rbufs.rp_color_tx);
  clear_ps_.bind_image("rp_value_img", &rbufs.rp_value_tx);
  clear_ps_.bind_image("rp_cryptomatte_img", &rbufs.cryptomatte_tx);
  /* Required by validation layers. */
  clear_ps_.bind_resources(inst_.cryptomatte);
  clear_ps_.bind_resources(inst_.uniform_data);
  clear_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  /* To allow opaque pass rendering over it. */
  clear_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);

  world_ps_.init();
  if (shader == nullptr) {
    return;
  }
  world_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_CLIP_CONTROL_UNIT_RANGE |
                      DRW_STATE_DEPTH_EQUAL);
  world_ps_.material_set(manager, gpumat, false, inst_.anisotropic_filtering);
  world_ps_.push_constant("world_opacity_fade", background_opacity);
  world_ps_.push_constant("world_background_blur", square_f(background_blur));
  SphereProbeData &world_data = *static_cast<SphereProbeData *>(&inst_.light_probes.world_sphere_);
  world_ps_.push_constant("world_coord_packed", reinterpret_cast<int4 *>(&world_data.atlas_coord));
  world_ps_.bind_texture("utility_tx", inst_.pipelines.utility_tx);
  /* RenderPasses & AOVs. */
  world_ps_.bind_image("rp_color_img", &rbufs.rp_color_tx);
  world_ps_.bind_image("rp_value_img", &rbufs.rp_value_tx);
  world_ps_.bind_image("rp_cryptomatte_img", &rbufs.cryptomatte_tx);
  /* Required by validation layers. */
  world_ps_.bind_resources(inst_.cryptomatte);
  world_ps_.bind_resources(inst_.uniform_data);
  world_ps_.bind_resources(inst_.sampling);
  world_ps_.bind_resources(inst_.render_textures);
  world_ps_.bind_resources(inst_.sphere_probes);
  world_ps_.bind_resources(inst_.volume_probes);
  world_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  /* To allow opaque pass rendering over it. */
  world_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
}

void BackgroundPipeline::clear(View &view)
{
  inst_.manager->submit(clear_ps_, view);
}

void BackgroundPipeline::render(View &view, Framebuffer &combined_fb)
{
  GPU_framebuffer_bind(combined_fb);
  inst_.manager->submit(world_ps_, view);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name World Probe Pipeline
 * \{ */

void WorldPipeline::sync(GPUMaterial *gpumat)
{
  const int2 extent(1);
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_WRITE |
                                     GPU_TEXTURE_USAGE_SHADER_READ;
  gpu::Shader *shader = (gpumat != nullptr) ? GPU_material_get_shader(gpumat) : nullptr;
  dummy_cryptomatte_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, extent, usage);
  dummy_renderpass_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, extent, usage);
  dummy_aov_color_tx_.ensure_2d_array(gpu::TextureFormat::SFLOAT_16_16_16_16, extent, 1, usage);
  dummy_aov_value_tx_.ensure_2d_array(gpu::TextureFormat::SFLOAT_16, extent, 1, usage);

  PassSimple &pass = cubemap_face_ps_;
  pass.init();
  if (shader == nullptr) {
    use_lightpath_node_ = false;
    return;
  }
  pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_ALWAYS);

  Manager &manager = *inst_.manager;
  pass.material_set(manager, gpumat, false, inst_.anisotropic_filtering);
  pass.push_constant("world_opacity_fade", 1.0f);
  pass.push_constant("world_background_blur", 0.0f);
  pass.push_constant("world_coord_packed", int4(0.0f));
  pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
  pass.bind_image("rp_normal_img", dummy_renderpass_tx_);
  pass.bind_image("rp_light_img", dummy_renderpass_tx_);
  pass.bind_image("rp_diffuse_color_img", dummy_renderpass_tx_);
  pass.bind_image("rp_specular_color_img", dummy_renderpass_tx_);
  pass.bind_image("rp_emission_img", dummy_renderpass_tx_);
  pass.bind_image("rp_cryptomatte_img", dummy_cryptomatte_tx_);
  pass.bind_image("rp_color_img", dummy_aov_color_tx_);
  pass.bind_image("rp_value_img", dummy_aov_value_tx_);
  pass.bind_image("aov_color_img", dummy_aov_color_tx_);
  pass.bind_image("aov_value_img", dummy_aov_value_tx_);
  pass.bind_ssbo("aov_buf", &inst_.film.aovs_info);
  /* Required by validation layers. */
  pass.bind_resources(inst_.cryptomatte);
  pass.bind_resources(inst_.uniform_data);
  pass.bind_resources(inst_.sampling);
  pass.bind_resources(inst_.render_textures);
  pass.bind_resources(inst_.sphere_probes);
  pass.bind_resources(inst_.volume_probes);
  pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  /* Split the rendering of the world in two passes. */
  use_lightpath_node_ = GPU_material_flag_get(gpumat, GPU_MATFLAG_IS_DIFFUSE_OR_GLOSSY_RAY_FLAG);
}

void WorldPipeline::render(View &view)
{
  inst_.manager->submit(cubemap_face_ps_, view);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name World Volume Pipeline
 *
 * \{ */

void WorldVolumePipeline::sync(GPUMaterial *gpumat)
{
  is_valid_ = (gpumat != nullptr) && (GPU_material_status(gpumat) == GPU_MAT_SUCCESS) &&
              GPU_material_has_volume_output(gpumat);
  if (!is_valid_) {
    /* Skip if the material has not compiled yet. */
    return;
  }

  if (GPU_material_get_shader(gpumat) == nullptr) {
    is_valid_ = false;
    return;
  }

  world_ps_.init();
  world_ps_.state_set(DRW_STATE_WRITE_COLOR);
  world_ps_.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
  world_ps_.bind_resources(inst_.uniform_data);
  world_ps_.bind_resources(inst_.volume.properties);
  world_ps_.bind_resources(inst_.sampling);

  world_ps_.material_set(*inst_.manager, gpumat, false, inst_.anisotropic_filtering);
  /* Bind correct dummy texture for attributes defaults. */
  PassSimple::Sub *sub = volume_sub_pass(world_ps_, nullptr, nullptr, gpumat);

  is_valid_ = (sub != nullptr);
  if (is_valid_) {
    world_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
    /* Sync with object property pass. */
    world_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  }
}

void WorldVolumePipeline::render(View &view)
{
  if (!is_valid_) {
    /* Clear the properties buffer instead of rendering if there is no valid shader. */
    inst_.volume.prop_scattering_tx_.clear(float4(0.0f));
    inst_.volume.prop_extinction_tx_.clear(float4(0.0f));
    inst_.volume.prop_emission_tx_.clear(float4(0.0f));
    inst_.volume.prop_phase_tx_.clear(float4(0.0f));
    inst_.volume.prop_phase_weight_tx_.clear(float4(0.0f));
    return;
  }

  inst_.manager->submit(world_ps_, view);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shadow Pipeline
 *
 * \{ */

void ShadowPipeline::sync()
{
  render_ps_.init();

  {
    DRWState state = DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS;

    draw::PassMain::Sub &pass = render_ps_.sub("Shadow.Surface");
    pass.state_set(state);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_ssbo(SHADOW_RENDER_VIEW_BUF_SLOT, &inst_.shadows.render_view_buf_);
    pass.bind_image(SHADOW_ATLAS_IMG_SLOT, inst_.shadows.atlas_tx_);
    pass.bind_image(SHADOW_CASTER_ATLAS_IMG_SLOT, inst_.shadows.caster_atlas_ref());
    pass.bind_ssbo(SHADOW_RENDER_MAP_BUF_SLOT, &inst_.shadows.render_map_buf_);
    pass.bind_ssbo(SHADOW_PAGE_INFO_SLOT, &inst_.shadows.pages_infos_data_);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.render_textures);
    surface_double_sided_ps_ = &pass.sub("Shadow.Surface.Double-Sided");
    surface_single_sided_ps_ = &pass.sub("Shadow.Surface.Single-Sided");
    surface_single_sided_ps_->state_set(state | DRW_STATE_CULL_BACK);
  }
}

PassMain::Sub *ShadowPipeline::surface_material_add(blender::Material *material,
                                                    GPUMaterial *gpumat)
{
  PassMain::Sub *pass = (material->blend_flag & MA_BL_CULL_BACKFACE_SHADOW) ?
                            surface_single_sided_ps_ :
                            surface_double_sided_ps_;
  PassMain::Sub *material_pass = &pass->sub(GPU_material_get_name(gpumat));
  GPUPass *gpupass = GPU_material_get_pass(gpumat);
  material_pass->shader_set(GPU_pass_shader_get(gpupass));
  material_pass->push_constant("use_shadow_caster_atlas",
                               inst_.shadows.use_caster_atlas_push_ref());
  return material_pass;
}

void ShadowPipeline::render(View &view)
{
  inst_.manager->submit(render_ps_, view);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Screen Space Shadow Filter
 *
 * \{ */

void ScreenSpaceShadowFilter::sync()
{
  const float white = 1.0f;
  dummy_source_tx_.ensure_2d_array(
      gpu::TextureFormat::SFLOAT_16, int2(1), 1, GPU_TEXTURE_USAGE_SHADER_READ, &white);
  dummy_white_tx_.ensure_2d(
      gpu::TextureFormat::SFLOAT_16, int2(1), GPU_TEXTURE_USAGE_SHADER_READ, &white);

  horizontal_ps_.init();
  horizontal_ps_.state_set(DRW_STATE_WRITE_COLOR);
  horizontal_ps_.shader_set(inst_.shaders.static_shader_get(SHADOW_MASK_FILTER_LAYERED));
  horizontal_ps_.bind_texture("shadow_tx", &source_tx_);
  horizontal_ps_.bind_texture("depth_tx", &depth_tx_);
  horizontal_ps_.push_constant("shadow_layer", &source_layer_);
  horizontal_ps_.push_constant("filter_direction", int2(1, 0));
  horizontal_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  vertical_ps_.init();
  vertical_ps_.state_set(DRW_STATE_WRITE_COLOR);
  vertical_ps_.shader_set(inst_.shaders.static_shader_get(SHADOW_MASK_FILTER));
  vertical_ps_.bind_texture("shadow_tx", &scratch_tx_);
  vertical_ps_.bind_texture("depth_tx", &depth_tx_);
  vertical_ps_.push_constant("filter_direction", int2(0, 1));
  vertical_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  this->release();
}

void ScreenSpaceShadowFilter::set_source(gpu::Texture *source_tx, int source_layer)
{
  source_tx_ = (source_tx != nullptr && source_layer >= 0) ? source_tx :
                                                            dummy_source_tx_.gpu_texture();
  source_layer_ = max_ii(source_layer, 0);
  scene_shadow_tx_ = dummy_white_tx_.gpu_texture();
}

void ScreenSpaceShadowFilter::render(View &view, int2 extent, gpu::Texture *depth_tx)
{
  if (source_tx_ == nullptr || source_tx_ == dummy_source_tx_.gpu_texture() || depth_tx == nullptr) {
    scene_shadow_tx_ = dummy_white_tx_.gpu_texture();
    return;
  }

  scratch_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16, extent, GPU_TEXTURE_USAGE_GENERAL);
  filtered_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16, extent, GPU_TEXTURE_USAGE_GENERAL);

  depth_tx_ = depth_tx;

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  framebuffer_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(scratch_tx_));
  horizontal_ps_.framebuffer_set(&framebuffer_);
  GPU_framebuffer_bind(framebuffer_);
  inst_.manager->submit(horizontal_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  framebuffer_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(filtered_tx_));
  vertical_ps_.framebuffer_set(&framebuffer_);
  GPU_framebuffer_bind(framebuffer_);
  inst_.manager->submit(vertical_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

  scene_shadow_tx_ = filtered_tx_;
}

void ScreenSpaceShadowFilter::release()
{
  source_tx_ = dummy_source_tx_.gpu_texture();
  depth_tx_ = nullptr;
  scene_shadow_tx_ = dummy_white_tx_.gpu_texture();
  source_layer_ = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Prepass
 *
 * Helper class for handling prepasses in Forward and Deferred pipelines.
 * \{ */

void Prepass::init(DRWState extra_state,
                   bool supports_motion_vectors,
                   bool supports_raycast_visibility,
                   FunctionRef<void(PassMain &pass)> pass_setup_cb)
{
  common_state_ = DRW_STATE_WRITE_DEPTH | DRW_STATE_CLIP_CONTROL_UNIT_RANGE |
                  inst_.film.depth.test_state | extra_state;
  supports_motion_vectors_ = supports_motion_vectors;
  supports_raycast_visibility_ = supports_raycast_visibility;

  pass_.init();
  /* Common resources. */
  pass_.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
  pass_.bind_resources(inst_.uniform_data);
  pass_.bind_resources(inst_.velocity);
  pass_.bind_resources(inst_.sampling);
  if (pass_setup_cb) {
    pass_setup_cb(pass_);
  }

  static constexpr const char *subpass_names
      [2 /*hide from raycast*/][EEVEE_SURFACE_CULL_METHOD_COUNT][2 /*moving*/]
      [2 /*write id*/] = {
          {{{"DoubleSided.Static", "DoubleSided.Static.ID"},
            {"DoubleSided.Moving", "DoubleSided.Moving.ID"}},
           {{"BackCull.Static", "BackCull.Static.ID"},
            {"BackCull.Moving", "BackCull.Moving.ID"}},
           {{"FrontCull.Static", "FrontCull.Static.ID"},
            {"FrontCull.Moving", "FrontCull.Moving.ID"}}},
          {{{"HideFromRaycast.DoubleSided.Static", ""},
            {"HideFromRaycast.DoubleSided.Moving", ""}},
           {{"HideFromRaycast.BackCull.Static", ""},
            {"HideFromRaycast.BackCull.Moving", ""}},
           {{"HideFromRaycast.FrontCull.Static", ""},
            {"HideFromRaycast.FrontCull.Moving", ""}}}};
  static constexpr const char *ztest_names[8] = {
      "ZTest.LessEqual",
      "ZTest.Less",
      "ZTest.Greater",
      "ZTest.GreaterEqual",
      "ZTest.Equal",
      "ZTest.NotEqual",
      "ZTest.Always",
      "ZTest.Never",
  };

  for (bool hide_from_raycast : {false, true}) {
    for (int ztest_mode = MA_ZTEST_LESS_EQUAL; ztest_mode <= MA_ZTEST_NEVER; ztest_mode++) {
      PassMain::Sub &ztest_pass = pass_.sub(ztest_names[ztest_mode]);
      for (int cull_method = 0; cull_method < EEVEE_SURFACE_CULL_METHOD_COUNT; cull_method++) {
        for (bool moving : {false, true}) {
          for (bool write_id : {false, true}) {
            PassMain::Sub *&sub =
                subs_[hide_from_raycast][ztest_mode][cull_method][moving][write_id];
            PassMain::Sub *&setup_sub =
                setup_subs_[hide_from_raycast][ztest_mode][cull_method][moving][write_id];
            if ((hide_from_raycast && write_id) || (!supports_motion_vectors && moving) ||
                (!supports_raycast_visibility && !hide_from_raycast))
            {
              /* Never needed.
               * Object IDs are only used for checking raycast self-hits.
               * If the pipeline doesn't support motion vectors, Prepass::add should always be
               * called with has_motion == false.
               * If the pipeline doesn't support raycast visibility, Prepass::add should always be
               * called with hide_from_raycast == true. */
              sub = nullptr;
              setup_sub = nullptr;
              continue;
            }
            sub = &ztest_pass.sub(subpass_names[hide_from_raycast][cull_method][moving]
                                               [write_id]);
            setup_sub = &sub->sub("Setup");
          }
        }
      }
    }
  }

  dummy_raycast_depth_tx_.ensure_2d(RenderBuffers::depth_format, int2(1));
  dummy_raycast_id_tx_.ensure_2d(RenderBuffers::object_id_format, int2(1));
  dummy_raycast_normal_tx_.ensure_2d(RenderBuffers::prepass_normal_format, int2(1));
}

PassMain::Sub *Prepass::add(blender::Material *blender_mat,
                            GPUMaterial *gpumat,
                            bool has_motion,
                            bool hide_from_raycast,
                            bool force_write_id)
{
  const int cull_method = material_surface_cull_subpass_index(blender_mat);
  const int ztest_mode = material_ztest_mode(blender_mat);
  const bool has_raycast = GPU_material_flag_get(gpumat, GPU_MATFLAG_RAYCAST);
  const bool write_id = (force_write_id || has_raycast) && !hide_from_raycast;

  PassMain::Sub &sub = subs_[hide_from_raycast][ztest_mode][cull_method][has_motion][write_id]->sub(
      GPU_material_get_name(gpumat));
  if (has_raycast) {
    /* NOTE: Bound per subpass since material textures could override these slots. */
    sub.bind_texture(RAYCAST_DEPTH_TEX_SLOT,
                     hide_from_raycast ? &inst_.render_buffers.raycast_depth_tx :
                                         &dummy_raycast_depth_tx_);
    sub.bind_texture(OBJECT_ID_TEX_SLOT,
                     hide_from_raycast ? &inst_.render_buffers.object_id_tx :
                                         &dummy_raycast_id_tx_);
    sub.bind_texture(PREPASS_NORMAL_TEX_SLOT,
                     hide_from_raycast ? &inst_.render_buffers.prepass_normal_tx :
                                         &dummy_raycast_normal_tx_);
  }
  return &sub;
}

void Prepass::end_sync()
{
  const bool has_raycast = inst_.pipelines.has_raycast;
  const bool needs_camera_prepass_normal = !has_raycast &&
                                           (inst_.outline.enabled() ||
                                            inst_.lights.needs_front_light_shader());

  for (bool hide_from_raycast : {false, true}) {
    for (int ztest_mode = MA_ZTEST_LESS_EQUAL; ztest_mode <= MA_ZTEST_NEVER; ztest_mode++) {
      for (int cull_method = 0; cull_method < EEVEE_SURFACE_CULL_METHOD_COUNT; cull_method++) {
        for (bool moving : {false, true}) {
          for (bool write_id : {false, true}) {
            PassMain::Sub *sub =
                setup_subs_[hide_from_raycast][ztest_mode][cull_method][moving][write_id];
            if (!sub) {
              continue;
            }
            const bool write_raycast = has_raycast && !hide_from_raycast;
            const bool read_raycast = has_raycast && hide_from_raycast;
            const bool write_normal = write_raycast || needs_camera_prepass_normal;
            const bool write_motion = supports_motion_vectors_ && moving;
            DRWState state = material_ztest_state_replace(
                common_state_, eMaterialZTestMode(ztest_mode), inst_.film.depth.test_state);
            state |= material_surface_cull_state(eMaterialCullMethod(cull_method));
            SET_FLAG_FROM_TEST(state, write_normal || write_motion, DRW_STATE_WRITE_COLOR);
            sub->state_set(state);
            sub->subpass_transition(
                GPU_ATTACHMENT_WRITE,
                {write_normal ?
                     GPU_ATTACHMENT_WRITE :
                     (read_raycast ? GPU_ATTACHMENT_READ : GPU_ATTACHMENT_IGNORE), /* normal */
                 (write_raycast && write_id) ?
                     GPU_ATTACHMENT_WRITE :
                     (read_raycast ? GPU_ATTACHMENT_READ : GPU_ATTACHMENT_IGNORE),
                 write_motion ? GPU_ATTACHMENT_WRITE : GPU_ATTACHMENT_IGNORE});
          }
        }
      }
    }
  }

  for (int ztest_mode = MA_ZTEST_LESS_EQUAL; ztest_mode <= MA_ZTEST_NEVER; ztest_mode++) {
    if (supports_raycast_visibility_) {
      /* First Raycast-visible Subpass for each material z-test group. */
      setup_subs_[false][ztest_mode][MA_SURFACE_CULL_NONE][false][false]->bind_ubo(
          PIPELINE_BUF_SLOT, &pipeline_buf_copy_);
    }
    /* First HideFromRaycast Subpass for each material z-test group. */
    setup_subs_[true][ztest_mode][MA_SURFACE_CULL_NONE][false][false]->bind_ubo(
        PIPELINE_BUF_SLOT, &pipeline_buf_copy_hide_from_raycast_);
  }

  if (has_raycast && supports_raycast_visibility_) {
    setup_subs_[true][MA_ZTEST_LESS_EQUAL][false][false][false]->texture_copy(
        &fb_depth_tx_, &inst_.render_buffers.raycast_depth_tx);
  }
}

void Prepass::render(View &view, gpu::Texture *fb_depth_tx, bool can_raycast)
{
  *pipeline_buf_copy_.data() = *inst_.uniform_data.pipeline.data();
  pipeline_buf_copy_.can_raycast = false;
  pipeline_buf_copy_.push_update();

  *pipeline_buf_copy_hide_from_raycast_.data() = *inst_.uniform_data.pipeline.data();
  pipeline_buf_copy_hide_from_raycast_.can_raycast = can_raycast;
  pipeline_buf_copy_hide_from_raycast_.push_update();

  fb_depth_tx_ = fb_depth_tx;

  inst_.manager->submit(pass_, view);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Forward Pass
 *
 * NPR materials (using Closure to RGBA) or material using ALPHA_BLEND.
 * \{ */

void ForwardPipeline::sync()
{
  camera_forward_ = inst_.camera.forward();
  has_opaque_ = false;
  has_transparent_ = false;
  has_no_depth_ = false;
  has_colored_transparency_ = false;
  has_holdout_ = false;
  has_outline_occluders_ = false;
  has_stencil_ = false;

  stencil_ps_.init();
  prepass_.init({}, true, false, [&](PassMain &pass) {
    pass.bind_resources(inst_.render_textures);
  });

  {
    opaque_ps_.init();

    {
      /* Common resources. */
      opaque_ps_.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
      opaque_ps_.bind_texture(HIZ_PREVIOUS_LAYER_TEX_SLOT, &inst_.hiz_buffer.back.ref_tx_);
      opaque_ps_.bind_texture(RADIANCE_PREVIOUS_LAYER_TEX_SLOT, &inst_.render_buffers.combined_tx);
      opaque_ps_.bind_texture(OBJECT_ID_TEX_SLOT, &inst_.render_buffers.object_id_tx);
      opaque_ps_.bind_texture(PREPASS_NORMAL_TEX_SLOT, &inst_.render_buffers.prepass_normal_tx);
      opaque_ps_.bind_image(RBUFS_COLOR_SLOT, &inst_.render_buffers.rp_color_tx);
      opaque_ps_.bind_image(RBUFS_VALUE_SLOT, &inst_.render_buffers.rp_value_tx);
      opaque_ps_.bind_image(OUTLINE_COLOR_SLOT, &inst_.render_buffers.outline_color_tx);
      opaque_ps_.bind_image(OUTLINE_INFO_SLOT, &inst_.render_buffers.outline_info_tx);

      opaque_ps_.bind_resources(inst_.uniform_data);
      opaque_ps_.bind_resources(inst_.lights);
      inst_.lights.bind_front_light_shader_resources(opaque_ps_);
      opaque_ps_.bind_resources(inst_.shadows);
      opaque_ps_.bind_resources(inst_.volume.result);
      opaque_ps_.bind_resources(inst_.sampling);
      opaque_ps_.bind_resources(inst_.render_textures);
      opaque_ps_.bind_resources(inst_.hiz_buffer.front);
      opaque_ps_.bind_resources(inst_.volume_probes);
      opaque_ps_.bind_resources(inst_.sphere_probes);
      opaque_ps_.bind_resources(inst_.planar_probes);
    }

    const DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_CLIP_CONTROL_UNIT_RANGE |
                           DRW_STATE_DEPTH_EQUAL;

    static constexpr const char
        *subpass_names[2 /*Raycast*/][EEVEE_SURFACE_CULL_METHOD_COUNT] = {
            {"NoRaycast.DoubleSided", "NoRaycast.BackCull", "NoRaycast.FrontCull"},
            {"Raycast.DoubleSided", "Raycast.BackCull", "Raycast.FrontCull"}};

    for (bool raycast : {false, true}) {
      for (int cull_method = 0; cull_method < EEVEE_SURFACE_CULL_METHOD_COUNT; cull_method++) {
        PassMain::Sub *&pass = opaque_subpasses_[raycast][cull_method];
        pass = &opaque_ps_.sub(subpass_names[raycast][cull_method]);
        pass->state_set(state | material_surface_cull_state(eMaterialCullMethod(cull_method)));
        if (raycast) {
          pass->bind_texture(RAYCAST_DEPTH_TEX_SLOT, &inst_.render_buffers.raycast_depth_tx);
          pass->bind_texture(OBJECT_ID_TEX_SLOT, &inst_.render_buffers.object_id_tx);
          pass->bind_texture(PREPASS_NORMAL_TEX_SLOT, &inst_.render_buffers.prepass_normal_tx);
        }
      }
    }
  }
  {
    transparent_ps_.init();
    /* Workaround limitation of PassSortable. Use dummy pass that will be sorted first in all
     * circumstances. */
    PassMain::Sub &sub = transparent_ps_.sub("ResourceBind", -FLT_MAX);

    /* Common resources. */

    /* Textures. */
    sub.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    sub.bind_texture(HIZ_PREVIOUS_LAYER_TEX_SLOT, &inst_.hiz_buffer.back.ref_tx_);
    sub.bind_texture(RADIANCE_PREVIOUS_LAYER_TEX_SLOT, &inst_.render_buffers.combined_tx);
    sub.bind_texture(OBJECT_ID_TEX_SLOT, &inst_.render_buffers.object_id_tx);
    sub.bind_texture(PREPASS_NORMAL_TEX_SLOT, &inst_.render_buffers.prepass_normal_tx);
    sub.bind_image(RBUFS_COLOR_SLOT, &inst_.render_buffers.rp_color_tx);
    sub.bind_image(RBUFS_VALUE_SLOT, &inst_.render_buffers.rp_value_tx);
    sub.bind_image(OUTLINE_COLOR_SLOT, &inst_.render_buffers.outline_color_tx);
    sub.bind_image(OUTLINE_INFO_SLOT, &inst_.render_buffers.outline_info_tx);

    sub.bind_resources(inst_.uniform_data);
    sub.bind_resources(inst_.lights);
    inst_.lights.bind_front_light_shader_resources(sub);
    sub.bind_resources(inst_.shadows);
    sub.bind_resources(inst_.volume.result);
    sub.bind_resources(inst_.sampling);
    sub.bind_resources(inst_.render_textures);
    sub.bind_resources(inst_.hiz_buffer.front);
    sub.bind_resources(inst_.volume_probes);
    sub.bind_resources(inst_.sphere_probes);
    sub.bind_resources(inst_.planar_probes);
  }
  {
    no_depth_ps_.init();
    PassMain::Sub &sub = no_depth_ps_.sub("ResourceBind", -FLT_MAX);
    sub.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    sub.bind_texture(HIZ_PREVIOUS_LAYER_TEX_SLOT, &inst_.hiz_buffer.back.ref_tx_);
    sub.bind_texture(RADIANCE_PREVIOUS_LAYER_TEX_SLOT, &inst_.render_buffers.combined_tx);
    sub.bind_texture(OBJECT_ID_TEX_SLOT, &inst_.render_buffers.object_id_tx);
    sub.bind_texture(PREPASS_NORMAL_TEX_SLOT, &inst_.render_buffers.prepass_normal_tx);
    sub.bind_image(RBUFS_COLOR_SLOT, &inst_.render_buffers.rp_color_tx);
    sub.bind_image(RBUFS_VALUE_SLOT, &inst_.render_buffers.rp_value_tx);
    sub.bind_image(OUTLINE_COLOR_SLOT, &inst_.render_buffers.outline_color_tx);
    sub.bind_image(OUTLINE_INFO_SLOT, &inst_.render_buffers.outline_info_tx);
    sub.bind_resources(inst_.uniform_data);
    sub.bind_resources(inst_.lights);
    inst_.lights.bind_front_light_shader_resources(sub);
    sub.bind_resources(inst_.shadows);
    sub.bind_resources(inst_.volume.result);
    sub.bind_resources(inst_.sampling);
    sub.bind_resources(inst_.render_textures);
    sub.bind_resources(inst_.hiz_buffer.front);
    sub.bind_resources(inst_.volume_probes);
    sub.bind_resources(inst_.sphere_probes);
    sub.bind_resources(inst_.planar_probes);
  }
  {
    outline_occlusion_ps_.init();
    outline_occlusion_ps_.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    outline_occlusion_ps_.bind_resources(inst_.uniform_data);
    outline_occlusion_ps_.bind_resources(inst_.velocity);
    outline_occlusion_ps_.bind_resources(inst_.sampling);
    outline_occlusion_ps_.bind_resources(inst_.render_textures);
    outline_occlusion_ps_.bind_resources(inst_.lights);
  }
  {
    gpu::Shader *sh = inst_.shaders.static_shader_get(TRANSPARENCY_RESOLVE);

    resolve_ps_.init();
    resolve_ps_.state_set(DRW_STATE_WRITE_COLOR);
    resolve_ps_.shader_set(sh);
    resolve_ps_.bind_texture("transparency_r_tx", &transp_buffer_.r_channel_tx);
    resolve_ps_.bind_texture("transparency_g_tx", &transp_buffer_.g_channel_tx);
    resolve_ps_.bind_texture("transparency_b_tx", &transp_buffer_.b_channel_tx);
    resolve_ps_.bind_texture("transparency_a_tx", &transp_buffer_.a_channel_tx);
    resolve_ps_.bind_texture("combined_tx", &resolve_input_tx_);
    resolve_ps_.bind_image("rp_color_img", &inst_.render_buffers.rp_color_tx);
    resolve_ps_.bind_image("rp_value_img", &inst_.render_buffers.rp_value_tx);
    resolve_ps_.bind_resources(inst_.uniform_data);
    resolve_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }
}

void ForwardPipeline::end_sync()
{
  inst_.pipelines.data.use_monochromatic_transmittance = !use_colored_transparency();
  prepass_.end_sync();
}

PassMain::Sub *ForwardPipeline::prepass_opaque_add(blender::Material *blender_mat,
                                                   GPUMaterial *gpumat,
                                                   bool has_motion)
{
  BLI_assert_msg(GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT) == false,
                 "Forward Transparent should be registered directly without calling "
                 "PipelineModule::material_add()");

  /* If material is fully additive or transparent, we can skip the opaque prepass. */
  /* TODO(fclem): To skip it, we need to know if the transparent BSDF is fully white AND if there
   * is no mix shader (could do better constant folding but that's expensive). */

  has_opaque_ = true;
  inst_.lights.tag_front_light_shader_needed();
  PassMain::Sub *pass = prepass_.add(blender_mat, gpumat, has_motion, true);
  if (inst_.scene->eevee.use_outline && GPU_material_has_outline_output(gpumat)) {
    pass->bind_image(OUTLINE_COLOR_SLOT, &inst_.render_buffers.outline_color_tx);
    pass->bind_image(OUTLINE_INFO_SLOT, &inst_.render_buffers.outline_info_tx);
  }
  return pass;
}

PassMain::Sub *ForwardPipeline::stencil_opaque_add(blender::Material *blender_mat,
                                                   GPUMaterial *gpumat,
                                                   bool has_motion,
                                                   bool force_write_id)
{
  PassMain::Sub *pass = material_stencil_pass_add(
      stencil_ps_, inst_, blender_mat, gpumat, has_motion, force_write_id);
  has_stencil_ |= pass != nullptr;
  return pass;
}

PassMain::Sub *ForwardPipeline::material_opaque_add(const Object *ob,
                                                    blender::Material *blender_mat,
                                                    GPUMaterial *gpumat)
{
  BLI_assert_msg(GPU_material_flag_get(gpumat, GPU_MATFLAG_TRANSPARENT) == false,
                 "Forward Transparent should be registered directly without calling "
                 "PipelineModule::material_add()");
  has_holdout_ |= GPU_material_flag_get(gpumat, GPU_MATFLAG_HOLDOUT) ||
                  (ob->base_flag & BASE_HOLDOUT) || (ob->visibility_flag & OB_HOLDOUT);
  PassMain::Sub *pass = get_opaque_subpass(blender_mat, gpumat);
  has_opaque_ = true;
  inst_.lights.tag_front_light_shader_needed();
  PassMain::Sub *sub_pass = &pass->sub(GPU_material_get_name(gpumat));
  return sub_pass;
}

PassMain::Sub *ForwardPipeline::material_no_depth_add(const Object *ob,
                                                      blender::Material *blender_mat,
                                                      GPUMaterial *gpumat)
{
  BLI_assert_msg(material_color_write_enabled(blender_mat),
                 "No-depth visible pass is only used by materials that write color.");
  DRWState state = material_write_state(blender_mat, true, false) |
                   DRW_STATE_CLIP_CONTROL_UNIT_RANGE | inst_.film.depth.test_state |
                   material_surface_cull_state(material_surface_cull_method(blender_mat));
  state = material_ztest_state_replace(
      state, material_ztest_mode(blender_mat), inst_.film.depth.test_state);

  has_opaque_ = true;
  has_no_depth_ = true;
  has_holdout_ |= GPU_material_flag_get(gpumat, GPU_MATFLAG_HOLDOUT) ||
                  (ob->base_flag & BASE_HOLDOUT) || (ob->visibility_flag & OB_HOLDOUT);
  inst_.lights.tag_front_light_shader_needed();
  float sorting_value = math::dot(float3(ob->object_to_world().location()), camera_forward_);
  PassMain::Sub *pass = &no_depth_ps_.sub(GPU_material_get_name(gpumat), sorting_value);
  pass->state_set(state);
  pass->material_set(*inst_.manager, gpumat, true, inst_.anisotropic_filtering);
  pass->push_constant("surface_cull_mode", int(material_surface_cull_method(blender_mat)));
  material_stencil_test_only_state_set(*pass, blender_mat);
  pass->bind_resources(inst_.lights);
  inst_.lights.bind_front_light_shader_resources(*pass);
  return pass;
}

void ForwardPipeline::transparent_add(const Object *ob,
                                      const float3 &ob_location,
                                      blender::Material *blender_mat,
                                      GPUMaterial *gpumat,
                                      PassMain::Sub *&r_prepass_subpass,
                                      PassMain::Sub *&r_material_subpass)
{
  DRWState prepass_state = material_write_state(blender_mat, false, true) |
                           DRW_STATE_CLIP_CONTROL_UNIT_RANGE | inst_.film.depth.test_state;
  prepass_state = material_ztest_state_replace(
      prepass_state, material_ztest_mode(blender_mat), inst_.film.depth.test_state);
  prepass_state |= material_surface_cull_state(material_surface_cull_method(blender_mat));

  DRWState material_state = material_write_state(blender_mat, true, false) |
                            DRW_STATE_BLEND_TRANSPARENCY | DRW_STATE_CLIP_CONTROL_UNIT_RANGE |
                            inst_.film.depth.test_state;
  material_state = material_ztest_state_replace(
      material_state, material_ztest_mode(blender_mat), inst_.film.depth.test_state);
  material_state |= material_surface_cull_state(material_surface_cull_method(blender_mat));

  has_transparent_ = true;
  has_colored_transparency_ |= GPU_material_flag_get(gpumat,
                                                     GPU_MATFLAG_TRANSPARENT_MAYBE_COLORED);
  has_holdout_ |= GPU_material_flag_get(gpumat, GPU_MATFLAG_HOLDOUT) ||
                  (ob->base_flag & BASE_HOLDOUT) || (ob->visibility_flag & OB_HOLDOUT);
  /* Must be checked here too,
   * since this function is not called from PipelineModule::material_add. */
  inst_.pipelines.has_raycast |= GPU_material_flag_get(gpumat, GPU_MATFLAG_RAYCAST);
  inst_.lights.tag_front_light_shader_needed();

  /* Transparent needs to use one sub pass per object to support reordering.
   * NOTE: Pre-pass needs to be created first in order to be sorted first. */
  float sorting_value = math::dot(ob_location, camera_forward_);

  const bool has_raycast = GPU_material_flag_get(gpumat, GPU_MATFLAG_RAYCAST);

  /* Prepass */
  if (blender_mat->blend_flag & MA_BL_HIDE_BACKFACE) {
    PassMain::Sub *pass = &transparent_ps_.sub(GPU_material_get_name(gpumat), sorting_value);
    pass->state_set(prepass_state);
    pass->material_set(*inst_.manager, gpumat, true, inst_.anisotropic_filtering);
    pass->push_constant("surface_cull_mode", int(material_surface_cull_method(blender_mat)));
    material_stencil_test_only_state_set(*pass, blender_mat);
    if (has_raycast) {
      pass->bind_texture(RAYCAST_DEPTH_TEX_SLOT, &inst_.render_buffers.raycast_depth_tx);
      pass->bind_texture(OBJECT_ID_TEX_SLOT, &inst_.render_buffers.object_id_tx);
      pass->bind_texture(PREPASS_NORMAL_TEX_SLOT, &inst_.render_buffers.prepass_normal_tx);
    }
    r_prepass_subpass = pass;
  }

  /* Material */
  {
    PassMain::Sub *pass = &transparent_ps_.sub(GPU_material_get_name(gpumat), sorting_value);
    pass->state_set(material_state);
    pass->material_set(*inst_.manager, gpumat, true, inst_.anisotropic_filtering);
    pass->push_constant("surface_cull_mode", int(material_surface_cull_method(blender_mat)));
    material_stencil_test_only_state_set(*pass, blender_mat);
    pass->bind_resources(inst_.lights);
    inst_.lights.bind_front_light_shader_resources(*pass);
    if (has_raycast) {
      pass->bind_texture(RAYCAST_DEPTH_TEX_SLOT, &inst_.render_buffers.raycast_depth_tx);
      pass->bind_texture(OBJECT_ID_TEX_SLOT, &inst_.render_buffers.object_id_tx);
      pass->bind_texture(PREPASS_NORMAL_TEX_SLOT, &inst_.render_buffers.prepass_normal_tx);
    }
    r_material_subpass = pass;
  }
}

PassMain::Sub *ForwardPipeline::outline_occlusion_add(blender::Material *blender_mat,
                                                      GPUMaterial *gpumat)
{
  if (gpumat == nullptr) {
    return nullptr;
  }

  DRWState state = DRW_STATE_WRITE_DEPTH | DRW_STATE_CLIP_CONTROL_UNIT_RANGE |
                   inst_.film.depth.test_state;
  state = material_ztest_state_replace(
      state, material_ztest_mode(blender_mat), inst_.film.depth.test_state);
  state |= material_surface_cull_state(material_surface_cull_method(blender_mat));

  has_outline_occluders_ = true;
  PassMain::Sub *pass = &outline_occlusion_ps_.sub(GPU_material_get_name(gpumat));
  pass->state_set(state);
  pass->material_set(*inst_.manager, gpumat, true, inst_.anisotropic_filtering);
  pass->push_constant("surface_cull_mode", int(material_surface_cull_method(blender_mat)));
  material_stencil_test_only_state_set(*pass, blender_mat);
  return pass;
}

void ForwardPipeline::TransparencyBuffer::acquire(int2 extent, bool use_colored_transparency)
{
  eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ;

  if (!use_colored_transparency) {
    r_channel_tx.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16_16_16, usage);
    /* Dummy texture for validation. Will not be sampled or attached. */
    g_channel_tx.acquire_2d(int2(1), gpu::TextureFormat::UNORM_8_8_8_8);
    b_channel_tx.acquire_2d(int2(1), gpu::TextureFormat::UNORM_8_8_8_8);
    a_channel_tx.acquire_2d(int2(1), gpu::TextureFormat::UNORM_8_8_8_8);
  }
  else {
    r_channel_tx.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16, usage);
    g_channel_tx.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16, usage);
    b_channel_tx.acquire_2d(extent, gpu::TextureFormat::SFLOAT_16_16, usage);
    a_channel_tx.acquire_2d(extent, gpu::TextureFormat::UNORM_8_8, usage);
  }
}

void ForwardPipeline::TransparencyBuffer::release()
{
  r_channel_tx.release();
  g_channel_tx.release();
  b_channel_tx.release();
  a_channel_tx.release();
}

bool ForwardPipeline::use_colored_transparency() const
{
  /* The monochromatic transparent path regressed on this branch and only preserves the first
   * radiance channel in final renders. Forward opaque and custom depth-test/no-depth draws also
   * resolve through this path, and must preserve combined color when their depth test fails. */
  return has_opaque_ || has_no_depth_ || has_transparent_ || has_holdout_;
}

bool ForwardPipeline::has_outline_occluders() const
{
  return has_outline_occluders_;
}

void ForwardPipeline::render_outline_occlusion(View &view, Framebuffer &outline_occlusion_fb)
{
  if (!has_outline_occluders_) {
    return;
  }

  GPU_debug_group_begin("Forward.OutlineOcclusion");
  outline_occlusion_fb.bind();
  inst_.manager->submit(outline_occlusion_ps_, view);
  GPU_debug_group_end();
}

void ForwardPipeline::render(View &view,
                             gpu::Texture *depth_tx,
                             Framebuffer &prepass_fb,
                             Framebuffer &transparent_fb,
                             Framebuffer &combined_fb,
                             int2 extent)
{
  /* We need to ensure the pipeline runs if outputting the transparent render-pass (see #154895). */
  if (!has_transparent_ && !has_opaque_ && !has_no_depth_ &&
      inst_.render_buffers.data.transparent_id == -1)
  {
    return;
  }

  inst_.hiz_buffer.swap_layer();

  const bool needs_transparency_resolve = has_opaque_ || has_no_depth_ || has_transparent_ ||
                                          inst_.render_buffers.data.transparent_id != -1;
  const bool use_colored_transparency = needs_transparency_resolve &&
                                        this->use_colored_transparency();
  const bool32_t use_monochromatic_transmittance = !use_colored_transparency;
  if (inst_.pipelines.data.use_monochromatic_transmittance != use_monochromatic_transmittance) {
    inst_.pipelines.data.use_monochromatic_transmittance = use_monochromatic_transmittance;
    inst_.uniform_data.push_update();
  }

  GPU_debug_group_begin("Forward.Opaque");

  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainForwardPrepass);
    prepass_fb.bind();
    prepass_.render(view, nullptr, true);
  }

  if (has_stencil_) {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainForwardPrepass);
    prepass_fb.bind();
    inst_.manager->submit(stencil_ps_, view);
  }

  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainForwardHiZUpdate);
    inst_.hiz_buffer.set_dirty();
    inst_.hiz_buffer.update();
  }

  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                           TelemetryStageId::MainForwardShadowSetup);
    inst_.shadows.set_view(view, extent, TelemetryShadowContext::MainView);
    inst_.shadows.render(view, extent);
  }
  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                           TelemetryStageId::MainForwardProbeSetup);
    inst_.volume_probes.set_view(view);
    inst_.sphere_probes.set_view(view);
  }
  inst_.lights.eval_uniform_light_shaders(view);
  inst_.lights.eval_front_light_shaders(view, extent);

  if (needs_transparency_resolve) {
    ScopedTelemetrySample telemetry_sample(
        inst_.telemetry, TelemetryStageId::MainForwardTransparencySetup);
    transp_buffer_.acquire(extent, use_colored_transparency);

    if (!use_colored_transparency) {
      /* NOTE: When using Vulkan this triggers a (false positive) validation warning about writing
       * to an attachment that isn't filled. The warning could be removed by adding dummy
       * attachments, recompiling the shader, etc. But it is not worth the hassle.
       *
       * VUID: Undefined-Value-ShaderOutputNotConsumed-DynamicRendering
       * MessageId: 0x46877e3e
       */
      transparent_fb.ensure(GPU_ATTACHMENT_TEXTURE(depth_tx),
                            GPU_ATTACHMENT_TEXTURE(transp_buffer_.r_channel_tx));
    }
    else {
      transparent_fb.ensure(GPU_ATTACHMENT_TEXTURE(depth_tx),
                            GPU_ATTACHMENT_TEXTURE(transp_buffer_.r_channel_tx),
                            GPU_ATTACHMENT_TEXTURE(transp_buffer_.g_channel_tx),
                            GPU_ATTACHMENT_TEXTURE(transp_buffer_.b_channel_tx),
                            GPU_ATTACHMENT_TEXTURE(transp_buffer_.a_channel_tx));
    }

    transparent_fb.bind();

    if (use_colored_transparency) {
      /* Split channel targets. Radiance in 1st channel, transmittance in 2nd channel. */
      transparent_fb.clear_color(float4(0.0f, 1.0f, 0.0f, 0.0f));
    }
    else {
      transparent_fb.clear_color(float4(0.0f, 0.0f, 0.0f, 1.0f));
    }
  }

  if (has_opaque_) {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainForwardOpaque);
    inst_.manager->submit(opaque_ps_, view);
  }

  if (has_no_depth_) {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainForwardOpaque);
    inst_.manager->submit(no_depth_ps_, view);
  }

  GPU_debug_group_end();

  if (has_transparent_) {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                           TelemetryStageId::MainForwardTransparent);
    inst_.manager->submit(transparent_ps_, view);
  }

  if (needs_transparency_resolve) {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainForwardResolve);
    resolve_input_tx_.acquire_2d(
        extent, GPU_texture_format(inst_.render_buffers.combined_tx), GPU_TEXTURE_USAGE_GENERAL);
    GPU_texture_copy(resolve_input_tx_, inst_.render_buffers.combined_tx);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH |
                       GPU_BARRIER_FRAMEBUFFER);
    combined_fb.bind();
    inst_.manager->submit(resolve_ps_, view);

    resolve_input_tx_.release();
    transp_buffer_.release();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Deferred Layer
 * \{ */

void DeferredLayerBase::gbuffer_pass_sync(Instance &inst)
{
  gbuffer_ps_.init();
  gbuffer_ps_.subpass_transition(GPU_ATTACHMENT_WRITE,
                                 {GPU_ATTACHMENT_WRITE,
                                  GPU_ATTACHMENT_WRITE,
                                  GPU_ATTACHMENT_WRITE,
                                  GPU_ATTACHMENT_WRITE,
                                  GPU_ATTACHMENT_WRITE});
  /* G-buffer. */
  inst.gbuffer.bind_optional_layers(gbuffer_ps_);
  /* RenderPasses & AOVs. */
  gbuffer_ps_.bind_image(RBUFS_COLOR_SLOT, &inst.render_buffers.rp_color_tx);
  gbuffer_ps_.bind_image(RBUFS_VALUE_SLOT, &inst.render_buffers.rp_value_tx);
  /* Cryptomatte. */
  gbuffer_ps_.bind_image(RBUFS_CRYPTOMATTE_SLOT, &inst.render_buffers.cryptomatte_tx);
  gbuffer_ps_.bind_image(OUTLINE_COLOR_SLOT, &inst.render_buffers.outline_color_tx);
  gbuffer_ps_.bind_image(OUTLINE_INFO_SLOT, &inst.render_buffers.outline_info_tx);
  /* Storage Buffer. */
  /* Textures. */
  gbuffer_ps_.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst.pipelines.utility_tx);

  gbuffer_ps_.bind_resources(inst.uniform_data);
  gbuffer_ps_.bind_resources(inst.sampling);
  gbuffer_ps_.bind_resources(inst.hiz_buffer.front);
  gbuffer_ps_.bind_resources(inst.render_textures);
  gbuffer_ps_.bind_resources(inst.sphere_probes);
  gbuffer_ps_.bind_resources(inst.volume_probes);
  gbuffer_ps_.bind_resources(inst.cryptomatte);

  DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL | DRW_STATE_WRITE_STENCIL |
                   DRW_STATE_CLIP_CONTROL_UNIT_RANGE | DRW_STATE_STENCIL_ALWAYS;

  static constexpr const char *subpass_names[2 /*Hybrid*/][2 /*Raycast*/]
                                            [EEVEE_SURFACE_CULL_METHOD_COUNT] = {
                                                {{"Deferred.NoRaycast.DoubleSided",
                                                  "Deferred.NoRaycast.BackCull",
                                                  "Deferred.NoRaycast.FrontCull"},
                                                 {"Deferred.Raycast.DoubleSided",
                                                  "Deferred.Raycast.BackCull",
                                                  "Deferred.Raycast.FrontCull"}},
                                                {{"Hybrid.NoRaycast.DoubleSided",
                                                  "Hybrid.NoRaycast.BackCull",
                                                  "Hybrid.NoRaycast.FrontCull"},
                                                 {"Hybrid.Raycast.DoubleSided",
                                                  "Hybrid.Raycast.BackCull",
                                                  "Hybrid.Raycast.FrontCull"}}};

  for (bool hybrid : {false, true}) {
    for (bool raycast : {false, true}) {
      for (int cull_method = 0; cull_method < EEVEE_SURFACE_CULL_METHOD_COUNT; cull_method++) {
        PassMain::Sub *&pass = gbuffer_subpasses_[hybrid][raycast][cull_method];
        pass = &gbuffer_ps_.sub(subpass_names[hybrid][raycast][cull_method]);
        pass->state_set(state | material_surface_cull_state(eMaterialCullMethod(cull_method)));
        if (hybrid) {
          pass->bind_resources(inst.lights);
          pass->bind_resources(inst.shadows);
          pass->bind_resources(inst.sphere_probes);
          pass->bind_resources(inst.volume_probes);
          if (is_probe_) {
            pass->bind_resources(inst.planar_probes.dummy_resources);
          }
          else {
            pass->bind_resources(inst.planar_probes);
          }
          pass->bind_texture(HIZ_PREVIOUS_LAYER_TEX_SLOT, &inst.hiz_buffer.back.ref_tx_);
          pass->bind_texture(RADIANCE_PREVIOUS_LAYER_TEX_SLOT, &radiance_behind_tx_);
        }
        if (raycast) {
          pass->bind_texture(RAYCAST_DEPTH_TEX_SLOT, &inst.render_buffers.raycast_depth_tx);
          pass->bind_texture(OBJECT_ID_TEX_SLOT, &inst.render_buffers.object_id_tx);
          pass->bind_texture(PREPASS_NORMAL_TEX_SLOT, &inst.render_buffers.prepass_normal_tx);
        }
      }
    }
  }

  closure_bits_ = CLOSURE_NONE;
  closure_count_ = 0;
  use_depth_offset_lighting_data_ = false;
  radiance_behind_tx_ = nullptr;
}

template<typename F> void DeferredLayerBase::npr_pass_sync(Instance &inst, F callback)
{
  npr_ps_.init();
  npr_ps_.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst.pipelines.utility_tx);
  npr_ps_.bind_texture(SCENE_SHADOW_TEX_SLOT, &inst.pipelines.shadow_filter.texture_ref());
  npr_ps_.bind_image(RBUFS_COLOR_SLOT, &inst.render_buffers.rp_color_tx);
  npr_ps_.bind_image(RBUFS_VALUE_SLOT, &inst.render_buffers.rp_value_tx);
  npr_aov_color_input_tx_ = inst.render_buffers.rp_color_tx;
  npr_aov_value_input_tx_ = inst.render_buffers.rp_value_tx;
  npr_ps_.bind_texture(NPR_AOV_COLOR_TEX_SLOT, &npr_aov_color_input_tx_);
  npr_ps_.bind_texture(NPR_AOV_VALUE_TEX_SLOT, &npr_aov_value_input_tx_);
  npr_ps_.bind_image(OUTLINE_COLOR_SLOT, &inst.render_buffers.outline_color_tx);
  npr_ps_.bind_image(OUTLINE_INFO_SLOT, &inst.render_buffers.outline_info_tx);
  /* Bind manually to fixed slots before the sub-pass shader is selected.
   * `bind_resources(inst.gbuffer)` resolves sampler bindings from the active shader interface,
   * which is not available yet during probe NPR pass setup and can crash in viewport sync. */
  npr_ps_.bind_texture(GBUF_NORMAL_TEX_SLOT, &inst.gbuffer.normal_tx);
  npr_ps_.bind_texture(GBUF_HEADER_TEX_SLOT, &inst.gbuffer.header_tx);
  npr_ps_.bind_texture(GBUF_CLOSURE_TEX_SLOT, &inst.gbuffer.closure_tx);
  npr_ps_.bind_texture(OBJECT_ID_TEX_SLOT, &inst.render_buffers.object_id_tx);
  npr_ps_.bind_texture(PREPASS_NORMAL_TEX_SLOT, &inst.render_buffers.prepass_normal_tx);
  npr_ps_.bind_resources(inst.uniform_data);
  npr_ps_.bind_resources(inst.sampling);
  npr_ps_.bind_resources(inst.hiz_buffer.front);
  npr_ps_.bind_resources(inst.render_textures);
  npr_ps_.bind_resources(inst.lights);
  inst.lights.bind_npr_front_light_shader_resources(npr_ps_);
  npr_ps_.bind_resources(inst.shadows);
  npr_ps_.bind_resources(inst.sphere_probes);
  npr_ps_.bind_resources(inst.volume_probes);

  callback();

  DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL |
                   DRW_STATE_CLIP_CONTROL_UNIT_RANGE;

  npr_double_sided_ps_ = &npr_ps_.sub("DoubleSided");
  npr_double_sided_ps_->state_set(state);

  npr_single_sided_ps_ = &npr_ps_.sub("BackCull");
  npr_single_sided_ps_->state_set(state | DRW_STATE_CULL_BACK);

  npr_front_cull_ps_ = &npr_ps_.sub("FrontCull");
  npr_front_cull_ps_->state_set(state | DRW_STATE_CULL_FRONT);
}

void DeferredLayer::begin_sync()
{
  has_outline_ = false;
  has_prepass_ = false;
  has_stencil_ = false;
  has_npr_aov_access_ = false;
  is_first_pass_ = true;
  stencil_ps_.init();

  /* Make alpha hash scale sub-pixel so that it converges to a noise free image.
   * If there is motion, use pixel scale for stability. */
  bool alpha_hash_subpixel_scale = !inst_.is_viewport() || !inst_.velocity.camera_has_motion();
  inst_.pipelines.data.alpha_hash_scale = alpha_hash_subpixel_scale ? 0.1f : 1.0f;

  /* Clear user stencil to 0 for this frame while keeping an internal untouched marker. The
   * marker is cleared only by fragments that pass the current layer prepass, so secondary layer
   * outline clearing stays limited to pixels actually touched by this layer. The material GBuffer
   * pass later rewrites this bit with its normal THICKNESS_FROM_SHADOW meaning before lighting
   * evaluation. */
  const uint8_t prepass_untouched_stencil = uint8_t(StencilBits::THICKNESS_FROM_SHADOW);
  prepass_.init(DRW_STATE_WRITE_STENCIL | DRW_STATE_STENCIL_ALWAYS,
                true,
                true,
                [&](PassMain &pass) {
                  pass.bind_resources(inst_.render_textures);
                  pass.clear_stencil(prepass_untouched_stencil);
                  pass.state_stencil(prepass_untouched_stencil, 0u, prepass_untouched_stencil);
                });

  {
    gpu::Shader *sh = inst_.shaders.static_shader_get(DEFERRED_AOV_CLEAR);
    clear_aovs_ps_.init();
    clear_aovs_ps_.shader_set(sh);
    clear_aovs_ps_.state_set(DRW_STATE_WRITE_STENCIL | DRW_STATE_STENCIL_EQUAL);
    clear_aovs_ps_.bind_image("rp_color_img", &inst_.render_buffers.rp_color_tx);
    clear_aovs_ps_.bind_image("rp_value_img", &inst_.render_buffers.rp_value_tx);
    clear_aovs_ps_.bind_image("rp_cryptomatte_img", &inst_.render_buffers.cryptomatte_tx);
    clear_aovs_ps_.bind_resources(inst_.cryptomatte);
    clear_aovs_ps_.bind_resources(inst_.uniform_data);
    clear_aovs_ps_.state_stencil(0xFFu, 0x0u, 0xFFu);
  }
  {
    aov_clear_ps_.init();
    aov_clear_ps_.state_set(DRW_STATE_WRITE_STENCIL | DRW_STATE_STENCIL_EQUAL |
                            DRW_STATE_CLIP_CONTROL_UNIT_RANGE);
    aov_clear_ps_.state_stencil(0x0u,
                                0x0u,
                                uint8_t(StencilBits::THICKNESS_FROM_SHADOW));
    aov_clear_ps_.shader_set(inst_.shaders.static_shader_get(DEFERRED_AOV_CLEAR));
    aov_clear_ps_.bind_image(RBUFS_COLOR_SLOT, &inst_.render_buffers.rp_color_tx);
    aov_clear_ps_.bind_image(RBUFS_VALUE_SLOT, &inst_.render_buffers.rp_value_tx);
    aov_clear_ps_.bind_image(OUTLINE_COLOR_SLOT, &inst_.render_buffers.outline_color_tx);
    aov_clear_ps_.bind_image(OUTLINE_INFO_SLOT, &inst_.render_buffers.outline_info_tx);
    aov_clear_ps_.bind_resources(inst_.uniform_data);
    aov_clear_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }

  this->gbuffer_pass_sync(inst_);
  this->npr_pass_sync(inst_, [&]() {
    npr_ps_.bind_texture(NPR_RADIANCE_TEX_SLOT, &npr_radiance_input_tx_);
    npr_ps_.bind_texture(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 0, &direct_radiance_txs_[0]);
    npr_ps_.bind_texture(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 1, &direct_radiance_txs_[1]);
    npr_ps_.bind_texture(DIRECT_RADIANCE_NPR_TX_SLOT_1 + 2, &direct_radiance_txs_[2]);
    npr_ps_.bind_texture(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 0, &indirect_result_.closures[0]);
    npr_ps_.bind_texture(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 1, &indirect_result_.closures[1]);
    npr_ps_.bind_texture(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + 2, &indirect_result_.closures[2]);
    npr_ps_.bind_texture(BACK_RADIANCE_TX_SLOT, &radiance_back_tx_);
    npr_ps_.bind_texture(BACK_HIZ_TX_SLOT, &inst_.hiz_buffer.back.ref_tx_);
  });
}

bool DeferredLayer::do_merge_direct_indirect_eval(const Instance &inst)
{
  return !inst.raytracing.use_raytracing();
}

bool DeferredLayer::do_split_direct_indirect_radiance(const Instance &inst)
{
  return do_merge_direct_indirect_eval(inst) &&
         (inst.sampling.use_clamp_direct() || inst.sampling.use_clamp_indirect());
}

void DeferredLayer::end_sync(bool is_first_pass,
                             bool is_last_pass,
                             bool next_layer_has_transmission)
{
  is_first_pass_ = is_first_pass;
  prepass_.end_sync();

  const bool has_any_closure = closure_bits_ != 0;
  /* We need the feedback output in case of refraction in the next pass (see #126455). */
  const bool is_layer_refracted = (next_layer_has_transmission && has_any_closure);
  const bool has_transmit_closure = (closure_bits_ & (CLOSURE_REFRACTION | CLOSURE_TRANSLUCENT));
  const bool has_reflect_closure = (closure_bits_ & (CLOSURE_REFLECTION | CLOSURE_DIFFUSE));
  const bool has_transparent_shader_to_rgba = (closure_bits_ & CLOSURE_TRANSPARENCY) &&
                                              (closure_bits_ & CLOSURE_SHADER_TO_RGBA);
  const bool use_direct_scale = inst_.sampling.use_direct_scale();
  const bool use_indirect_scale = inst_.sampling.use_indirect_scale();
  use_raytracing_ = (has_transmit_closure || has_reflect_closure) &&
                    inst_.raytracing.use_raytracing();
  use_clamp_direct_ = inst_.sampling.use_clamp_direct();
  use_clamp_indirect_ = inst_.sampling.use_clamp_indirect();
  /* Is the radiance split for the combined pass. */
  use_split_radiance_ = use_raytracing_ || use_clamp_direct_ || use_clamp_indirect_ ||
                        use_indirect_scale || use_direct_scale;

  /* The first pass will never have any surfaces behind it. Nothing is refracted except the
   * environment. So in this case, disable tracing and fallback to probe. */
  use_screen_transmission_ = use_raytracing_ &&
                             (has_transmit_closure || has_transparent_shader_to_rgba) &&
                             !is_first_pass;
  use_screen_reflection_ = use_raytracing_ && has_reflect_closure;

  use_feedback_output_ = (use_raytracing_ || is_layer_refracted) &&
                          (!is_last_pass || use_screen_reflection_);
  /* Keep a resolved radiance feedback for every non-first layer so transmission/refraction can
   * always sample the already composited layer behind, matching the prototype pipeline. */
  use_feedback_output_ = use_feedback_output_ || !is_first_pass;

  /* Clear AOVs in case previous layers wrote to them. First pass always get clear buffer because
   * of #BackgroundPipeline::clear(). */
  if (inst_.film.aovs_info.color_len > 0 && !is_first_pass) {
    clear_aovs_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }

  {
    RenderBuffersInfoData &rbuf_data = inst_.render_buffers.data;

    /* Add the stencil classification step at the end of the GBuffer pass. */
    {
      gpu::Shader *sh = inst_.shaders.static_shader_get(DEFERRED_TILE_CLASSIFY);
      PassMain::Sub &sub = gbuffer_ps_.sub("StencilClassify");
      sub.subpass_transition(GPU_ATTACHMENT_WRITE, /* Needed for depth test. */
                             {GPU_ATTACHMENT_IGNORE,
                              GPU_ATTACHMENT_READ, /* Header. */
                              GPU_ATTACHMENT_IGNORE,
                              GPU_ATTACHMENT_IGNORE,
                              GPU_ATTACHMENT_IGNORE});
      sub.shader_set(sh);
      if (GPU_stencil_clasify_buffer_workaround()) {
        /* Binding any buffer to satisfy the binding. The buffer is not actually used. */
        sub.bind_ssbo("dummy_workaround_buf", &inst_.film.aovs_info);
      }
      sub.state_set(DRW_STATE_WRITE_STENCIL | DRW_STATE_STENCIL_ALWAYS);
      if (GPU_stencil_export_support() && !has_stencil_) {
        /* The shader sets the stencil directly in one full-screen pass. */
        sub.state_stencil(uint8_t(StencilBits::HEADER_BITS),
                          /* Set by shader */ EEVEE_STENCIL_INTERNAL_MASK,
                          EEVEE_STENCIL_INTERNAL_MASK);
        sub.draw_procedural(GPU_PRIM_TRIS, 1, 3);
      }
      else {
        /* The shader cannot set the stencil directly. So we do one full-screen pass for each
         * stencil bit we need to set and accumulate the result. */
        auto set_bit = [&](StencilBits bit) {
          sub.push_constant("current_bit", int(bit));
          sub.state_stencil(uint8_t(bit), EEVEE_STENCIL_INTERNAL_MASK, EEVEE_STENCIL_INTERNAL_MASK);
          sub.draw_procedural(GPU_PRIM_TRIS, 1, 3);
        };

        if (closure_count_ > 0) {
          set_bit(StencilBits::CLOSURE_COUNT_0);
        }
        if (closure_count_ > 1) {
          set_bit(StencilBits::CLOSURE_COUNT_1);
        }
        if (closure_bits_ & CLOSURE_TRANSMISSION) {
          set_bit(StencilBits::TRANSMISSION);
        }
      }
    }

    {
      PassSimple &pass = eval_light_ps_;
      pass.init();

      /* TODO(fclem): Could also skip if no material uses thickness from shadow. */
      if (closure_bits_ & CLOSURE_TRANSMISSION) {
        PassSimple::Sub &sub = pass.sub("Eval.ThicknessFromShadow");
        sub.shader_set(inst_.shaders.static_shader_get(DEFERRED_THICKNESS_AMEND));
        sub.bind_resources(inst_.lights);
        sub.bind_resources(inst_.shadows);
        sub.bind_resources(inst_.hiz_buffer.front);
        sub.bind_resources(inst_.uniform_data);
        sub.bind_resources(inst_.sampling);
        sub.bind_resources(inst_.render_textures);
        sub.bind_texture("utility_tx", &inst_.pipelines.utility_tx);
        sub.bind_texture("gbuf_header_tx", &inst_.gbuffer.header_tx);
        sub.bind_image("gbuf_normal_img", &inst_.gbuffer.normal_tx);
        sub.state_set(DRW_STATE_WRITE_STENCIL | DRW_STATE_STENCIL_EQUAL);
        /* Render where there is transmission and the thickness from shadow bit is set. */
        uint8_t stencil_bits = uint8_t(StencilBits::TRANSMISSION) |
                               uint8_t(StencilBits::THICKNESS_FROM_SHADOW);
        sub.state_stencil(0x0u, stencil_bits, stencil_bits);
        sub.draw_procedural(GPU_PRIM_TRIS, 1, 3);
        sub.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
      }
      {
        const bool use_transmission = (closure_bits_ & CLOSURE_TRANSMISSION) != 0;
        const bool use_split_indirect = do_split_direct_indirect_radiance(inst_);
        const bool use_lightprobe_eval = do_merge_direct_indirect_eval(inst_);
        PassSimple::Sub &sub = pass.sub("Eval.Light");
        /* Stencil rejects pixels without GBuffer data. Do not also depth-test this fullscreen pass:
         * materials that write gl_FragDepth can move the prepass depth outside the fullscreen
         * triangle compare range, leaving valid GBuffer pixels unlit. */
        /* WORKAROUND: Avoid rasterizer discard by enabling stencil write, but the shaders actually
         * use no fragment output. */
        sub.state_set(DRW_STATE_WRITE_STENCIL | DRW_STATE_STENCIL_EQUAL);
        sub.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
        sub.bind_image(RBUFS_COLOR_SLOT, &inst_.render_buffers.rp_color_tx);
        sub.bind_image(RBUFS_VALUE_SLOT, &inst_.render_buffers.rp_value_tx);
        const ShadowSceneData &shadow_scene = inst_.shadows.get_data();
        auto set_specialization_constants =
            [&](PassSimple::Sub &sub, gpu::Shader *sh, bool use_transmission) {
              sub.specialize_constant(sh, "render_pass_shadow_id", rbuf_data.shadow_id);
              sub.specialize_constant(sh, "use_split_indirect", use_split_indirect);
              sub.specialize_constant(sh, "use_lightprobe_eval", use_lightprobe_eval);
              sub.specialize_constant(sh, "use_transmission", use_transmission);
              sub.specialize_constant(sh, "shadow_ray_count", &shadow_scene.ray_count);
              sub.specialize_constant(sh, "shadow_ray_step_count", &shadow_scene.step_count);
            };
        /* Submit the more costly ones first to avoid long tail in occupancy.
         * See page 78 of "SIGGRAPH 2023: Unreal Engine Substrate" by Hillaire & de Rousiers. */

        for (int i = min_ii(3, closure_count_) - 1; i >= 0; i--) {
          gpu::Shader *sh = inst_.shaders.static_shader_get(
              eShaderType(DEFERRED_LIGHT_SINGLE + i));
          set_specialization_constants(sub, sh, false);
          sub.shader_set(sh);
          sub.bind_image("direct_radiance_1_img", &direct_radiance_txs_[0]);
          sub.bind_image("direct_radiance_2_img", &direct_radiance_txs_[1]);
          sub.bind_image("direct_radiance_3_img", &direct_radiance_txs_[2]);
          sub.bind_image("indirect_radiance_1_img", &indirect_result_.closures[0]);
          sub.bind_image("indirect_radiance_2_img", &indirect_result_.closures[1]);
          sub.bind_image("indirect_radiance_3_img", &indirect_result_.closures[2]);
          sub.bind_resources(inst_.uniform_data);
          sub.bind_resources(inst_.gbuffer);
          sub.bind_resources(inst_.lights);
          inst_.lights.bind_light_shader_resources(sub);
          sub.bind_resources(inst_.shadows);
          sub.bind_resources(inst_.sampling);
          sub.bind_resources(inst_.render_textures);
          sub.bind_resources(inst_.hiz_buffer.front);
          sub.bind_resources(inst_.sphere_probes);
          sub.bind_resources(inst_.volume_probes);
          uint8_t compare_mask = uint8_t(StencilBits::CLOSURE_COUNT_0) |
                                 uint8_t(StencilBits::CLOSURE_COUNT_1) |
                                 uint8_t(StencilBits::TRANSMISSION);
          const uint8_t closure_stencil = uint8_t((i + 1) << 4);
          sub.state_stencil(0x0u, closure_stencil, compare_mask);
          sub.draw_procedural(GPU_PRIM_TRIS, 1, 3);
          if (use_transmission) {
            /* Separate pass for transmission BSDF as their evaluation is quite costly. */
            set_specialization_constants(sub, sh, true);
            sub.shader_set(sh);
            sub.state_stencil(
                0x0u, closure_stencil | uint8_t(StencilBits::TRANSMISSION), compare_mask);
            sub.draw_procedural(GPU_PRIM_TRIS, 1, 3);
          }
        }
      }
    }
    {
      PassSimple &pass = combine_ps_;
      pass.init();
      gpu::Shader *sh = inst_.shaders.static_shader_get(DEFERRED_COMBINE);
      /* TODO(fclem): Could specialize directly with the pass index but this would break it for
       * OpenGL and Vulkan implementation which aren't fully supporting the specialize
       * constant. */
      pass.specialize_constant(sh,
                               "render_pass_diffuse_light_enabled",
                               (rbuf_data.diffuse_light_id != -1) ||
                                   (rbuf_data.diffuse_color_id != -1));
      pass.specialize_constant(sh,
                               "render_pass_specular_light_enabled",
                               (rbuf_data.specular_light_id != -1) ||
                                   (rbuf_data.specular_color_id != -1));
      pass.specialize_constant(sh, "use_split_radiance", use_split_radiance_);
      pass.specialize_constant(
          sh, "use_radiance_feedback", use_feedback_output_ && use_clamp_direct_);
      pass.specialize_constant(sh, "render_pass_normal_enabled", rbuf_data.normal_id != -1);
      pass.specialize_constant(sh, "render_pass_position_enabled", rbuf_data.position_id != -1);
      pass.shader_set(sh);
      /* Use stencil test to reject pixels not written by this layer. */
      pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ADD_FULL | DRW_STATE_STENCIL_NEQUAL);
      /* Render where stencil is not 0. */
      pass.state_stencil(0x0u, 0x0u, uint8_t(StencilBits::HEADER_BITS));
      pass.bind_texture("direct_radiance_1_tx", &direct_radiance_txs_[0]);
      pass.bind_texture("direct_radiance_2_tx", &direct_radiance_txs_[1]);
      pass.bind_texture("direct_radiance_3_tx", &direct_radiance_txs_[2]);
      pass.bind_texture("indirect_radiance_1_tx", &indirect_result_.closures[0]);
      pass.bind_texture("indirect_radiance_2_tx", &indirect_result_.closures[1]);
      pass.bind_texture("indirect_radiance_3_tx", &indirect_result_.closures[2]);
      pass.bind_image(RBUFS_COLOR_SLOT, &inst_.render_buffers.rp_color_tx);
      pass.bind_image(RBUFS_VALUE_SLOT, &inst_.render_buffers.rp_value_tx);
      pass.bind_image("radiance_feedback_img", &radiance_feedback_tx_);
      pass.bind_resources(inst_.gbuffer);
      pass.bind_resources(inst_.uniform_data);
      pass.bind_resources(inst_.hiz_buffer.front);
      pass.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
      pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
    }
  }
}

PassMain::Sub *DeferredLayer::prepass_add(blender::Material *blender_mat,
                                          GPUMaterial *gpumat,
                                          bool has_motion,
                                          bool hide_from_raycast,
                                          bool force_write_id)
{
  has_prepass_ = true;
  return prepass_.add(blender_mat, gpumat, has_motion, hide_from_raycast, force_write_id);
}

PassMain::Sub *DeferredLayerBase::stencil_add(blender::Material *blender_mat,
                                              GPUMaterial *gpumat,
                                              Instance &inst,
                                              DRWState /*depth_state*/,
                                              bool has_motion,
                                              bool force_write_id)
{
  return material_stencil_pass_add(
      stencil_ps_, inst, blender_mat, gpumat, has_motion, force_write_id);
}

PassMain::Sub *DeferredLayer::stencil_add(blender::Material *blender_mat,
                                          GPUMaterial *gpumat,
                                          bool has_motion,
                                          bool force_write_id)
{
  PassMain::Sub *pass = DeferredLayerBase::stencil_add(
      blender_mat, gpumat, inst_, inst_.film.depth.test_state, has_motion, force_write_id);
  has_stencil_ |= pass != nullptr;
  has_prepass_ |= pass != nullptr;
  return pass;
}

PassMain::Sub *DeferredLayer::material_add(blender::Material *blender_mat, GPUMaterial *gpumat)
{
  eClosureBits closure_bits = shader_closure_bits_from_flag(gpumat);
  const bool color_write = material_color_write_enabled(blender_mat);
  const bool depth_write = material_depth_write_enabled(blender_mat);
  const bool depth_only = !color_write && depth_write;
  if (depth_only) {
    return nullptr;
  }
  if (closure_bits == eClosureBits(0)) {
    /* Fix the case where there is no active closure in the shader.
     * In this case we force the evaluation of emission to avoid disabling the entire layer by
     * accident, see #126459. */
    closure_bits |= CLOSURE_EMISSION;
  }
  closure_bits_ |= closure_bits;
  closure_count_ = max_ii(closure_count_, count_bits_i(closure_bits));
  use_depth_offset_lighting_data_ |= material_uses_depth_offset_lighting_data(blender_mat, gpumat);
  has_outline_ = has_outline_ || inst_.materials.material_uses_outline_control(blender_mat);

  const bool needs_front_light_shader = material_needs_front_light_shader_resources(gpumat,
                                                                                   closure_bits);
  if (needs_front_light_shader) {
    inst_.lights.tag_front_light_shader_needed();
  }
  PassMain::Sub *pass = get_gbuffer_subpass(blender_mat, gpumat);
  PassMain::Sub *material_pass = &pass->sub(GPU_material_get_name(gpumat));
  if (inst_.scene->eevee.use_outline && GPU_material_has_outline_output(gpumat)) {
    material_pass->bind_image(OUTLINE_COLOR_SLOT, &inst_.render_buffers.outline_color_tx);
    material_pass->bind_image(OUTLINE_INFO_SLOT, &inst_.render_buffers.outline_info_tx);
  }
  if (needs_front_light_shader) {
    material_pass->bind_resources(inst_.lights);
    inst_.lights.bind_front_light_shader_resources(*material_pass);
    material_pass->bind_resources(inst_.shadows);
  }
  if (material_needs_lightprobe_resources(gpumat)) {
    material_pass->bind_resources(inst_.sphere_probes);
    material_pass->bind_resources(inst_.volume_probes);
  }
  /* Set stencil for some deferred specialized shaders. */
  uint8_t material_stencil_bits = 0u;
  if (blender_mat->blend_flag & MA_BL_THICKNESS_FROM_SHADOW) {
    material_stencil_bits |= uint8_t(StencilBits::THICKNESS_FROM_SHADOW);
  }
  const MaterialStencilState stencil = material_stencil_state_get(blender_mat);
  const bool is_stencil_reader = stencil.enabled && !material_stencil_state_writes(stencil);
  if (is_stencil_reader) {
    /* Stencil readers have their test deferred from the prepass to here, so the stencil writer
     * pass has already executed and the user stencil bits are in place. */
    material_pass->state_stencil_op(
        GPU_STENCIL_OP_KEEP, GPU_STENCIL_OP_KEEP, GPU_STENCIL_OP_REPLACE_VALUE);
    material_pass->state_stencil(EEVEE_STENCIL_INTERNAL_MASK,
                                 material_stencil_bits | stencil.reference,
                                 stencil.read_mask);
    material_pass->state_stencil_test(stencil.test);
  }
  else {
    /* We use this opportunity to clear the stencil bits. The undefined areas are discarded using
     * the gbuf header value. */
    material_pass->state_stencil(EEVEE_STENCIL_INTERNAL_MASK,
                                 material_stencil_bits,
                                 EEVEE_STENCIL_INTERNAL_MASK);
  }

  return material_pass;
}

PassMain::Sub *DeferredLayer::npr_add(blender::Material *blender_mat, GPUMaterial *gpumat)
{
  BLI_assert(GPU_material_flag_get(gpumat, GPU_MATFLAG_NPR));
  use_depth_offset_lighting_data_ |= material_uses_depth_offset_lighting_data(blender_mat, gpumat);
  has_outline_ = has_outline_ || inst_.materials.material_uses_outline_control(blender_mat);
  has_npr_aov_access_ |= GPU_material_flag_get(gpumat, GPU_MATFLAG_AOV);
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) ||
      GPU_material_has_glsl_light_shader_eval(gpumat) ||
      GPU_material_flag_get(gpumat, GPU_MATFLAG_GLSL_LIGHT_ACCESS))
  {
    inst_.lights.tag_front_light_shader_needed();
  }
  PassMain::Sub *pass = material_surface_cull_pass_get(
      npr_double_sided_ps_, npr_single_sided_ps_, npr_front_cull_ps_, blender_mat);

  PassMain::Sub *material_pass = &pass->sub(GPU_material_get_name(gpumat));
  inst_.lights.bind_npr_front_light_shader_resources(*material_pass);

  /* Bind the material shader before setting NPR-specific push constants. */
  GPUPass *gpupass = GPU_material_get_pass(gpumat);
  material_pass->shader_set(GPU_pass_shader_get(gpupass));
  if (inst_.scene->eevee.use_outline && GPU_material_has_outline_output(gpumat)) {
    material_pass->bind_image(OUTLINE_COLOR_SLOT, &inst_.render_buffers.outline_color_tx);
    material_pass->bind_image(OUTLINE_INFO_SLOT, &inst_.render_buffers.outline_info_tx);
  }
  material_pass->push_constant("use_split_radiance", &use_split_radiance_);
  material_pass->push_constant("use_radiance_input_for_combined", false);

  return material_pass;
}

gpu::Texture *DeferredLayer::render(View &render_view,
                                    Framebuffer &prepass_fb,
                                    Framebuffer &combined_fb,
                                    Framebuffer &gbuffer_fb,
                                    int2 extent,
                                    RayTraceBuffer &rt_buffer,
                                    gpu::Texture *radiance_behind_tx)
{
  if (this->is_empty()) {
    return radiance_behind_tx;
  }

  radiance_behind_tx_ = radiance_behind_tx ? radiance_behind_tx : dummy_black;

  RenderBuffers &rb = inst_.render_buffers;

  npr_aov_color_input_tx_ = rb.rp_color_tx;
  npr_aov_value_input_tx_ = rb.rp_value_tx;

  const bool has_aovs = inst_.film.aovs_info.color_len > 0 || inst_.film.aovs_info.value_len > 0;
  /* Keep previous layer AOVs in the live render-pass buffer for NPR AOV Input.
   * Current-layer AOV Output writes naturally override them where the current material writes. */
  const bool preserve_npr_aov_input = !is_first_pass_ && has_npr_aov_access_ && has_aovs;

  constexpr eGPUTextureUsage usage_read = GPU_TEXTURE_USAGE_SHADER_READ;
  constexpr eGPUTextureUsage usage_write = GPU_TEXTURE_USAGE_SHADER_WRITE;
  constexpr eGPUTextureUsage usage_rw = usage_read | usage_write;

  if (use_screen_transmission_) {
    /* Update for refraction. */
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredHiZUpdate);
    inst_.hiz_buffer.update();
  }

  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredPrepass);
    GPU_framebuffer_bind(prepass_fb);
    /* Clear stencil buffer so that prepass can tag it. Then draw a full-screen triangle that will
     * clear AOVs for all the pixels touched by this layer. */
    GPU_framebuffer_clear_stencil(prepass_fb, 0xFFu);
    prepass_.render(render_view, rb.depth_tx, true);
    if (!clear_aovs_ps_.is_empty() && !preserve_npr_aov_input) {
      inst_.manager->submit(clear_aovs_ps_);
    }
  }

  if (closure_count_ == 0) {
    if (has_stencil_) {
      ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredPrepass);
      GPU_framebuffer_bind(prepass_fb);
      inst_.manager->submit(stencil_ps_, render_view);
    }
    inst_.hiz_buffer.swap_layer();
    inst_.hiz_buffer.update();
    npr_aov_color_input_tx_ = rb.rp_color_tx;
    npr_aov_value_input_tx_ = rb.rp_value_tx;
    return radiance_behind_tx;
  }

  if (!is_first_pass_ && !preserve_npr_aov_input) {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredPrepass);
    GPU_framebuffer_bind(prepass_fb);
    inst_.manager->submit(aov_clear_ps_, render_view);
  }

  inst_.hiz_buffer.swap_layer();
  /* Update for lighting pass or AO node. */
  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredHiZUpdate);
    inst_.hiz_buffer.update();
  }

  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                           TelemetryStageId::MainDeferredProbeSetup);
    inst_.volume_probes.set_view(render_view);
    inst_.sphere_probes.set_view(render_view);
  }
  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                           TelemetryStageId::MainDeferredShadowSetup);
    inst_.shadows.set_view(render_view, extent, TelemetryShadowContext::MainView);
    inst_.shadows.render(render_view, extent);
  }
  inst_.lights.eval_uniform_light_shaders(render_view);
  inst_.lights.eval_front_light_shaders(render_view, extent);

  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                           TelemetryStageId::MainDeferredGBufferPass);
    if (has_stencil_ && !GPU_stencil_export_support()) {
      /* GBuffer::bind() clears the full stencil buffer on backends without shader stencil export.
       * Material stencil writers must run after that clear so deferred GBuffer readers can observe
       * the user bits. */
      GPU_framebuffer_bind(gbuffer_fb);
      GPU_framebuffer_clear_stencil(gbuffer_fb, 0x0u);
    }
    if (has_stencil_) {
      GPU_framebuffer_bind(prepass_fb);
      inst_.manager->submit(stencil_ps_, render_view);
    }
    inst_.gbuffer.bind(gbuffer_fb, false, !has_stencil_);
    inst_.manager->submit(gbuffer_ps_, render_view);
  }
  inst_.lights.eval_light_shaders(render_view, extent);

  for (int i = 0; i < ARRAY_SIZE(direct_radiance_txs_); i++) {
    direct_radiance_txs_[i].acquire_2d((closure_count_ > i) ? extent : int2(1),
                                       gpu::TextureFormat::DEFERRED_RADIANCE_FORMAT,
                                       usage_rw);
  }

  if (use_raytracing_) {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredRaytrace);
    indirect_result_ = inst_.raytracing.render(
        rt_buffer, radiance_behind_tx, closure_bits_, render_view);
  }
  else if (use_split_radiance_) {
    indirect_result_ = inst_.raytracing.alloc_only(rt_buffer);
  }
  else {
    indirect_result_ = inst_.raytracing.alloc_dummy(rt_buffer);
  }

  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredEvalLight);
    GPU_framebuffer_bind(combined_fb);
    inst_.manager->submit(eval_light_ps_, render_view);
  }

  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredSubsurface);
    inst_.subsurface.render(
        direct_radiance_txs_[0], indirect_result_.closures[0], closure_bits_, render_view);
  }

  radiance_feedback_tx_ = rt_buffer.feedback_ensure(!use_feedback_output_, extent);
  radiance_back_tx_ = radiance_behind_tx ? radiance_behind_tx : radiance_feedback_tx_;

  if (use_feedback_output_ && use_clamp_direct_) {
    /* We need to do a copy before the combine pass (otherwise we have a dependency issue) to save
     * the emission and the previous layer's radiance. */
    GPU_texture_copy(radiance_feedback_tx_, rb.combined_tx);
  }

  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredCombine);
    GPU_framebuffer_bind(combined_fb);
    inst_.manager->submit(combine_ps_, render_view);
  }

  if (!npr_ps_.is_empty()) {
    gpu::Texture *shadow_source_tx = (rb.data.shadow_id >= 0) ? rb.rp_value_tx.gpu_texture() :
                                                                 nullptr;
    {
      ScopedTelemetrySample telemetry_sample(inst_.telemetry,
                                             TelemetryStageId::MainDeferredShadowFilter);
      inst_.pipelines.shadow_filter.set_source(shadow_source_tx, rb.data.shadow_id);
      inst_.pipelines.shadow_filter.render(render_view, extent, rb.depth_tx);
    }

    TextureFromPool npr_radiance_input = {"NPR Radiance Input"};
    npr_radiance_input.acquire_2d(
        extent, GPU_texture_format(rb.combined_tx), GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE);
    npr_radiance_input_tx_ = npr_radiance_input;
    GPU_texture_copy(npr_radiance_input_tx_, rb.combined_tx);
    {
      ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredNPR);
      GPU_framebuffer_bind(combined_fb);
      inst_.manager->submit(npr_ps_, render_view);
    }
    npr_radiance_input_tx_ = nullptr;
    npr_radiance_input.release();
  }

  if (use_feedback_output_) {
    /* The texture returned to later transmission/refraction passes must contain the final visible
     * radiance, including any NPR rewrite. The pre-combine copy above is only an intermediate
     * source used when the combine shader needs read/write-safe feedback access. */
    GPU_texture_copy(radiance_feedback_tx_, rb.combined_tx);
  }

  indirect_result_.release();

  for (int i = 0; i < ARRAY_SIZE(direct_radiance_txs_); i++) {
    direct_radiance_txs_[i].release();
  }

  inst_.pipelines.deferred.debug_draw(render_view, combined_fb);

  npr_aov_color_input_tx_ = rb.rp_color_tx;
  npr_aov_value_input_tx_ = rb.rp_value_tx;

  return use_feedback_output_ ? radiance_feedback_tx_ : nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Deferred Pipeline
 *
 * Closure data are written to intermediate buffer allowing screen space processing.
 * \{ */

void DeferredPipeline::begin_sync()
{
  use_combined_lightprobe_eval = !inst_.raytracing.use_raytracing();
  opaque_layer_.begin_sync();
  refraction_layers_.clear();
}

void DeferredPipeline::end_sync()
{
  opaque_layer_.end_sync(true, refraction_layers_.empty(), !refraction_layers_.empty());

  if (!refraction_layers_.empty()) {
    const short last_index = refraction_layers_.rbegin()->first;
    for (auto &[index, layer] : refraction_layers_) {
      layer->end_sync(opaque_layer_.is_empty(), index == last_index, index != last_index);
    }
  }

  inst_.pipelines.data.gbuffer_additional_data_layer_id = this->normal_layer_count() - 1;

  debug_pass_sync();
}

void DeferredPipeline::debug_pass_sync()
{
  Instance &inst = opaque_layer_.inst_;
  if (!ELEM(inst.debug_mode,
            eDebugMode::DEBUG_GBUFFER_EVALUATION,
            eDebugMode::DEBUG_GBUFFER_STORAGE))
  {
    return;
  }

  PassSimple &pass = debug_draw_ps_;
  pass.init();
  pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_CUSTOM);
  pass.shader_set(inst.shaders.static_shader_get(DEBUG_GBUFFER));
  pass.push_constant("debug_mode", int(inst.debug_mode));
  pass.bind_resources(inst.gbuffer);
  pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
}

void DeferredPipeline::debug_draw(draw::View &view, gpu::FrameBuffer *combined_fb)
{
  Instance &inst = opaque_layer_.inst_;
  if (!ELEM(inst.debug_mode,
            eDebugMode::DEBUG_GBUFFER_EVALUATION,
            eDebugMode::DEBUG_GBUFFER_STORAGE))
  {
    return;
  }

  switch (inst.debug_mode) {
    case eDebugMode::DEBUG_GBUFFER_EVALUATION:
      inst.info_append("Debug Mode: Deferred Lighting Cost");
      break;
    case eDebugMode::DEBUG_GBUFFER_STORAGE:
      inst.info_append("Debug Mode: Gbuffer Storage Cost");
      break;
    default:
      /* Nothing to display. */
      return;
  }

  GPU_framebuffer_bind(combined_fb);
  inst.manager->submit(debug_draw_ps_, view);
}

PassMain::Sub *PipelineModule::material_add(Object *ob,
                                            blender::Material *blender_mat,
                                            GPUMaterial *gpumat,
                                            eMaterialPipeline pipeline_type,
                                            eMaterialProbe probe_capture)
{
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_RAYCAST)) {
    has_raycast = true;
  }
  const bool hide_from_raycast = ob->visibility_flag & OB_HIDE_RAYCAST;
  if (GPU_material_has_shader_info_shadow_classification(gpumat) &&
      (pipeline_type == MAT_PIPE_DEFERRED || pipeline_type == MAT_PIPE_DEFERRED_NPR ||
       pipeline_type == MAT_PIPE_FORWARD))
  {
    inst_.shadows.tag_caster_atlas_needed();
  }

  if (probe_capture == MAT_PROBE_REFLECTION) {
    switch (pipeline_type) {
      case MAT_PIPE_PREPASS_DEFERRED:
        return probe.prepass_add(blender_mat, gpumat, hide_from_raycast);
      case MAT_PIPE_DEFERRED:
        return probe.material_add(blender_mat, gpumat);
      case MAT_PIPE_DEFERRED_NPR:
        return probe.npr_add(blender_mat, gpumat);
      default:
        BLI_assert_unreachable();
        break;
    }
  }
  if (probe_capture == MAT_PROBE_PLANAR) {
    switch (pipeline_type) {
      case MAT_PIPE_PREPASS_PLANAR:
        return planar.prepass_add(blender_mat, gpumat, hide_from_raycast);
      case MAT_PIPE_DEFERRED:
        return planar.material_add(blender_mat, gpumat);
      case MAT_PIPE_DEFERRED_NPR:
        return planar.npr_add(blender_mat, gpumat);
      default:
        BLI_assert_unreachable();
        break;
    }
  }

  switch (pipeline_type) {
    case MAT_PIPE_PREPASS_DEFERRED:
      return deferred.prepass_add(
          blender_mat, gpumat, false, ob->refraction_layer_index, hide_from_raycast);
    case MAT_PIPE_PREPASS_FORWARD:
      return forward.prepass_opaque_add(blender_mat, gpumat, false);
    case MAT_PIPE_PREPASS_OVERLAP:
      return forward.outline_occlusion_add(blender_mat, gpumat);

    case MAT_PIPE_PREPASS_DEFERRED_VELOCITY:
      return deferred.prepass_add(
          blender_mat, gpumat, true, ob->refraction_layer_index, hide_from_raycast);
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY:
      return forward.prepass_opaque_add(blender_mat, gpumat, true);

    case MAT_PIPE_DEFERRED:
      return deferred.material_add(blender_mat, gpumat, ob->refraction_layer_index);
    case MAT_PIPE_DEFERRED_NPR:
      return deferred.npr_add(blender_mat, gpumat, ob->refraction_layer_index);
    case MAT_PIPE_FORWARD:
      if (!material_color_write_get(*blender_mat)) {
        return nullptr;
      }
      if (!material_depth_write_get(*blender_mat)) {
        return forward.material_no_depth_add(ob, blender_mat, gpumat);
      }
      return forward.material_opaque_add(ob, blender_mat, gpumat);
    case MAT_PIPE_SHADOW:
      return shadow.surface_material_add(blender_mat, gpumat);
    case MAT_PIPE_CAPTURE:
      return capture.surface_material_add(blender_mat, gpumat);
    case MAT_PIPE_FILTER:
      BLI_assert_msg(0, "Filter shaders are evaluated by the filter material module.");
      return nullptr;
    case MAT_PIPE_BAKE_COLOR:
      BLI_assert_msg(0, "Bake shaders are evaluated by the Eevee bake callback.");
      return nullptr;

    case MAT_PIPE_VOLUME_OCCUPANCY:
    case MAT_PIPE_VOLUME_MATERIAL:
      BLI_assert_msg(0, "Volume shaders must register to the volume pipeline directly.");
      return nullptr;

    case MAT_PIPE_PREPASS_PLANAR:
      /* Should be handled by the `probe_capture == MAT_PROBE_PLANAR` case. */
      BLI_assert_unreachable();
      return nullptr;
  }
  return nullptr;
}

PassMain::Sub *DeferredPipeline::prepass_add(blender::Material *blender_mat,
                                             GPUMaterial *gpumat,
                                             bool has_motion,
                                             short refraction_layer,
                                             bool hide_from_raycast,
                                             bool force_write_id)
{
  if (!use_combined_lightprobe_eval && (blender_mat->blend_flag & MA_BL_SS_REFRACTION)) {
    return get_refraction_layer(refraction_layer)
        .prepass_add(blender_mat, gpumat, has_motion, hide_from_raycast, force_write_id);
  }
  return opaque_layer_.prepass_add(
      blender_mat, gpumat, has_motion, hide_from_raycast, force_write_id);
}

PassMain::Sub *DeferredPipeline::material_add(blender::Material *blender_mat,
                                              GPUMaterial *gpumat,
                                              short refraction_layer)
{
  if (!use_combined_lightprobe_eval && (blender_mat->blend_flag & MA_BL_SS_REFRACTION)) {
    return get_refraction_layer(refraction_layer).material_add(blender_mat, gpumat);
  }
  return opaque_layer_.material_add(blender_mat, gpumat);
}

PassMain::Sub *DeferredPipeline::stencil_add(blender::Material *blender_mat,
                                             GPUMaterial *gpumat,
                                             short refraction_layer,
                                             bool has_motion,
                                             bool force_write_id)
{
  if (!use_combined_lightprobe_eval && (blender_mat->blend_flag & MA_BL_SS_REFRACTION)) {
    return get_refraction_layer(refraction_layer).stencil_add(
        blender_mat, gpumat, has_motion, force_write_id);
  }
  return opaque_layer_.stencil_add(blender_mat, gpumat, has_motion, force_write_id);
}

PassMain::Sub *DeferredPipeline::npr_add(blender::Material *blender_mat,
                                         GPUMaterial *gpumat,
                                         short refraction_layer)
{
  if (!use_combined_lightprobe_eval && (blender_mat->blend_flag & MA_BL_SS_REFRACTION)) {
    return get_refraction_layer(refraction_layer).npr_add(blender_mat, gpumat);
  }
  return opaque_layer_.npr_add(blender_mat, gpumat);
}

void DeferredPipeline::render(View & /*main_view*/,
                              View &render_view,
                              Framebuffer &prepass_fb,
                              Framebuffer &combined_fb,
                              Framebuffer &gbuffer_fb,
                              int2 extent,
                              RayTraceBuffer &rt_buffer_opaque_layer,
                              RayTraceBuffer &rt_buffer_refract_layer)
{
  gpu::Texture *feedback_tx = nullptr;

  GPU_debug_group_begin("Deferred.Opaque");
  {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredOpaque);
    feedback_tx = opaque_layer_.render(render_view,
                                       prepass_fb,
                                       combined_fb,
                                       gbuffer_fb,
                                       extent,
                                       rt_buffer_opaque_layer,
                                       feedback_tx);
  }
  GPU_debug_group_end();

  GPU_debug_group_begin("Deferred.Refract");
  for (auto &[index, layer] : refraction_layers_) {
    ScopedTelemetrySample telemetry_sample(inst_.telemetry, TelemetryStageId::MainDeferredRefract);
    feedback_tx = layer->render(render_view,
                                prepass_fb,
                                combined_fb,
                                gbuffer_fb,
                                extent,
                                rt_buffer_refract_layer,
                                feedback_tx);
  }
  GPU_debug_group_end();
}

DeferredLayer &DeferredPipeline::get_refraction_layer(short index)
{
  auto layer_it = refraction_layers_.find(index);
  if (layer_it == refraction_layers_.end()) {
    auto layer = std::make_unique<DeferredLayer>(inst_);
    layer->begin_sync();
    layer_it = refraction_layers_.emplace(index, std::move(layer)).first;
  }
  return *layer_it->second;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Volume Layer
 *
 * \{ */

void VolumeLayer::sync()
{
  object_bounds_.clear();
  combined_screen_bounds_ = std::nullopt;
  use_hit_list = false;
  is_empty = true;
  finalized = false;
  has_scatter = false;
  has_absorption = false;

  draw::PassMain &layer_pass = volume_layer_ps_;
  layer_pass.init();
  layer_pass.clear_stencil(0x0u);
  {
    PassMain::Sub &pass = layer_pass.sub("occupancy_ps");
    /* Always double sided to let all fragments be invoked. */
    pass.state_set(DRW_STATE_WRITE_DEPTH);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.volume.occupancy);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.render_textures);
    occupancy_ps_ = &pass;
  }
  {
    PassMain::Sub &pass = layer_pass.sub("material_ps");
    /* Double sided with stencil equal to ensure only one fragment is invoked per pixel. */
    pass.state_set(DRW_STATE_WRITE_STENCIL | DRW_STATE_STENCIL_NEQUAL);
    pass.state_stencil(0x1u, 0x1u, 0x1u);
    pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.volume.properties);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.render_textures);
    material_ps_ = &pass;
  }
}

PassMain::Sub *VolumeLayer::occupancy_add(const Object *ob,
                                          const blender::Material *blender_mat,
                                          GPUMaterial *gpumat)
{
  BLI_assert_msg((ob->type == OB_VOLUME) || GPU_material_has_volume_output(gpumat),
                 "Only volume material should be added here");
  bool use_fast_occupancy = (ob->type == OB_VOLUME) ||
                            (blender_mat->volume_intersection_method == MA_VOLUME_ISECT_FAST);
  use_hit_list |= !use_fast_occupancy;
  is_empty = false;

  PassMain::Sub *pass = &occupancy_ps_->sub(GPU_material_get_name(gpumat));
  pass->material_set(*inst_.manager, gpumat, true, inst_.anisotropic_filtering);
  pass->push_constant("use_fast_method", use_fast_occupancy);
  return pass;
}

PassMain::Sub *VolumeLayer::material_add(const Object *ob,
                                         const blender::Material * /*blender_mat*/,
                                         GPUMaterial *gpumat)
{
  BLI_assert_msg((ob->type == OB_VOLUME) || GPU_material_has_volume_output(gpumat),
                 "Only volume material should be added here");
  UNUSED_VARS_NDEBUG(ob);

  PassMain::Sub *pass = &material_ps_->sub(GPU_material_get_name(gpumat));
  pass->material_set(*inst_.manager, gpumat, true, inst_.anisotropic_filtering);
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_VOLUME_SCATTER)) {
    has_scatter = true;
  }
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_VOLUME_ABSORPTION)) {
    has_absorption = true;
  }
  return pass;
}

bool VolumeLayer::bounds_overlaps(const VolumeObjectBounds &object_bounds) const
{
  /* First check the biggest area. */
  if (bounds::intersect(object_bounds.screen_bounds, combined_screen_bounds_)) {
    return true;
  }
  /* Check against individual bounds to try to squeeze the new object between them. */
  for (const std::optional<Bounds<float2>> &other_aabb : object_bounds_) {
    if (bounds::intersect(object_bounds.screen_bounds, other_aabb)) {
      return true;
    }
  }
  return false;
}

void VolumeLayer::add_object_bound(const VolumeObjectBounds &object_bounds)
{
  object_bounds_.append(object_bounds.screen_bounds);
  combined_screen_bounds_ = bounds::merge(combined_screen_bounds_, object_bounds.screen_bounds);
}

void VolumeLayer::render(View &view, Texture &occupancy_tx)
{
  if (is_empty) {
    return;
  }
  if (finalized == false) {
    finalized = true;
    if (use_hit_list) {
      /* Add resolve pass only when needed. Insert after occupancy, before material pass. */
      occupancy_ps_->shader_set(inst_.shaders.static_shader_get(VOLUME_OCCUPANCY_CONVERT));
      occupancy_ps_->barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
      occupancy_ps_->draw_procedural(GPU_PRIM_TRIS, 1, 3);
    }
  }
  /* TODO(fclem): Move this clear inside the render pass. */
  occupancy_tx.clear(uint4(0u));
  inst_.manager->submit(volume_layer_ps_, view);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Volume Pipeline
 * \{ */

void VolumePipeline::sync()
{
  object_integration_range_ = std::nullopt;
  has_scatter_ = false;
  has_absorption_ = false;
  for (auto &layer : layers_) {
    layer->sync();
  }
}

void VolumePipeline::render(View &view, Texture &occupancy_tx)
{
  for (auto &layer : layers_) {
    layer->render(view, occupancy_tx);
  }
}

VolumeObjectBounds::VolumeObjectBounds(const Camera &camera,
                                       const ObjectHandle &ob_handle,
                                       int instance_index)
{
  /* TODO(fclem): For panoramic camera, we will have to do this check for each cube-face. */
  const float4x4 &view_matrix = camera.data_get().viewmat;
  /* Note in practice we only care about the projection type since we only care about 2D overlap,
   * and this is independent of FOV. */
  const float4x4 &projection_matrix = camera.data_get().winmat;

  const Bounds<float3> bounds =
      BKE_object_boundbox_get(ob_handle.object).value_or(Bounds(float3(0.0f)));

  const std::array<float3, 8> corners = bounds::corners(bounds);

  screen_bounds = std::nullopt;
  z_range = std::nullopt;

  for (const float3 &l_corner : corners) {
    float3 ws_corner = math::transform_point(ob_handle.object_to_world(instance_index), l_corner);
    /* Split view and projection for precision. */
    float3 vs_corner = math::transform_point(view_matrix, ws_corner);
    float3 ss_corner = math::project_point(projection_matrix, vs_corner);

    z_range = bounds::min_max(z_range, vs_corner.z);
    if (camera.is_perspective() && vs_corner.z >= 1.0e-8f) {
      /* If the object is crossing the z=0 plane, we can't determine its 2D bounds easily.
       * In this case, consider the object covering the whole screen.
       * Still continue the loop for the Z range. */
      screen_bounds = Bounds<float2>(float2(-1.0f), float2(1.0f));
    }
    else {
      screen_bounds = bounds::min_max(screen_bounds, ss_corner.xy());
    }
  }
}

VolumeLayer *VolumePipeline::register_and_get_layer(const VolumeObjectBounds &object_bounds)
{
  if (math::reduce_max(object_bounds.screen_bounds->size()) < 1e-5) {
    /* WORKAROUND(fclem): Fixes an issue with 0 scaled object (see #132889).
     * Is likely to be an issue somewhere else in the pipeline but it is hard to find. */
    return nullptr;
  }

  object_integration_range_ = bounds::merge(object_integration_range_, object_bounds.z_range);

  /* Do linear search in all layers in order. This can be optimized. */
  for (auto &layer : layers_) {
    if (!layer->bounds_overlaps(object_bounds)) {
      layer->add_object_bound(object_bounds);
      return layer.get();
    }
  }
  /* No non-overlapping layer found. Create new one. */
  int64_t index = layers_.append_and_get_index(std::make_unique<VolumeLayer>(inst_));
  (*layers_[index]).add_object_bound(object_bounds);
  return layers_[index].get();
}

std::optional<Bounds<float>> VolumePipeline::object_integration_range() const
{
  return object_integration_range_;
}

bool VolumePipeline::use_hit_list() const
{
  for (const auto &layer : layers_) {
    if (layer->use_hit_list) {
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Deferred Probe Pipeline
 *
 * Closure data are written to intermediate buffer allowing screen space processing.
 * \{ */

void DeferredProbePipeline::begin_sync()
{
  opaque_layer_.prepass_.init({}, false, true, [&](PassMain &pass) {
    pass.clear_stencil(0x00u);
    pass.bind_resources(inst_.render_textures);
  });

  opaque_layer_.gbuffer_pass_sync(inst_);
  opaque_layer_.npr_pass_sync(inst_, [&]() {
    PassMain &npr_ps = opaque_layer_.npr_ps_;
    npr_ps.bind_texture(NPR_RADIANCE_TEX_SLOT, &npr_radiance_input_tx_);
    for (int i : IndexRange(3)) {
      npr_ps.bind_texture(DIRECT_RADIANCE_NPR_TX_SLOT_1 + i, &direct_radiance_txs_[i]);
      npr_ps.bind_texture(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + i, &indirect_radiance_txs_[i]);
    }
    npr_ps.bind_texture(BACK_RADIANCE_TX_SLOT, &dummy_black);
    npr_ps.bind_texture(BACK_HIZ_TX_SLOT, &dummy_black);
  });
}

void DeferredProbePipeline::end_sync()
{
  opaque_layer_.prepass_.end_sync();
  if (!opaque_layer_.gbuffer_ps_.is_empty()) {
    PassSimple &pass = eval_light_ps_;
    pass.init();
    /* Use depth test to reject background pixels. */
    pass.state_set(DRW_STATE_DEPTH_GREATER | DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ADD_FULL);
    pass.shader_set(inst_.shaders.static_shader_get(DEFERRED_CAPTURE_EVAL));
    pass.bind_image(RBUFS_COLOR_SLOT, &inst_.render_buffers.rp_color_tx);
    pass.bind_image(RBUFS_VALUE_SLOT, &inst_.render_buffers.rp_value_tx);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.gbuffer);
    pass.bind_resources(inst_.lights);
    inst_.lights.bind_light_shader_resources(pass);
    pass.bind_resources(inst_.shadows);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.hiz_buffer.front);
    pass.bind_resources(inst_.volume_probes);
    pass.bind_image("direct_radiance_1_img", &direct_radiance_txs_[0]);
    pass.bind_image("direct_radiance_2_img", &direct_radiance_txs_[1]);
    pass.bind_image("direct_radiance_3_img", &direct_radiance_txs_[2]);
    pass.bind_image("indirect_radiance_1_img", &indirect_radiance_txs_[0]);
    pass.bind_image("indirect_radiance_2_img", &indirect_radiance_txs_[1]);
    pass.bind_image("indirect_radiance_3_img", &indirect_radiance_txs_[2]);
    pass.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
    pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }
}

PassMain::Sub *DeferredProbePipeline::prepass_add(blender::Material *blender_mat,
                                                  GPUMaterial *gpumat,
                                                  bool hide_from_raycast,
                                                  bool force_write_id)
{
  return opaque_layer_.prepass_.add(
      blender_mat, gpumat, false, hide_from_raycast, force_write_id);
}

PassMain::Sub *DeferredProbePipeline::material_add(blender::Material *blender_mat,
                                                   GPUMaterial *gpumat)
{
  eClosureBits closure_bits = shader_closure_bits_from_flag(gpumat);
  if (closure_bits == eClosureBits(0)) {
    /* Fix the case where there is no active closure in the shader.
     * In this case we force the evaluation of emission to avoid disabling the entire layer by
     * accident, see #126459. */
    closure_bits |= CLOSURE_EMISSION;
  }
  opaque_layer_.closure_bits_ |= closure_bits;
  opaque_layer_.closure_count_ = max_ii(opaque_layer_.closure_count_, count_bits_i(closure_bits));
  opaque_layer_.use_depth_offset_lighting_data_ |= material_uses_depth_offset_lighting_data(
      blender_mat, gpumat);

  const bool needs_front_light_shader = material_needs_front_light_shader_resources(gpumat,
                                                                                   closure_bits);
  if (needs_front_light_shader) {
    inst_.lights.tag_front_light_shader_needed();
  }

  PassMain::Sub *pass = opaque_layer_.get_gbuffer_subpass(blender_mat, gpumat);
  PassMain::Sub *material_pass = &pass->sub(GPU_material_get_name(gpumat));
  if (needs_front_light_shader) {
    material_pass->bind_resources(inst_.lights);
    inst_.lights.bind_front_light_shader_resources(*material_pass);
    material_pass->bind_resources(inst_.shadows);
  }
  if (material_needs_lightprobe_resources(gpumat)) {
    material_pass->bind_resources(inst_.sphere_probes);
    material_pass->bind_resources(inst_.volume_probes);
  }
  return material_pass;
}

PassMain::Sub *DeferredProbePipeline::npr_add(blender::Material *blender_mat, GPUMaterial *gpumat)
{
  PassMain::Sub *pass = material_surface_cull_pass_get(opaque_layer_.npr_double_sided_ps_,
                                                       opaque_layer_.npr_single_sided_ps_,
                                                       opaque_layer_.npr_front_cull_ps_,
                                                       blender_mat);
  opaque_layer_.use_depth_offset_lighting_data_ |= material_uses_depth_offset_lighting_data(
      blender_mat, gpumat);
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) ||
      GPU_material_has_glsl_light_shader_eval(gpumat) ||
      GPU_material_flag_get(gpumat, GPU_MATFLAG_GLSL_LIGHT_ACCESS))
  {
    inst_.lights.tag_front_light_shader_needed();
  }

  PassMain::Sub *material_ps = &pass->sub(GPU_material_get_name(gpumat));
  inst_.lights.bind_npr_front_light_shader_resources(*material_ps);
  GPUPass *gpupass = GPU_material_get_pass(gpumat);
  material_ps->shader_set(GPU_pass_shader_get(gpupass));
  material_ps->push_constant("use_split_radiance", true);
  material_ps->push_constant("use_radiance_input_for_combined", false);

  return material_ps;
}

void DeferredProbePipeline::render(View &view,
                                   Framebuffer &prepass_fb,
                                   Framebuffer &combined_fb,
                                   Framebuffer &gbuffer_fb,
                                   int2 extent,
                                   gpu::Texture *combined_tx)
{
  GPU_debug_group_begin("Probe.Render");

  opaque_layer_.radiance_behind_tx_ = dummy_black;
  inst_.lights.set_view(view, extent);

  const eGPUTextureUsage usage_rw = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  for (int i = 0; i < ARRAY_SIZE(direct_radiance_txs_); i++) {
    const int2 target_extent = (opaque_layer_.closure_count_ > i) ? extent : int2(1);
    direct_radiance_txs_[i].acquire_2d(
        target_extent, gpu::TextureFormat::DEFERRED_RADIANCE_FORMAT, usage_rw);
    indirect_radiance_txs_[i].acquire_2d(
        target_extent, gpu::TextureFormat::RAYTRACE_RADIANCE_FORMAT, usage_rw);
  }

  prepass_fb.bind();
  prepass_fb.clear_depth(inst_.film.depth.clear_value);
  prepass_fb.clear_color(float4(0.0f));
  opaque_layer_.prepass_.render(view, inst_.render_buffers.depth_tx, true);

  inst_.hiz_buffer.set_source(&inst_.render_buffers.depth_tx);
  inst_.hiz_buffer.update();

  inst_.shadows.set_view(view, extent, TelemetryShadowContext::CaptureProbe);
  inst_.shadows.render(view, extent);
  inst_.volume_probes.set_view(view);
  inst_.sphere_probes.set_view(view);
  inst_.lights.eval_uniform_light_shaders(view);
  inst_.lights.eval_front_light_shaders(view, extent);

  /* Update for lighting pass. */
  inst_.hiz_buffer.update();

  inst_.gbuffer.bind(gbuffer_fb);
  inst_.manager->submit(opaque_layer_.gbuffer_ps_, view);
  inst_.lights.eval_light_shaders(view, extent);

  combined_fb.bind();
  inst_.manager->submit(eval_light_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  opaque_layer_.npr_aov_color_input_tx_ = inst_.render_buffers.rp_color_tx;
  opaque_layer_.npr_aov_value_input_tx_ = inst_.render_buffers.rp_value_tx;

  TextureFromPool npr_radiance_input = {"NPR Radiance Input"};
  {
    npr_radiance_input.acquire_2d(
        extent,
        GPU_texture_format(combined_tx),
        GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
    npr_radiance_input_tx_ = npr_radiance_input;
    Framebuffer npr_radiance_fb;
    npr_radiance_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(npr_radiance_input_tx_));
    GPU_framebuffer_blit(combined_fb, 0, npr_radiance_fb, 0, GPU_COLOR_BIT);
  }

  inst_.manager->submit(opaque_layer_.npr_ps_, view);

  opaque_layer_.npr_aov_color_input_tx_ = nullptr;
  opaque_layer_.npr_aov_value_input_tx_ = nullptr;
  npr_radiance_input_tx_ = nullptr;
  npr_radiance_input.release();
  for (int i = 0; i < ARRAY_SIZE(direct_radiance_txs_); i++) {
    direct_radiance_txs_[i].release();
    indirect_radiance_txs_[i].release();
  }

  GPU_debug_group_end();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Deferred Planar Probe Pipeline
 *
 * \{ */

void PlanarProbePipeline::begin_sync()
{
  prepass_.init(DRW_STATE_NO_DRAW, false, true, [&](PassMain &pass) {
    pass.clear_stencil(0x00u);
    pass.bind_ubo(CLIP_PLANE_BUF, inst_.planar_probes.world_clip_buf_);
    pass.bind_resources(inst_.render_textures);
  });

  this->gbuffer_pass_sync(inst_);
  this->npr_pass_sync(inst_, [&]() {
    npr_ps_.bind_texture(NPR_RADIANCE_TEX_SLOT, &npr_radiance_input_tx_);
    for (int i : IndexRange(3)) {
      npr_ps_.bind_texture(DIRECT_RADIANCE_NPR_TX_SLOT_1 + i, &direct_radiance_txs_[i]);
      npr_ps_.bind_texture(INDIRECT_RADIANCE_NPR_TX_SLOT_1 + i, &indirect_radiance_txs_[i]);
    }
    npr_ps_.bind_texture(BACK_RADIANCE_TX_SLOT, &dummy_black_);
    npr_ps_.bind_texture(BACK_HIZ_TX_SLOT, &dummy_black_);
  });

  closure_bits_ = CLOSURE_NONE;
  closure_count_ = 0;
  use_depth_offset_lighting_data_ = false;
}

void PlanarProbePipeline::end_sync()
{
  prepass_.end_sync();
  if (!gbuffer_ps_.is_empty()) {
    PassSimple &pass = eval_light_ps_;
    pass.init();
    pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ADD_FULL | DRW_STATE_DEPTH_GREATER);
    pass.shader_set(inst_.shaders.static_shader_get(DEFERRED_PLANAR_EVAL));
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.gbuffer);
    pass.bind_resources(inst_.lights);
    inst_.lights.bind_light_shader_resources(pass);
    pass.bind_resources(inst_.shadows);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.render_textures);
    pass.bind_resources(inst_.hiz_buffer.front);
    pass.bind_resources(inst_.sphere_probes);
    pass.bind_resources(inst_.volume_probes);
    pass.bind_image("direct_radiance_1_img", &direct_radiance_txs_[0]);
    pass.bind_image("direct_radiance_2_img", &direct_radiance_txs_[1]);
    pass.bind_image("direct_radiance_3_img", &direct_radiance_txs_[2]);
    pass.bind_image("indirect_radiance_1_img", &indirect_radiance_txs_[0]);
    pass.bind_image("indirect_radiance_2_img", &indirect_radiance_txs_[1]);
    pass.bind_image("indirect_radiance_3_img", &indirect_radiance_txs_[2]);
    pass.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
    pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }
}

PassMain::Sub *PlanarProbePipeline::prepass_add(blender::Material *blender_mat,
                                                GPUMaterial *gpumat,
                                                bool hide_from_raycast,
                                                bool force_write_id)
{
  return prepass_.add(blender_mat, gpumat, false, hide_from_raycast, force_write_id);
}

PassMain::Sub *PlanarProbePipeline::material_add(blender::Material *blender_mat,
                                                 GPUMaterial *gpumat)
{
  eClosureBits closure_bits = shader_closure_bits_from_flag(gpumat);
  if (closure_bits == eClosureBits(0)) {
    /* Fix the case where there is no active closure in the shader.
     * In this case we force the evaluation of emission to avoid disabling the entire layer by
     * accident, see #126459. */
    closure_bits |= CLOSURE_EMISSION;
  }
  closure_bits_ |= closure_bits;
  closure_count_ = max_ii(closure_count_, count_bits_i(closure_bits));
  use_depth_offset_lighting_data_ |= material_uses_depth_offset_lighting_data(blender_mat, gpumat);

  const bool needs_front_light_shader = material_needs_front_light_shader_resources(gpumat,
                                                                                   closure_bits);
  if (needs_front_light_shader) {
    inst_.lights.tag_front_light_shader_needed();
  }

  PassMain::Sub *pass = get_gbuffer_subpass(blender_mat, gpumat);
  PassMain::Sub *material_pass = &pass->sub(GPU_material_get_name(gpumat));
  if (needs_front_light_shader) {
    material_pass->bind_resources(inst_.lights);
    inst_.lights.bind_front_light_shader_resources(*material_pass);
    material_pass->bind_resources(inst_.shadows);
  }
  if (material_needs_lightprobe_resources(gpumat)) {
    material_pass->bind_resources(inst_.sphere_probes);
    material_pass->bind_resources(inst_.volume_probes);
  }
  return material_pass;
}

PassMain::Sub *PlanarProbePipeline::npr_add(blender::Material *blender_mat, GPUMaterial *gpumat)
{
  PassMain::Sub *pass = material_surface_cull_pass_get(
      npr_double_sided_ps_, npr_single_sided_ps_, npr_front_cull_ps_, blender_mat);
  use_depth_offset_lighting_data_ |= material_uses_depth_offset_lighting_data(blender_mat, gpumat);
  if (GPU_material_flag_get(gpumat, GPU_MATFLAG_SHADER_INFO) ||
      GPU_material_has_glsl_light_shader_eval(gpumat) ||
      GPU_material_flag_get(gpumat, GPU_MATFLAG_GLSL_LIGHT_ACCESS))
  {
    inst_.lights.tag_front_light_shader_needed();
  }

  PassMain::Sub *material_ps = &pass->sub(GPU_material_get_name(gpumat));
  inst_.lights.bind_npr_front_light_shader_resources(*material_ps);
  GPUPass *gpupass = GPU_material_get_pass(gpumat);
  material_ps->shader_set(GPU_pass_shader_get(gpupass));
  material_ps->push_constant("use_split_radiance", true);
  material_ps->push_constant("use_radiance_input_for_combined", false);

  return material_ps;
}

void PlanarProbePipeline::render(View &view,
                                 gpu::Texture *depth_layer_tx,
                                 Framebuffer &prepass_fb,
                                 Framebuffer &gbuffer_fb,
                                 Framebuffer &combined_fb,
                                 int2 extent,
                                 gpu::Texture *combined_tx)
{
  GPU_debug_group_begin("Planar.Capture");

  radiance_behind_tx_ = dummy_black_;

  const eGPUTextureUsage usage_rw = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  for (int i = 0; i < ARRAY_SIZE(direct_radiance_txs_); i++) {
    const int2 target_extent = (closure_count_ > i) ? extent : int2(1);
    direct_radiance_txs_[i].acquire_2d(
        target_extent, gpu::TextureFormat::DEFERRED_RADIANCE_FORMAT, usage_rw);
    indirect_radiance_txs_[i].acquire_2d(
        target_extent, gpu::TextureFormat::RAYTRACE_RADIANCE_FORMAT, usage_rw);
  }

  inst_.pipelines.data.ray_type = RAY_TYPE_GLOSSY;
  inst_.uniform_data.pipeline.push_update();
  inst_.lights.set_view(view, extent);

  GPU_framebuffer_bind(prepass_fb);
  GPU_framebuffer_clear_depth(prepass_fb, inst_.film.depth.clear_value);
  prepass_.render(view, depth_layer_tx, true);

  /* TODO(fclem): This is the only place where we use the layer source to HiZ.
   * This is because the texture layer view is still a layer texture. */
  inst_.hiz_buffer.set_source(&depth_layer_tx, 0);
  inst_.hiz_buffer.update();

  inst_.shadows.set_view(view, extent, TelemetryShadowContext::PlanarProbe);
  inst_.shadows.render(view, extent);
  inst_.volume_probes.set_view(view);
  inst_.sphere_probes.set_view(view);
  inst_.lights.eval_uniform_light_shaders(view);
  inst_.lights.eval_front_light_shaders(view, extent);

  /* Clear before the GBuffer pass so direct surface radiance written there survives capture.
   * The GBuffer color attachment is shared with the planar radiance target. */
  GPU_framebuffer_bind(combined_fb);
  GPU_framebuffer_clear_color(combined_fb, double4(0.0));

  inst_.gbuffer.bind(gbuffer_fb, true);
  inst_.manager->submit(gbuffer_ps_, view);
  inst_.lights.eval_light_shaders(view, extent);

  GPU_framebuffer_bind(combined_fb);
  inst_.manager->submit(eval_light_ps_, view);
  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  inst_.pipelines.data.ray_type = RAY_TYPE_CAMERA;
  inst_.uniform_data.pipeline.push_update();

  inst_.pipelines.background.render(view, combined_fb);

  npr_aov_color_input_tx_ = inst_.render_buffers.rp_color_tx;
  npr_aov_value_input_tx_ = inst_.render_buffers.rp_value_tx;

  TextureFromPool npr_radiance_input = {"NPR Radiance Input"};
  {
    npr_radiance_input.acquire_2d(
        extent,
        GPU_texture_format(combined_tx),
        GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
    npr_radiance_input_tx_ = npr_radiance_input;
    Framebuffer npr_radiance_fb;
    npr_radiance_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(npr_radiance_input_tx_));
    GPU_framebuffer_blit(combined_fb, 0, npr_radiance_fb, 0, GPU_COLOR_BIT);
  }

  inst_.manager->submit(npr_ps_, view);

  npr_aov_color_input_tx_ = nullptr;
  npr_aov_value_input_tx_ = nullptr;
  npr_radiance_input_tx_ = nullptr;
  npr_radiance_input.release();
  for (int i = 0; i < ARRAY_SIZE(direct_radiance_txs_); i++) {
    direct_radiance_txs_[i].release();
    indirect_radiance_txs_[i].release();
  }

  GPU_debug_group_end();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Capture Pipeline
 *
 * \{ */

void CapturePipeline::sync()
{
  surface_ps_.init();
  /* Surfel output is done using a SSBO, so no need for a fragment shader output color or depth. */
  /* WORKAROUND: Avoid rasterizer discard, but the shaders actually use no fragment output. */
  surface_ps_.state_set(DRW_STATE_WRITE_STENCIL);
  surface_ps_.framebuffer_set(&inst_.volume_probes.bake.empty_raster_fb_);

  surface_ps_.bind_ssbo(SURFEL_BUF_SLOT, &inst_.volume_probes.bake.surfels_buf_);
  surface_ps_.bind_ssbo(CAPTURE_BUF_SLOT, &inst_.volume_probes.bake.capture_info_buf_);

  surface_ps_.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
  /* TODO(fclem): Remove. Bind to get the camera data,
   * but there should be no view dependent behavior during capture. */
  surface_ps_.bind_resources(inst_.uniform_data);
  surface_ps_.bind_resources(inst_.sampling);
  surface_ps_.bind_resources(inst_.render_textures);
}

PassMain::Sub *CapturePipeline::surface_material_add(blender::Material *blender_mat,
                                                     GPUMaterial *gpumat)
{
  PassMain::Sub &sub_pass = surface_ps_.sub(GPU_material_get_name(gpumat));
  GPUPass *gpupass = GPU_material_get_pass(gpumat);
  sub_pass.shader_set(GPU_pass_shader_get(gpupass));
  sub_pass.push_constant("is_double_sided",
                         bool(blender_mat->blend_flag & MA_BL_LIGHTPROBE_VOLUME_DOUBLE_SIDED));
  return &sub_pass;
}

void CapturePipeline::render(View &view)
{
  inst_.manager->submit(surface_ps_, view);
}

/** \} */

}  // namespace blender::eevee
