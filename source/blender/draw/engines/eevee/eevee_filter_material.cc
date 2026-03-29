/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_set.hh"

#include "BKE_node_legacy_types.hh"
#include "BKE_node.hh"

#include "GPU_framebuffer.hh"
#include "GPU_texture.hh"

#include "eevee_filter_material.hh"
#include "eevee_instance.hh"

namespace blender::eevee {

static bool filter_material_is_valid(blender::Material *material)
{
  return material != nullptr && material->eevee_domain == MA_EEVEE_DOMAIN_FILTER &&
         material->nodetree != nullptr;
}

static void filter_material_collect_scene_sources(const bNodeTree &ntree,
                                                  Set<const bNodeTree *> &visited,
                                                  bool &r_uses_scene_depth,
                                                  bool &r_uses_scene_normal,
                                                  bool &r_uses_scene_position)
{
  if (visited.contains(&ntree)) {
    return;
  }
  visited.add(&ntree);

  for (bNode *node = static_cast<bNode *>(ntree.nodes.first); node != nullptr; node = node->next) {
    if (node->type_legacy == SH_NODE_SCENE_COLOR) {
      const int source = node->custom1;
      r_uses_scene_depth |= (source == SHD_SCENE_SOURCE_DEPTH);
      r_uses_scene_normal |= (source == SHD_SCENE_SOURCE_NORMAL);
      r_uses_scene_position |= (source == SHD_SCENE_SOURCE_POSITION);
      if (r_uses_scene_depth && r_uses_scene_normal && r_uses_scene_position) {
        return;
      }
    }
    if (node->type_legacy == NODE_GROUP && node->id != nullptr) {
      filter_material_collect_scene_sources(*reinterpret_cast<bNodeTree *>(node->id),
                                            visited,
                                            r_uses_scene_depth,
                                            r_uses_scene_normal,
                                            r_uses_scene_position);
      if (r_uses_scene_depth && r_uses_scene_normal && r_uses_scene_position) {
        return;
      }
    }
  }
}

void FilterMaterialModule::init()
{
  uses_scene_depth_ = false;
  uses_scene_normal_ = false;
  uses_scene_position_ = false;

  Set<const bNodeTree *> visited;
  for (SceneFilterMaterial *filter_entry = static_cast<SceneFilterMaterial *>(
           inst_.scene->eevee.filter_materials.first);
       filter_entry != nullptr;
       filter_entry = filter_entry->next)
  {
    if (!filter_entry->enabled || !filter_material_is_valid(filter_entry->material)) {
      continue;
    }
    filter_material_collect_scene_sources(*filter_entry->material->nodetree,
                                          visited,
                                          uses_scene_depth_,
                                          uses_scene_normal_,
                                          uses_scene_position_);
    if (uses_scene_depth_ && uses_scene_normal_ && uses_scene_position_) {
      break;
    }
  }
}

bool FilterMaterialModule::uses_aov() const
{
  for (const FilterPassEntry &entry : entries_) {
    if (entry.gpumat != nullptr && GPU_material_flag_get(entry.gpumat, GPU_MATFLAG_AOV)) {
      return true;
    }
  }
  return false;
}

void FilterMaterialModule::begin_sync()
{
  entries_.clear();

  for (SceneFilterMaterial *filter_entry = static_cast<SceneFilterMaterial *>(
           inst_.scene->eevee.filter_materials.first);
       filter_entry != nullptr;
       filter_entry = filter_entry->next)
  {
    if (!filter_entry->enabled || !filter_material_is_valid(filter_entry->material)) {
      continue;
    }

    GPUMaterial *gpumat = inst_.shaders.material_shader_get(filter_entry->material,
                                                            filter_entry->material->nodetree,
                                                            MAT_PIPE_FILTER,
                                                            MAT_GEOM_WORLD,
                                                            MAT_PROBE_NONE,
                                                            false,
                                                            nullptr);
    const int status = (gpumat != nullptr) ? GPU_material_status(gpumat) : -1;
    const int has_filter_output = (gpumat != nullptr) ?
                                      int(GPU_material_has_filter_output(gpumat)) :
                                      0;
    if (gpumat == nullptr || status != GPU_MAT_SUCCESS || !has_filter_output) {
      continue;
    }

    inst_.manager->register_layer_attributes(gpumat);
    FilterPassEntry entry;
    entry.scene_filter = filter_entry;
    entry.material = filter_entry->material;
    entry.gpumat = gpumat;
    entries_.append(entry);
  }

}

bool FilterMaterialModule::has_stage_entries(SceneEEVEEFilterExecutionStage stage) const
{
  for (const FilterPassEntry &entry : entries_) {
    if (entry.scene_filter != nullptr && entry.scene_filter->execution_stage == stage) {
      return true;
    }
  }
  return false;
}

gpu::Texture *FilterMaterialModule::render_stage(draw::View &view,
                                                 gpu::Texture *input_tx,
                                                 int2 extent,
                                                 SceneEEVEEFilterExecutionStage stage)
{
  if (entries_.is_empty() || input_tx == nullptr || !has_stage_entries(stage)) {
    return input_tx;
  }

  ping_tx_.ensure_2d(GPU_texture_format(input_tx), extent, GPU_TEXTURE_USAGE_GENERAL);
  pong_tx_.ensure_2d(GPU_texture_format(input_tx), extent, GPU_TEXTURE_USAGE_GENERAL);

  gpu::Texture *source_tx = input_tx;
  int stage_entry_index = 0;

  for (const int entry_index : entries_.index_range()) {
    if (entries_[entry_index].scene_filter == nullptr ||
        entries_[entry_index].scene_filter->execution_stage != stage)
    {
      continue;
    }

    Texture &target_tx = ((stage_entry_index & 1) == 0) ? ping_tx_ : pong_tx_;
    gpu::Texture *scene_color_tx = source_tx;

    framebuffer_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(target_tx));

    PassSimple pass = {"FilterMaterial.Pass"};
    pass.state_set(DRW_STATE_WRITE_COLOR);
    pass.framebuffer_set(&framebuffer_);
    pass.material_set(*inst_.manager, entries_[entry_index].gpumat);
    pass.bind_texture("scene_color_tx", &scene_color_tx);
    pass.bind_texture("rp_color_tx", &inst_.render_buffers.rp_color_tx);
    pass.bind_texture("rp_value_tx", &inst_.render_buffers.rp_value_tx);
    pass.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.render_textures);
    pass.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
    pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);

    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
    inst_.manager->submit(pass, view);
    GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);

    source_tx = target_tx;
    stage_entry_index++;
  }

  return source_tx;
}

}  // namespace blender::eevee
