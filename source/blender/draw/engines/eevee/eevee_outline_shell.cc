/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GPU_capabilities.hh"
#include "GPU_material.hh"

#include "eevee_instance.hh"
#include "eevee_outline_shell.hh"

namespace blender::eevee {

void OutlineShellModule::begin_sync()
{
  object_layers_.clear();
  object_count_ = 0;
  has_objects_ = false;
  layers_dirty_ = true;
  layers_.as_span().fill(0);
}

PassMain::Sub *OutlineShellModule::material_add(ResourceHandle handle, GPUMaterial *gpumat)
{
  has_objects_ = true;
  const int64_t previous_size = layers_.size();
  layers_.get_or_resize(handle.index());
  layers_.as_span().drop_front(previous_size).fill(0);
  if (gpumat == nullptr) {
    return nullptr;
  }
  int layer = object_layers_.lookup_or_add_cb(handle.index(), [&]() {
    const int index = object_count_++;
    if (index == objects_.size()) {
      objects_.append(std::make_unique<ObjectDepth>());
    }
    PassMain &pass = objects_[index]->pass;
    pass.init();
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.render_textures);
    pass.bind_resources(inst_.lights);
    pass.bind_resources(inst_.shadows);
    pass.bind_resources(inst_.hiz_buffer.front);
    return index;
  });

  layers_[handle.index()] = layer + 1;

  blender::Material *mat = GPU_material_get_material(gpumat);
  PassMain::Sub &pass = objects_[layer]->pass.sub(GPU_material_get_name(gpumat));
  const eMaterialCullMethod cull = blender::material_surface_cull_method_get(*mat);
  const DRWState cull_state = cull == MA_SURFACE_CULL_BACK  ? DRW_STATE_CULL_BACK :
                              cull == MA_SURFACE_CULL_FRONT ? DRW_STATE_CULL_FRONT :
                                                              DRW_STATE_NO_DRAW;
  pass.state_set(DRW_STATE_WRITE_DEPTH | DRW_STATE_CLIP_CONTROL_UNIT_RANGE |
                 inst_.film.depth.test_state | cull_state);
  pass.material_set(*inst_.manager, gpumat, true, inst_.anisotropic_filtering);
  return &pass;
}

void OutlineShellModule::render(View &view, int2 extent)
{
  if (!has_objects_) {
    depth_tx_.release();
    objects_.clear();
    layers_.trim_to_next_power_of_2(1);
    return;
  }

  const int layer_count = std::min(object_count_, GPU_max_texture_layers());
  depth_tx_.acquire_2d_array(layer_count > 0 ? extent : int2(1),
                             std::max(layer_count, 1),
                             gpu::TextureFormat::SFLOAT_32_DEPTH,
                             GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ);
  /* An unavailable layer must leave the original shell intact. */
  for (const auto item : object_layers_.items()) {
    if (item.value >= layer_count) {
      layers_[item.key] = 0;
    }
  }
  if (layer_count < object_count_) {
    inst_.info_append("Outline Shell: self-occlusion object count exceeds GPU texture layers");
  }
  if (layers_dirty_) {
    layers_.push_update();
    layers_dirty_ = false;
  }

  GPU_debug_group_begin("OutlineShell.SelfDepth");
  objects_.resize(object_count_);
  for (const int i : IndexRange(layer_count)) {
    depth_fb_.ensure(GPU_ATTACHMENT_TEXTURE_LAYER(depth_tx_, i));
    depth_fb_.bind();
    depth_fb_.clear_depth(inst_.film.depth.clear_value);
    inst_.manager->submit(objects_[i]->pass, view);
  }
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
  GPU_debug_group_end();
}

}  // namespace blender::eevee
