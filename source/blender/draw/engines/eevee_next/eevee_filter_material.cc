/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "DNA_material_types.h"

#include "GPU_framebuffer.hh"
#include "GPU_texture.hh"

#include "eevee_filter_material.hh"
#include "eevee_instance.hh"

namespace blender::eevee {

void FilterMaterialModule::begin_sync()
{
  material_ = nullptr;
  gpumat_ = nullptr;
  scene_color_tx_ = nullptr;

  pass_.init();

  if (!inst_.scene->eevee.use_filter_material || inst_.scene->eevee.filter_material == nullptr) {
    return;
  }

  material_ = inst_.scene->eevee.filter_material;
  if (material_ == nullptr || material_->eevee_domain != MA_EEVEE_DOMAIN_FILTER ||
      !material_->use_nodes || material_->nodetree == nullptr)
  {
    material_ = nullptr;
    return;
  }

  gpumat_ = inst_.shaders.material_shader_get(
      material_, material_->nodetree, MAT_PIPE_FILTER, MAT_GEOM_WORLD, false);
  if (gpumat_ == nullptr || GPU_material_status(gpumat_) != GPU_MAT_SUCCESS ||
      !GPU_material_has_filter_output(gpumat_))
  {
    gpumat_ = nullptr;
    return;
  }

  inst_.manager->register_layer_attributes(gpumat_);

  pass_.state_set(DRW_STATE_WRITE_COLOR);
  pass_.framebuffer_set(&framebuffer_);
  pass_.material_set(*inst_.manager, gpumat_);
  pass_.bind_texture("scene_color_tx", &scene_color_tx_);
  pass_.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
  pass_.bind_resources(inst_.uniform_data);
  pass_.bind_resources(inst_.sampling);
  pass_.bind_resources(inst_.render_textures);
  pass_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
}

GPUTexture *FilterMaterialModule::render(View &view, GPUTexture *input_tx, int2 extent)
{
  if (gpumat_ == nullptr || input_tx == nullptr) {
    return input_tx;
  }

  scene_color_tx_ = input_tx;
  output_tx_.ensure_2d(GPU_texture_format(input_tx), extent, GPU_TEXTURE_USAGE_GENERAL);
  framebuffer_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(output_tx_));

  inst_.manager->submit(pass_, view);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
  return output_tx_;
}

}  // namespace blender::eevee
