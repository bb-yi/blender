/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 */

#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include <cstring>

#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"
#include "BLI_set.hh"

#include "BKE_collection.hh"
#include "BKE_cryptomatte.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node.hh"

#include "DEG_depsgraph_query.hh"

#include "GPU_material.hh"
#include "GPU_framebuffer.hh"
#include "GPU_texture.hh"

#include "MEM_guardedalloc.h"

#include "eevee_filter_material.hh"
#include "eevee_instance.hh"

namespace blender::eevee {

static FilterObjectInfoData filter_object_info_default()
{
  FilterObjectInfoData data;
  data.location = float4(0.0f);
  data.rotation = float4(0.0f);
  data.scale = float4(1.0f, 1.0f, 1.0f, 0.0f);
  data.color = float4(0.0f);
  data.metadata = float4(0.0f);
  return data;
}

static bool filter_material_is_valid(blender::Material *material)
{
  return material != nullptr && material->eevee_domain == MA_EEVEE_DOMAIN_FILTER &&
         material->nodetree != nullptr;
}

static bool filter_mask_object_supported(const Object *object)
{
  return object != nullptr && OB_TYPE_IS_GEOMETRY(object->type);
}

static bool filter_mask_object_in_view_layer(Depsgraph *depsgraph,
                                             const Scene *scene,
                                             ViewLayer *view_layer,
                                             Object *object)
{
  if (!filter_mask_object_supported(object)) {
    return false;
  }

  const Scene *scene_orig = DEG_get_original(scene);
  if (scene_orig != nullptr && !BKE_scene_has_object(const_cast<Scene *>(scene_orig), object)) {
    return false;
  }

  Object *object_eval = (depsgraph != nullptr) ? DEG_get_evaluated(depsgraph, object) : nullptr;
  return BKE_view_layer_base_find(view_layer, object_eval ? object_eval : object) != nullptr;
}

static int16_t filter_mask_objects_signature(Span<Object *> objects)
{
  uint32_t hash = 2166136261u;
  for (const Object *object : objects) {
    hash ^= BLI_hash_string(object->id.name + 2);
    hash *= 16777619u;
  }
  return int16_t(hash & 0x7FFFu);
}

static bool filter_mask_sanitize_object_list(NodeFilterMask &storage,
                                             const Scene *scene,
                                             Depsgraph *depsgraph,
                                             ViewLayer *view_layer,
                                             Vector<Object *> &r_objects)
{
  Set<Object *> unique_objects;
  Vector<Object *> valid_objects;
  valid_objects.reserve(storage.items_num);

  for (const int index : IndexRange(storage.items_num)) {
    Object *object = storage.items[index].object;
    if (!filter_mask_object_in_view_layer(depsgraph, scene, view_layer, object) ||
        !unique_objects.add(object))
    {
      continue;
    }
    valid_objects.append(object);
  }

  bool changed = (valid_objects.size() != storage.items_num);
  if (!changed) {
    for (const int index : valid_objects.index_range()) {
      if (storage.items[index].object != valid_objects[index]) {
        changed = true;
        break;
      }
    }
  }

  if (changed) {
    Set<Object *> kept_objects;
    for (Object *object : valid_objects) {
      kept_objects.add(object);
    }
    for (const int index : IndexRange(storage.items_num)) {
      Object *object = storage.items[index].object;
      if (object != nullptr && !kept_objects.contains(object) &&
          BKE_id_is_in_global_main(reinterpret_cast<ID *>(object)))
      {
        id_us_min(reinterpret_cast<ID *>(object));
      }
    }
    if (storage.items != nullptr) {
      MEM_delete(storage.items);
      storage.items = nullptr;
    }
    if (!valid_objects.is_empty()) {
      storage.items = MEM_new_array<NodeFilterMaskItem>(valid_objects.size(), __func__);
      for (const int index : valid_objects.index_range()) {
        storage.items[index].object = valid_objects[index];
      }
    }
    storage.items_num = valid_objects.size();
    storage.active_index = valid_objects.is_empty() ? 0 :
                                                   min_ii(storage.active_index,
                                                          valid_objects.size() - 1);
  }

  r_objects = std::move(valid_objects);
  return changed;
}

static Vector<Object *> filter_mask_collection_objects(const Scene *scene,
                                                       Depsgraph *depsgraph,
                                                       ViewLayer *view_layer,
                                                       Collection *collection)
{
  Vector<Object *> objects;
  if (collection == nullptr) {
    return objects;
  }

  Set<Object *> unique_objects;
  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, object) {
    if (!filter_mask_object_in_view_layer(depsgraph, scene, view_layer, object) ||
        !unique_objects.add(object))
    {
      continue;
    }
    objects.append(object);
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;

  return objects;
}

static bool filter_mask_sanitize_tree(bNodeTree &ntree,
                                      const Scene *scene,
                                      Depsgraph *depsgraph,
                                      ViewLayer *view_layer,
                                      Set<const bNodeTree *> &visited)
{
  if (visited.contains(&ntree)) {
    return false;
  }
  visited.add(&ntree);

  bool changed = false;
  for (bNode *node = static_cast<bNode *>(ntree.nodes.first); node != nullptr; node = node->next) {
    if (node->type_legacy == SH_NODE_FILTER_OBJECT_MASK) {
      const NodeFilterMaskMode mode = NodeFilterMaskMode(node->custom1);
      if (mode == SHD_FILTER_MASK_SINGLE_OBJECT) {
        if (Object *object = (node->id != nullptr && GS(node->id->name) == ID_OB) ?
                                 reinterpret_cast<Object *>(node->id) :
                                 nullptr)
        {
          if (!filter_mask_object_in_view_layer(depsgraph, scene, view_layer, object)) {
            id_us_min(node->id);
            node->id = nullptr;
            changed = true;
          }
        }
        if (node->custom2 != 0) {
          node->custom2 = 0;
          changed = true;
        }
      }
      else if (mode == SHD_FILTER_MASK_OBJECT_LIST) {
        if (node->storage != nullptr) {
          NodeFilterMask &storage = *static_cast<NodeFilterMask *>(node->storage);
          Vector<Object *> valid_objects;
          changed |= filter_mask_sanitize_object_list(
              storage, scene, depsgraph, view_layer, valid_objects);
          const int16_t signature = filter_mask_objects_signature(valid_objects);
          if (node->custom2 != signature) {
            node->custom2 = signature;
            changed = true;
          }
        }
      }
      else if (mode == SHD_FILTER_MASK_COLLECTION) {
        Collection *collection = (node->id != nullptr && GS(node->id->name) == ID_GR) ?
                                     reinterpret_cast<Collection *>(node->id) :
                                     nullptr;
        const Vector<Object *> objects = filter_mask_collection_objects(
            scene, depsgraph, view_layer, collection);
        const int16_t signature = filter_mask_objects_signature(objects);
        if (node->custom2 != signature) {
          node->custom2 = signature;
          changed = true;
        }
      }
    }

    if (node->type_legacy == NODE_GROUP && node->id != nullptr) {
      changed |= filter_mask_sanitize_tree(
          *reinterpret_cast<bNodeTree *>(node->id), scene, depsgraph, view_layer, visited);
    }
  }

  return changed;
}

static bool filter_mask_tree_has_collection_mode(const bNodeTree &ntree,
                                                 Set<const bNodeTree *> &visited)
{
  if (visited.contains(&ntree)) {
    return false;
  }
  visited.add(&ntree);

  for (const bNode *node = static_cast<const bNode *>(ntree.nodes.first); node != nullptr;
       node = node->next)
  {
    if (node->type_legacy == SH_NODE_FILTER_OBJECT_MASK &&
        NodeFilterMaskMode(node->custom1) == SHD_FILTER_MASK_COLLECTION)
    {
      return true;
    }
    if (node->type_legacy == NODE_GROUP && node->id != nullptr) {
      if (filter_mask_tree_has_collection_mode(
              *reinterpret_cast<const bNodeTree *>(node->id), visited))
      {
        return true;
      }
    }
  }

  return false;
}

static void filter_material_collect_scene_sources(const bNodeTree &ntree,
                                                  Set<const bNodeTree *> &visited,
                                                  bool &r_uses_scene_depth,
                                                  bool &r_uses_scene_normal,
                                                  bool &r_uses_scene_position,
                                                  bool &r_uses_cryptomatte_object)
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
    }
    else if (node->type_legacy == SH_NODE_FILTER_OBJECT_MASK) {
      r_uses_cryptomatte_object = true;
    }
    if (r_uses_scene_depth && r_uses_scene_normal && r_uses_scene_position &&
        r_uses_cryptomatte_object)
    {
      return;
    }
    if (node->type_legacy == NODE_GROUP && node->id != nullptr) {
      filter_material_collect_scene_sources(*reinterpret_cast<bNodeTree *>(node->id),
                                            visited,
                                            r_uses_scene_depth,
                                            r_uses_scene_normal,
                                            r_uses_scene_position,
                                            r_uses_cryptomatte_object);
      if (r_uses_scene_depth && r_uses_scene_normal && r_uses_scene_position &&
          r_uses_cryptomatte_object)
      {
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
  uses_cryptomatte_object_ = false;

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
                                          uses_scene_position_,
                                          uses_cryptomatte_object_);
    if (uses_scene_depth_ && uses_scene_normal_ && uses_scene_position_ &&
        uses_cryptomatte_object_)
    {
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

void FilterMaterialModule::update_filter_object_info_buffer(GPUMaterial *gpumat)
{
  for (FilterObjectInfoData &entry : filter_object_info_buf_) {
    entry = filter_object_info_default();
  }

  const int material_object_count = GPU_material_filter_object_info_count(gpumat);
  const int object_count = (material_object_count < FILTER_OBJECT_INFO_MAX) ?
                               material_object_count :
                               FILTER_OBJECT_INFO_MAX;
  for (int index = 0; index < object_count; index++) {
    Object *object = GPU_material_filter_object_info_get(gpumat, index);
    if (object == nullptr) {
      continue;
    }

    Object *object_eval = DEG_get_evaluated(inst_.depsgraph, object);
    const Object *runtime_object = (object_eval != nullptr) ? object_eval : object;

    float3 location;
    math::EulerXYZ rotation;
    float3 scale;
    math::to_loc_rot_scale<true>(runtime_object->object_to_world(), location, rotation, scale);
    const float3 rotation_value = float3(rotation);

    FilterObjectInfoData &entry = filter_object_info_buf_[index];
    entry.location = float4(location[0], location[1], location[2], 0.0f);
    entry.rotation = float4(rotation_value[0], rotation_value[1], rotation_value[2], 0.0f);
    entry.scale = float4(scale[0], scale[1], scale[2], 0.0f);
    entry.color = float4(runtime_object->color[0],
                         runtime_object->color[1],
                         runtime_object->color[2],
                         runtime_object->color[3]);
    const char *name = object->id.name + 2;
    const uint32_t hash = BKE_cryptomatte_hash(name, int(std::strlen(name)));
    entry.metadata = float4(BKE_cryptomatte_hash_to_float(hash), 0.0f, 0.0f, 0.0f);
  }

  filter_object_info_buf_.push_update();
}

void FilterMaterialModule::begin_sync()
{
  entries_.clear();
  uses_scene_time_ = false;
  BKE_view_layer_synced_ensure(inst_.scene, inst_.view_layer);

  for (SceneFilterMaterial *filter_entry = static_cast<SceneFilterMaterial *>(
           inst_.scene->eevee.filter_materials.first);
       filter_entry != nullptr;
       filter_entry = filter_entry->next)
  {
    if (!filter_entry->enabled || !filter_material_is_valid(filter_entry->material)) {
      continue;
    }

    Set<const bNodeTree *> visited;
    const bool tree_changed = filter_mask_sanitize_tree(
        *filter_entry->material->nodetree, inst_.scene, inst_.depsgraph, inst_.view_layer, visited);
    visited.clear();
    const bool has_collection_mode = filter_mask_tree_has_collection_mode(
        *filter_entry->material->nodetree, visited);
    if (tree_changed || has_collection_mode)
    {
      GPU_material_free(&filter_entry->material->gpumaterial);
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

    uses_scene_time_ |= GPU_material_is_time_dependent(gpumat);
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
    update_filter_object_info_buffer(entries_[entry_index].gpumat);
    pass.material_set(*inst_.manager, entries_[entry_index].gpumat);
    pass.bind_texture("scene_color_tx", &scene_color_tx);
    pass.bind_texture("rp_color_tx", &inst_.render_buffers.rp_color_tx);
    pass.bind_texture("rp_value_tx", &inst_.render_buffers.rp_value_tx);
    pass.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
    pass.bind_texture("cryptomatte_tx", &inst_.render_buffers.cryptomatte_tx);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_ubo(FILTER_OBJECT_INFO_BUF_SLOT, &filter_object_info_buf_);
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
