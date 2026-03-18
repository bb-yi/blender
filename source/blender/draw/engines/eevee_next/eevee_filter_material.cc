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

static bool filter_material_is_valid(::Material *material)
{
  return material != nullptr && material->eevee_domain == MA_EEVEE_DOMAIN_FILTER &&
         material->use_nodes && material->nodetree != nullptr;
}

static void filter_material_collect_scene_sources(const bNodeTree &ntree,
                                                  Set<const bNodeTree *> &visited,
                                                  bool &r_uses_scene_depth,
                                                  bool &r_uses_scene_normal)
{
  if (visited.contains(&ntree)) {
    return;
  }
  visited.add(&ntree);

  LISTBASE_FOREACH (bNode *, node, &ntree.nodes) {
    if (node->type_legacy == SH_NODE_SCENE_COLOR) {
      const int source = node->custom1;
      r_uses_scene_depth |= (source == SHD_SCENE_SOURCE_DEPTH);
      r_uses_scene_normal |= (source == SHD_SCENE_SOURCE_NORMAL);
      if (r_uses_scene_depth && r_uses_scene_normal) {
        return;
      }
    }
    if (node->type_legacy == NODE_GROUP && node->id != nullptr) {
      filter_material_collect_scene_sources(
          *reinterpret_cast<bNodeTree *>(node->id), visited, r_uses_scene_depth, r_uses_scene_normal);
      if (r_uses_scene_depth && r_uses_scene_normal) {
        return;
      }
    }
  }
}

void FilterMaterialModule::init()
{
  uses_scene_depth_ = false;
  uses_scene_normal_ = false;

  Set<const bNodeTree *> visited;
  LISTBASE_FOREACH (SceneFilterMaterial *, filter_entry, &inst_.scene->eevee.filter_materials) {
    if (!filter_entry->enabled || !filter_material_is_valid(filter_entry->material)) {
      continue;
    }
    filter_material_collect_scene_sources(*filter_entry->material->nodetree,
                                          visited,
                                          uses_scene_depth_,
                                          uses_scene_normal_);
    if (uses_scene_depth_ && uses_scene_normal_) {
      break;
    }
  }
}

void FilterMaterialModule::begin_sync()
{
  entries_.clear();

  LISTBASE_FOREACH (SceneFilterMaterial *, filter_entry, &inst_.scene->eevee.filter_materials) {
    if (!filter_entry->enabled || !filter_material_is_valid(filter_entry->material)) {
      continue;
    }

    GPUMaterial *gpumat = inst_.shaders.material_shader_get(
        filter_entry->material, filter_entry->material->nodetree, MAT_PIPE_FILTER, MAT_GEOM_WORLD, false);
    if (gpumat == nullptr || GPU_material_status(gpumat) != GPU_MAT_SUCCESS ||
        !GPU_material_has_filter_output(gpumat))
    {
      continue;
    }

    inst_.manager->register_layer_attributes(gpumat);
    entries_.append({filter_entry, filter_entry->material, gpumat});
  }
}

GPUTexture *FilterMaterialModule::render(View &view, GPUTexture *input_tx, int2 extent)
{
  if (entries_.is_empty() || input_tx == nullptr) {
    return input_tx;
  }

  ping_tx_.ensure_2d(GPU_texture_format(input_tx), extent, GPU_TEXTURE_USAGE_GENERAL);
  pong_tx_.ensure_2d(GPU_texture_format(input_tx), extent, GPU_TEXTURE_USAGE_GENERAL);

  GPUTexture *source_tx = input_tx;

  for (const int entry_index : entries_.index_range()) {
    Texture &target_tx = ((entry_index & 1) == 0) ? ping_tx_ : pong_tx_;
    GPUTexture *scene_color_tx = source_tx;

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
  }

  return source_tx;
}

}  // namespace blender::eevee
