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
#include "BLI_map.hh"
#include "BLI_math_matrix.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "BKE_collection.hh"
#include "BKE_cryptomatte.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node.hh"

#include "NOD_filter_graph.hh"

#include "DEG_depsgraph_query.hh"

#include "GPU_material.hh"
#include "GPU_framebuffer.hh"
#include "GPU_texture.hh"

#include "eevee_filter_material.hh"
#include "eevee_instance.hh"
#include "eevee_shader.hh"

#include <memory>
#include <vector>

namespace blender::eevee {

static constexpr uint32_t FILTER_TEX_HANDLE_NULL = 0u;
static constexpr uint32_t FILTER_TEX_HANDLE_RP_COLOR = 1u;
static constexpr uint32_t FILTER_TEX_HANDLE_RP_VALUE = 2u;
static constexpr uint32_t FILTER_TEX_HANDLE_SCENE = 30u;
static constexpr uint32_t FILTER_TEX_HANDLE_FILTER_GRAPH_INPUT = 31u;
static constexpr uint32_t FILTER_TEX_HANDLE_FILTER_GRAPH_TEXTURE = 32u;

static int filter_graph_scene_alpha_mode(const int index)
{
  if (index == 0) {
    return FILTER_GRAPH_ALPHA_MODE_TRANSMITTANCE;
  }
  if (index == 1) {
    return FILTER_GRAPH_ALPHA_MODE_DEPTH;
  }
  return FILTER_GRAPH_ALPHA_MODE_OPACITY;
}

struct FilterGraphImageHandle {
  uint32_t type = FILTER_TEX_HANDLE_NULL;
  int index = 0;
  gpu::Texture *texture = nullptr;
  int layer = 0;
  int alpha_mode = FILTER_GRAPH_ALPHA_MODE_OPACITY;

  static FilterGraphImageHandle null()
  {
    return {};
  }

  static FilterGraphImageHandle scene(const int index)
  {
    return {FILTER_TEX_HANDLE_SCENE, index, nullptr, 0, filter_graph_scene_alpha_mode(index)};
  }

  static FilterGraphImageHandle render_pass_color(const int index)
  {
    return {FILTER_TEX_HANDLE_RP_COLOR, index, nullptr, 0, FILTER_GRAPH_ALPHA_MODE_OPACITY};
  }

  static FilterGraphImageHandle render_pass_value(const int index)
  {
    return {FILTER_TEX_HANDLE_RP_VALUE, index, nullptr, 0, FILTER_GRAPH_ALPHA_MODE_OPACITY};
  }

  static FilterGraphImageHandle graph_texture(gpu::Texture *texture, const int layer = 0)
  {
    return {FILTER_TEX_HANDLE_FILTER_GRAPH_TEXTURE,
            0,
            texture,
            layer,
            FILTER_GRAPH_ALPHA_MODE_OPACITY};
  }

  bool is_texture() const
  {
    return type == FILTER_TEX_HANDLE_FILTER_GRAPH_TEXTURE && texture != nullptr;
  }
};

struct FilterGraphTextureInput {
  gpu::Texture *texture = nullptr;
  int layer = 0;
};

static bool filter_material_is_valid(blender::Material *material);
static blender::Material *filter_graph_node_material(const bNode &node);
static const bNode *filter_material_active_output_node(const bNodeTree &ntree, const int output_type);

static bool filter_graph_enabled(const Scene &scene)
{
  return scene.eevee.filter_graph != nullptr;
}

static const bNodeLink *filter_graph_socket_used_link(const bNodeSocket &socket)
{
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (link->is_used() && link->fromnode != nullptr && link->fromsock != nullptr) {
      return link;
    }
  }
  return nullptr;
}

static const bNode *filter_graph_stage_output_node_get(const bNodeTree &ntree,
                                                       const SceneEEVEEFilterExecutionStage stage)
{
  for (const bNode *node : ntree.all_nodes()) {
    if (node->type_legacy != EEVEE_FILTER_GRAPH_NODE_STAGE_OUTPUT || node->is_muted() ||
        node->custom1 != stage || !(node->flag & NODE_DO_OUTPUT))
    {
      continue;
    }
    return node;
  }
  return nullptr;
}

static bool filter_graph_collect_dependencies(const bNode &node,
                                              Set<const bNode *> &visiting,
                                              Set<const bNode *> &visited,
                                              Vector<const bNode *> &r_order)
{
  if (visited.contains(&node)) {
    return true;
  }
  if (visiting.contains(&node)) {
    return false;
  }
  visiting.add(&node);

  auto collect_input_socket = [&](const bNodeSocket *socket) -> bool {
    if (socket == nullptr) {
      return true;
    }
    const bNodeLink *link = filter_graph_socket_used_link(*socket);
    return link == nullptr || filter_graph_collect_dependencies(
                                  *link->fromnode, visiting, visited, r_order);
  };

  if (node.is_reroute()) {
    if (!collect_input_socket(static_cast<const bNodeSocket *>(node.inputs.first))) {
      return false;
    }
  }
  else if (node.type_legacy == EEVEE_FILTER_GRAPH_NODE_FILTER_MATERIAL) {
    const NodeEeveeFilterGraphFilterMaterial *storage =
        static_cast<const NodeEeveeFilterGraphFilterMaterial *>(node.storage);
    if (storage == nullptr || storage->items_num > FILTER_GRAPH_INPUT_MAX ||
        !filter_material_is_valid(filter_graph_node_material(node)))
    {
      return false;
    }
    for (const int i : IndexRange(storage->items_num)) {
      const std::string identifier = "Image_" + std::to_string(storage->items[i].identifier);
      if (!collect_input_socket(node.input_by_identifier(identifier))) {
        return false;
      }
    }
  }
  else if (!ELEM(node.type_legacy,
                EEVEE_FILTER_GRAPH_NODE_SCENE_COLOR,
                EEVEE_FILTER_GRAPH_NODE_AOV_INPUT))
  {
    return false;
  }

  visiting.remove(&node);
  visited.add(&node);
  r_order.append(&node);
  return true;
}

static bool filter_graph_stage_dependency_order_get(const bNodeTree &filter_graph,
                                                    const SceneEEVEEFilterExecutionStage stage,
                                                    Vector<const bNode *> &r_order)
{
  const bNode *stage_output = filter_graph_stage_output_node_get(filter_graph, stage);
  if (stage_output == nullptr) {
    return true;
  }
  const bNodeSocket *stage_input = stage_output->input_by_identifier("Image");
  const bNodeLink *stage_link = (stage_input != nullptr) ? filter_graph_socket_used_link(*stage_input) :
                                                          nullptr;
  if (stage_link == nullptr) {
    return true;
  }
  Set<const bNode *> visiting;
  Set<const bNode *> visited;
  return filter_graph_collect_dependencies(*stage_link->fromnode, visiting, visited, r_order);
}

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

static blender::Material *filter_graph_node_material(const bNode &node)
{
  if (node.id == nullptr || GS(node.id->name) != ID_MA) {
    return nullptr;
  }
  return reinterpret_cast<blender::Material *>(node.id);
}

static int filter_material_output_count(const blender::Material *material)
{
  if (!filter_material_is_valid(const_cast<blender::Material *>(material))) {
    return 0;
  }
  const bNode *output_node = filter_material_active_output_node(*material->nodetree,
                                                               SH_NODE_OUTPUT_FILTER);
  if (output_node == nullptr) {
    return 0;
  }
  if (output_node->storage == nullptr) {
    return 1;
  }
  const NodeShaderFilterOutput &storage =
      *static_cast<const NodeShaderFilterOutput *>(output_node->storage);
  return storage.items_num;
}

static int filter_material_output_index_from_socket(const blender::Material *material,
                                                    const bNodeSocket &socket)
{
  if (!filter_material_is_valid(const_cast<blender::Material *>(material))) {
    return -1;
  }
  const bNode *output_node = filter_material_active_output_node(*material->nodetree,
                                                               SH_NODE_OUTPUT_FILTER);
  if (output_node == nullptr) {
    return -1;
  }
  if (output_node->storage == nullptr) {
    return STREQ(socket.identifier, "Image") ? 0 : -1;
  }
  const NodeShaderFilterOutput &storage =
      *static_cast<const NodeShaderFilterOutput *>(output_node->storage);
  for (const int i : IndexRange(storage.items_num)) {
    const NodeEeveeFilterGraphSocketItem &item = storage.items[i];
    const std::string identifier = item.identifier == 0 ?
                                       "Image" :
                                       "Image_" + std::to_string(item.identifier);
    if (STREQ(socket.identifier, identifier.c_str())) {
      return i;
    }
  }
  return -1;
}

static int filter_graph_aov_index_get(const RenderBuffersInfoData &render_pass,
                                      const StringRef name,
                                      const bool is_value)
{
  if (name.is_empty()) {
    return -1;
  }

  const uint32_t hash = BLI_hash_string(name.data());
  const int start = is_value ? render_pass.aovs.color_len : 0;
  const int len = is_value ? render_pass.aovs.value_len : render_pass.aovs.color_len;
  for (int i = start; i < start + len; i++) {
    if (render_pass.aovs.hash[i / 4][i % 4] == hash) {
      return i - start;
    }
  }
  return -1;
}

static bool filter_mask_object_supported(const Object *object)
{
  return object != nullptr && OB_TYPE_IS_GEOMETRY(object->type);
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

static Vector<Object *> filter_mask_collection_objects(Collection *collection)
{
  Vector<Object *> objects;
  if (collection == nullptr) {
    return objects;
  }

  Set<Object *> unique_objects;
  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, object) {
    if (!filter_mask_object_supported(object) || !unique_objects.add(object)) {
      continue;
    }
    objects.append(object);
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;

  return objects;
}

static bool filter_mask_update_collection_signatures(bNodeTree &ntree,
                                                     Set<const bNodeTree *> &visited)
{
  if (visited.contains(&ntree)) {
    return false;
  }
  visited.add(&ntree);

  bool changed = false;
  for (bNode *node = static_cast<bNode *>(ntree.nodes.first); node != nullptr; node = node->next) {
    if (node->type_legacy == SH_NODE_FILTER_OBJECT_MASK &&
        NodeFilterMaskMode(node->custom1) == SHD_FILTER_MASK_COLLECTION)
    {
      Collection *collection = (node->id != nullptr && GS(node->id->name) == ID_GR) ?
                                   reinterpret_cast<Collection *>(node->id) :
                                   nullptr;
      const Vector<Object *> objects = filter_mask_collection_objects(collection);
      const int16_t signature = filter_mask_objects_signature(objects);
      if (node->custom2 != signature) {
        node->custom2 = signature;
        changed = true;
      }
    }

    if (node->type_legacy == NODE_GROUP && node->id != nullptr) {
      changed |= filter_mask_update_collection_signatures(
          *reinterpret_cast<bNodeTree *>(node->id), visited);
    }
  }

  return changed;
}

struct FilterMaterialAOVUsage {
  Vector<std::string> input_names;
  Vector<std::string> output_names;
};

static void filter_material_add_aov_name(Vector<std::string> &names, const StringRef name)
{
  for (const std::string &existing : names) {
    if (existing == name) {
      return;
    }
  }
  names.append(std::string(name));
}

static StringRef filter_material_aov_node_name(const bNode &node)
{
  if (node.storage == nullptr) {
    return "";
  }
  const NodeShaderOutputAOV *aov = static_cast<const NodeShaderOutputAOV *>(node.storage);
  return aov->name;
}

static void filter_material_collect_aov_usage(const bNodeTree &ntree,
                                              Set<const bNodeTree *> &visited,
                                              FilterMaterialAOVUsage &r_usage)
{
  if (visited.contains(&ntree)) {
    return;
  }
  visited.add(&ntree);

  for (const bNode *node = static_cast<const bNode *>(ntree.nodes.first); node != nullptr;
       node = node->next)
  {
    if (ELEM(node->type_legacy, SH_NODE_INPUT_AOV, SH_NODE_OUTPUT_AOV)) {
      const StringRef aov_name = filter_material_aov_node_name(*node);
      if (!aov_name.is_empty()) {
        Vector<std::string> &names = (node->type_legacy == SH_NODE_INPUT_AOV) ?
                                         r_usage.input_names :
                                         r_usage.output_names;
        filter_material_add_aov_name(names, aov_name);
      }
    }
    if (node->type_legacy == NODE_GROUP && node->id != nullptr) {
      filter_material_collect_aov_usage(
          *reinterpret_cast<const bNodeTree *>(node->id), visited, r_usage);
    }
  }
}

static Vector<std::string> filter_material_collect_conflicting_aov_names(
    const FilterMaterialAOVUsage &usage)
{
  Vector<std::string> conflicts;
  for (const std::string &input_name : usage.input_names) {
    for (const std::string &output_name : usage.output_names) {
      if (input_name == output_name) {
        conflicts.append(input_name);
        break;
      }
    }
  }
  return conflicts;
}

static bool filter_material_scene_sources_complete(const bool uses_scene_depth,
                                                   const bool uses_scene_normal,
                                                   const bool uses_scene_position,
                                                   const bool uses_cryptomatte_object)
{
  return uses_scene_depth && uses_scene_normal && uses_scene_position && uses_cryptomatte_object;
}

static const bNode *filter_material_active_output_node(const bNodeTree &ntree,
                                                       const int output_type)
{
  for (const bNode *node : ntree.all_nodes()) {
    if (node->type_legacy == output_type && (node->flag & NODE_DO_OUTPUT) && !node->is_muted()) {
      return node;
    }
  }
  for (const bNode *node : ntree.all_nodes()) {
    if (node->type_legacy == output_type && !node->is_muted()) {
      return node;
    }
  }
  return nullptr;
}

static void filter_material_collect_scene_sources_from_socket(
    const bNodeTree &ntree,
    const bNodeSocket *root_socket,
    Set<const bNodeTree *> &active_trees,
    Map<const bNodeTree *, const bNode *> &group_node_by_tree,
    bool &r_uses_scene_depth,
    bool &r_uses_scene_normal,
    bool &r_uses_scene_position,
    bool &r_uses_cryptomatte_object);

static void filter_material_collect_scene_source_node(const bNode &node,
                                                      const bNodeSocket *output_socket,
                                                      bool &r_uses_scene_depth,
                                                      bool &r_uses_scene_normal,
                                                      bool &r_uses_scene_position,
                                                      bool &r_uses_cryptomatte_object)
{
  if (node.type_legacy == SH_NODE_SCENE_COLOR) {
    if (output_socket == nullptr) {
      return;
    }
    const bool legacy_sample_output = STREQ(output_socket->identifier, "Color") ||
                                      STREQ(output_socket->identifier, "Alpha");
    if (std::strcmp(output_socket->identifier, "Depth Image") == 0 ||
        (node.custom1 == SHD_SCENE_SOURCE_DEPTH && legacy_sample_output))
    {
      r_uses_scene_depth = true;
    }
    else if (std::strcmp(output_socket->identifier, "Normal Image") == 0 ||
             (node.custom1 == SHD_SCENE_SOURCE_NORMAL && legacy_sample_output))
    {
      r_uses_scene_normal = true;
    }
    else if (std::strcmp(output_socket->identifier, "Position Image") == 0 ||
             (node.custom1 == SHD_SCENE_SOURCE_POSITION && legacy_sample_output))
    {
      r_uses_scene_position = true;
    }
  }
  else if (node.type_legacy == SH_NODE_FILTER_OBJECT_MASK) {
    r_uses_cryptomatte_object = true;
  }
}

static void filter_material_collect_scene_sources_from_node(
    const bNodeTree &ntree,
    const bNode *node,
    const bNodeSocket *output_socket,
    Set<const bNodeTree *> &active_trees,
    Map<const bNodeTree *, const bNode *> &group_node_by_tree,
    bool &r_uses_scene_depth,
    bool &r_uses_scene_normal,
    bool &r_uses_scene_position,
    bool &r_uses_cryptomatte_object)
{
  if (node == nullptr || node->is_muted() ||
      filter_material_scene_sources_complete(r_uses_scene_depth,
                                             r_uses_scene_normal,
                                             r_uses_scene_position,
                                             r_uses_cryptomatte_object))
  {
    return;
  }

  if (node->type_legacy == NODE_GROUP) {
    const bNodeTree *group_tree = reinterpret_cast<const bNodeTree *>(node->id);
    if (group_tree == nullptr || output_socket == nullptr) {
      return;
    }

    const bNode *group_output_node = filter_material_active_output_node(*group_tree,
                                                                        NODE_GROUP_OUTPUT);
    if (group_output_node == nullptr) {
      return;
    }

    const bNodeSocket *group_output_input = group_output_node->input_by_identifier(
        output_socket->identifier);
    const bNode *previous_group_node = group_node_by_tree.lookup_default(group_tree, nullptr);
    group_node_by_tree.add_overwrite(group_tree, node);
    filter_material_collect_scene_sources_from_socket(*group_tree,
                                                      group_output_input,
                                                      active_trees,
                                                      group_node_by_tree,
                                                      r_uses_scene_depth,
                                                      r_uses_scene_normal,
                                                      r_uses_scene_position,
                                                      r_uses_cryptomatte_object);
    if (previous_group_node != nullptr) {
      group_node_by_tree.add_overwrite(group_tree, previous_group_node);
    }
    else {
      group_node_by_tree.remove(group_tree);
    }
    return;
  }

  if (node->type_legacy == NODE_GROUP_INPUT) {
    const bNode *group_node = group_node_by_tree.lookup_default(&ntree, nullptr);
    if (group_node == nullptr || output_socket == nullptr) {
      return;
    }

    if (const bNodeSocket *group_input = group_node->input_by_identifier(output_socket->identifier))
    {
      const bNodeTree &owner_tree = group_node->owner_tree();
      owner_tree.ensure_topology_cache();
      for (const bNodeLink *link : group_input->directly_linked_links()) {
        if (!link->is_used()) {
          continue;
        }
        filter_material_collect_scene_sources_from_node(owner_tree,
                                                        link->fromnode,
                                                        link->fromsock,
                                                        active_trees,
                                                        group_node_by_tree,
                                                        r_uses_scene_depth,
                                                        r_uses_scene_normal,
                                                        r_uses_scene_position,
                                                        r_uses_cryptomatte_object);
        if (filter_material_scene_sources_complete(r_uses_scene_depth,
                                                   r_uses_scene_normal,
                                                   r_uses_scene_position,
                                                   r_uses_cryptomatte_object))
        {
          break;
        }
      }
    }
    return;
  }

  filter_material_collect_scene_source_node(*node,
                                            output_socket,
                                            r_uses_scene_depth,
                                            r_uses_scene_normal,
                                            r_uses_scene_position,
                                            r_uses_cryptomatte_object);

  for (const bNodeSocket *socket : node->input_sockets()) {
    for (const bNodeLink *link : socket->directly_linked_links()) {
      if (!link->is_used()) {
        continue;
      }
      filter_material_collect_scene_sources_from_node(ntree,
                                                      link->fromnode,
                                                      link->fromsock,
                                                      active_trees,
                                                      group_node_by_tree,
                                                      r_uses_scene_depth,
                                                      r_uses_scene_normal,
                                                      r_uses_scene_position,
                                                      r_uses_cryptomatte_object);
      if (filter_material_scene_sources_complete(r_uses_scene_depth,
                                                 r_uses_scene_normal,
                                                 r_uses_scene_position,
                                                 r_uses_cryptomatte_object))
      {
        return;
      }
    }
  }
}

static void filter_material_collect_scene_sources_from_socket(
    const bNodeTree &ntree,
    const bNodeSocket *root_socket,
    Set<const bNodeTree *> &active_trees,
    Map<const bNodeTree *, const bNode *> &group_node_by_tree,
    bool &r_uses_scene_depth,
    bool &r_uses_scene_normal,
    bool &r_uses_scene_position,
    bool &r_uses_cryptomatte_object)
{
  if (root_socket == nullptr || active_trees.contains(&ntree)) {
    return;
  }

  ntree.ensure_topology_cache();
  active_trees.add(&ntree);
  for (const bNodeLink *link : root_socket->directly_linked_links()) {
    if (!link->is_used()) {
      continue;
    }
    filter_material_collect_scene_sources_from_node(ntree,
                                                    link->fromnode,
                                                    link->fromsock,
                                                    active_trees,
                                                    group_node_by_tree,
                                                    r_uses_scene_depth,
                                                    r_uses_scene_normal,
                                                    r_uses_scene_position,
                                                    r_uses_cryptomatte_object);
    if (filter_material_scene_sources_complete(r_uses_scene_depth,
                                               r_uses_scene_normal,
                                               r_uses_scene_position,
                                               r_uses_cryptomatte_object))
    {
      break;
    }
  }
  active_trees.remove(&ntree);
}

static void filter_material_collect_scene_sources_from_output(
    const bNodeTree &ntree,
    const bNode &output_node,
    Set<const bNodeTree *> &active_trees,
    Map<const bNodeTree *, const bNode *> &group_node_by_tree,
    bool &r_uses_scene_depth,
    bool &r_uses_scene_normal,
    bool &r_uses_scene_position,
    bool &r_uses_cryptomatte_object)
{
  for (const bNodeSocket *socket : output_node.input_sockets()) {
    filter_material_collect_scene_sources_from_socket(ntree,
                                                      socket,
                                                      active_trees,
                                                      group_node_by_tree,
                                                      r_uses_scene_depth,
                                                      r_uses_scene_normal,
                                                      r_uses_scene_position,
                                                      r_uses_cryptomatte_object);
    if (filter_material_scene_sources_complete(r_uses_scene_depth,
                                               r_uses_scene_normal,
                                               r_uses_scene_position,
                                               r_uses_cryptomatte_object))
    {
      return;
    }
  }
}

static void filter_material_collect_scene_sources(const bNodeTree &ntree,
                                                  bool &r_uses_scene_depth,
                                                  bool &r_uses_scene_normal,
                                                  bool &r_uses_scene_position,
                                                  bool &r_uses_cryptomatte_object)
{
  Set<const bNodeTree *> active_trees;
  Map<const bNodeTree *, const bNode *> group_node_by_tree;

  if (const bNode *output_node = filter_material_active_output_node(ntree, SH_NODE_OUTPUT_FILTER)) {
    filter_material_collect_scene_sources_from_output(ntree,
                                                      *output_node,
                                                      active_trees,
                                                      group_node_by_tree,
                                                      r_uses_scene_depth,
                                                      r_uses_scene_normal,
                                                      r_uses_scene_position,
                                                      r_uses_cryptomatte_object);
    if (filter_material_scene_sources_complete(r_uses_scene_depth,
                                               r_uses_scene_normal,
                                               r_uses_scene_position,
                                               r_uses_cryptomatte_object))
    {
      return;
    }
  }

  for (const bNode *node : ntree.all_nodes()) {
    if (!ELEM(node->type_legacy, SH_NODE_OUTPUT_AOV, SH_NODE_OUTLINE_CONTROL) || node->is_muted()) {
      continue;
    }
    filter_material_collect_scene_sources_from_output(ntree,
                                                      *node,
                                                      active_trees,
                                                      group_node_by_tree,
                                                      r_uses_scene_depth,
                                                      r_uses_scene_normal,
                                                      r_uses_scene_position,
                                                      r_uses_cryptomatte_object);
    if (filter_material_scene_sources_complete(r_uses_scene_depth,
                                               r_uses_scene_normal,
                                               r_uses_scene_position,
                                               r_uses_cryptomatte_object))
    {
      return;
    }
  }
}

static void filter_graph_collect_scene_sources(const Span<const bNode *> dependencies,
                                               bool &r_uses_scene_depth,
                                               bool &r_uses_scene_normal,
                                               bool &r_uses_scene_position)
{
  for (const bNode *node : dependencies) {
    if (node->type_legacy != EEVEE_FILTER_GRAPH_NODE_SCENE_COLOR || node->is_muted()) {
      continue;
    }
    for (const bNodeSocket *socket : node->output_sockets()) {
      bool has_used_link = false;
      for (const bNodeLink *link : socket->directly_linked_links()) {
        if (link->is_used()) {
          has_used_link = true;
          break;
        }
      }
      if (!has_used_link) {
        continue;
      }
      if (STREQ(socket->identifier, "Depth Image")) {
        r_uses_scene_depth = true;
      }
      else if (STREQ(socket->identifier, "Normal Image")) {
        r_uses_scene_normal = true;
      }
      else if (STREQ(socket->identifier, "Position Image")) {
        r_uses_scene_position = true;
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

  if (filter_graph_enabled(*inst_.scene)) {
    bNodeTree *filter_graph = inst_.scene->eevee.filter_graph;
    filter_graph->ensure_topology_cache();
    Vector<const bNode *> dependencies;
    for (const int stage : {SCE_EEVEE_FILTER_STAGE_BEFORE_VOLUME_FOG,
                            SCE_EEVEE_FILTER_STAGE_BEFORE_POSTFX,
                            SCE_EEVEE_FILTER_STAGE_BEFORE_DEPTH_OF_FIELD,
                            SCE_EEVEE_FILTER_STAGE_BEFORE_COMPOSITE})
    {
      if (!filter_graph_stage_dependency_order_get(
              *filter_graph, SceneEEVEEFilterExecutionStage(stage), dependencies))
      {
        dependencies.clear();
        break;
      }
    }
    filter_graph_collect_scene_sources(
        dependencies, uses_scene_depth_, uses_scene_normal_, uses_scene_position_);
    for (const bNode *node : dependencies) {
      if (node->type_legacy != EEVEE_FILTER_GRAPH_NODE_FILTER_MATERIAL || node->is_muted()) {
        continue;
      }
      blender::Material *material = filter_graph_node_material(*node);
      if (!filter_material_is_valid(material)) {
        continue;
      }
      filter_material_collect_scene_sources(*material->nodetree,
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
    return;
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
  const int material_mask_count = GPU_material_filter_mask_object_count(gpumat);
  const int object_count = min_ii(material_object_count + material_mask_count,
                                  FILTER_OBJECT_INFO_MAX);
  for (int index = 0; index < object_count; index++) {
    Object *object = (index < material_object_count) ?
                         GPU_material_filter_object_info_get(gpumat, index) :
                         GPU_material_filter_mask_object_get(gpumat, index - material_object_count);
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

bool FilterMaterialModule::sync_pass_entry(blender::Material *material, FilterPassEntry &entry)
{
  if (!filter_material_is_valid(material)) {
    return false;
  }

  Set<const bNodeTree *> visited;
  const bool collection_signature_changed = filter_mask_update_collection_signatures(
      *material->nodetree, visited);
  if (collection_signature_changed) {
    GPU_material_free(&material->gpumaterial);
  }

  GPUMaterial *gpumat = inst_.shaders.material_shader_get(material,
                                                          material->nodetree,
                                                          MAT_PIPE_FILTER,
                                                          MAT_GEOM_WORLD,
                                                          MAT_PROBE_NONE,
                                                          false,
                                                          nullptr,
                                                          false);
  const int status = (gpumat != nullptr) ? GPU_material_status(gpumat) : -1;
  const int has_filter_output = (gpumat != nullptr) ? int(GPU_material_has_filter_output(gpumat)) :
                                                     0;
  if (gpumat == nullptr || status != GPU_MAT_SUCCESS || !has_filter_output) {
    return false;
  }

  inst_.manager->register_layer_attributes(gpumat);

  visited.clear();
  FilterMaterialAOVUsage aov_usage;
  filter_material_collect_aov_usage(*material->nodetree, visited, aov_usage);

  entry.material = material;
  entry.gpumat = gpumat;
  entry.uses_aov_input = !aov_usage.input_names.is_empty();
  entry.uses_aov_output = !aov_usage.output_names.is_empty();
  entry.conflicting_aov_names = filter_material_collect_conflicting_aov_names(aov_usage);
  return true;
}

void FilterMaterialModule::begin_sync()
{
  entries_.clear();
  uses_scene_time_ = false;

  if (filter_graph_enabled(*inst_.scene)) {
    bNodeTree *filter_graph = inst_.scene->eevee.filter_graph;
    filter_graph->ensure_topology_cache();
    blender::nodes::filter_graph_sync_filter_pass_interfaces_from_materials(*filter_graph);
    Vector<const bNode *> dependencies;
    for (const int stage : {SCE_EEVEE_FILTER_STAGE_BEFORE_VOLUME_FOG,
                            SCE_EEVEE_FILTER_STAGE_BEFORE_POSTFX,
                            SCE_EEVEE_FILTER_STAGE_BEFORE_DEPTH_OF_FIELD,
                            SCE_EEVEE_FILTER_STAGE_BEFORE_COMPOSITE})
    {
      if (!filter_graph_stage_dependency_order_get(
              *filter_graph, SceneEEVEEFilterExecutionStage(stage), dependencies))
      {
        dependencies.clear();
        break;
      }
    }
    Set<const bNode *> synced_nodes;
    for (const bNode *node : dependencies) {
      if (node->type_legacy != EEVEE_FILTER_GRAPH_NODE_FILTER_MATERIAL || node->is_muted()) {
        continue;
      }
      if (synced_nodes.contains(node)) {
        continue;
      }
      synced_nodes.add(node);

      FilterPassEntry entry;
      entry.graph_node = node;
      blender::Material *material = filter_graph_node_material(*node);
      if (!sync_pass_entry(material, entry)) {
        continue;
      }

      uses_scene_time_ |= GPU_material_is_time_dependent(entry.gpumat);
      entries_.append(entry);
    }
    return;
  }
}

bool FilterMaterialModule::has_stage_entries(SceneEEVEEFilterExecutionStage stage) const
{
  if (filter_graph_enabled(*inst_.scene)) {
    const bNodeTree *filter_graph = inst_.scene->eevee.filter_graph;
    for (const bNode *node : filter_graph->all_nodes()) {
      if (node->type_legacy == EEVEE_FILTER_GRAPH_NODE_STAGE_OUTPUT && !node->is_muted() &&
          node->custom1 == stage && (node->flag & NODE_DO_OUTPUT))
      {
        return true;
      }
    }
    return false;
  }
  return false;
}

static FilterGraphImageHandle filter_graph_scene_output_handle(const bNodeSocket &socket)
{
  if (STREQ(socket.identifier, "Color Image")) {
    return FilterGraphImageHandle::scene(0);
  }
  if (STREQ(socket.identifier, "Depth Image")) {
    return FilterGraphImageHandle::scene(1);
  }
  if (STREQ(socket.identifier, "Normal Image")) {
    return FilterGraphImageHandle::scene(2);
  }
  if (STREQ(socket.identifier, "Position Image")) {
    return FilterGraphImageHandle::scene(4);
  }
  return FilterGraphImageHandle::null();
}

static FilterGraphImageHandle filter_graph_aov_output_handle(const bNode &node,
                                                             const bNodeSocket &socket,
                                                             const RenderBuffersInfoData &data)
{
  const NodeEeveeFilterGraphAOVInput *storage =
      static_cast<const NodeEeveeFilterGraphAOVInput *>(node.storage);
  if (storage == nullptr) {
    return FilterGraphImageHandle::null();
  }
  if (STREQ(socket.identifier, "Color")) {
    const int aov_index = filter_graph_aov_index_get(data, storage->name, false);
    return (aov_index >= 0) ?
               FilterGraphImageHandle::render_pass_color(data.color_len + aov_index) :
               FilterGraphImageHandle::null();
  }
  if (STREQ(socket.identifier, "Value")) {
    const int aov_index = filter_graph_aov_index_get(data, storage->name, true);
    return (aov_index >= 0) ?
               FilterGraphImageHandle::render_pass_value(data.value_len + aov_index) :
               FilterGraphImageHandle::null();
  }
  return FilterGraphImageHandle::null();
}

gpu::Texture *FilterMaterialModule::render_stage(draw::View &view,
                                                 gpu::Texture *input_tx,
                                                 int2 extent,
                                                 SceneEEVEEFilterExecutionStage stage)
{
  if (input_tx == nullptr || !has_stage_entries(stage)) {
    return input_tx;
  }

  const gpu::TextureFormat stage_format = GPU_texture_format(input_tx);

  auto black_graph_output = [&]() -> gpu::Texture * {
    graph_black_tx_.ensure_2d(stage_format, extent, GPU_TEXTURE_USAGE_GENERAL);
    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GPU_texture_clear(graph_black_tx_, GPU_DATA_FLOAT, clear_color);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
    return graph_black_tx_;
  };

  auto clear_filter_graph_inputs = [&]() {
    for (FilterGraphInputHandleData &handle : filter_graph_input_buf_) {
      handle.type = FILTER_TEX_HANDLE_NULL;
      handle.index = 0;
      handle.alpha_mode = FILTER_GRAPH_ALPHA_MODE_OPACITY;
      handle._pad = 0;
    }
    graph_input_tx_.ensure_2d_array(stage_format, extent, 1, GPU_TEXTURE_USAGE_GENERAL);
    filter_graph_input_buf_.push_update();
  };

  auto prepare_filter_graph_inputs = [&](const Vector<FilterGraphImageHandle> &inputs) -> bool {
    Vector<FilterGraphTextureInput> texture_inputs;
    for (FilterGraphInputHandleData &handle : filter_graph_input_buf_) {
      handle.type = FILTER_TEX_HANDLE_NULL;
      handle.index = 0;
      handle.alpha_mode = FILTER_GRAPH_ALPHA_MODE_OPACITY;
      handle._pad = 0;
    }
    for (const int i : inputs.index_range()) {
      if (i >= FILTER_GRAPH_INPUT_MAX) {
        break;
      }
      const FilterGraphImageHandle &input = inputs[i];
      if (input.is_texture()) {
        filter_graph_input_buf_[i].type = FILTER_TEX_HANDLE_FILTER_GRAPH_TEXTURE;
        filter_graph_input_buf_[i].index = texture_inputs.size();
        filter_graph_input_buf_[i].alpha_mode = input.alpha_mode;
        texture_inputs.append({input.texture, input.layer});
      }
      else if (input.type != FILTER_TEX_HANDLE_FILTER_GRAPH_INPUT) {
        filter_graph_input_buf_[i].type = input.type;
        filter_graph_input_buf_[i].index = input.index;
        filter_graph_input_buf_[i].alpha_mode = input.alpha_mode;
      }
    }

    const int layer_count = math::max(1, int(texture_inputs.size()));
    graph_input_tx_.ensure_2d_array(stage_format, extent, layer_count, GPU_TEXTURE_USAGE_GENERAL);
    if (!texture_inputs.is_empty()) {
      graph_input_tx_.ensure_layer_views();
      while (graph_input_fbs_.size() < texture_inputs.size()) {
        graph_input_fbs_.append(std::make_unique<Framebuffer>("FilterMaterial.GraphInputCopy"));
      }
      while (graph_input_fbs_.size() > texture_inputs.size()) {
        graph_input_fbs_.remove_last();
      }
      gpu::Shader *copy_shader = inst_.shaders.static_shader_get(FILTER_GRAPH_INPUT_COPY);
      if (copy_shader == nullptr) {
        filter_graph_input_buf_.push_update();
        return false;
      }
      for (const int layer : texture_inputs.index_range()) {
        graph_input_fbs_[layer]->ensure(
            GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE_LAYER(graph_input_tx_.layer_view(layer), 0));

        PassSimple pass("FilterMaterial.GraphInputCopy");
        pass.init();
        pass.state_set(DRW_STATE_WRITE_COLOR);
        pass.framebuffer_set(&*graph_input_fbs_[layer]);
        pass.shader_set(copy_shader);
        pass.bind_texture("input_tx", texture_inputs[layer].texture);
        pass.push_constant("input_layer", texture_inputs[layer].layer);
        pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
        inst_.manager->submit(pass);
      }
      GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
    }
    filter_graph_input_buf_.push_update();
    return true;
  };

  auto render_filter_entry = [&](const FilterPassEntry &entry,
                                 gpu::Texture *scene_color_tx,
                                 Texture &output_tx) {
    gpu::Texture *aov_color_tx = inst_.render_buffers.rp_color_tx;
    gpu::Texture *aov_value_tx = inst_.render_buffers.rp_value_tx;

    if (!entry.conflicting_aov_names.is_empty()) {
      aov_color_snapshot_tx_.ensure_2d_array(GPU_texture_format(inst_.render_buffers.rp_color_tx),
                                             extent,
                                             GPU_texture_layer_count(inst_.render_buffers.rp_color_tx),
                                             GPU_TEXTURE_USAGE_GENERAL);
      aov_value_snapshot_tx_.ensure_2d_array(GPU_texture_format(inst_.render_buffers.rp_value_tx),
                                             extent,
                                             GPU_texture_layer_count(inst_.render_buffers.rp_value_tx),
                                             GPU_TEXTURE_USAGE_GENERAL);
      GPU_texture_copy(aov_color_snapshot_tx_, inst_.render_buffers.rp_color_tx);
      GPU_texture_copy(aov_value_snapshot_tx_, inst_.render_buffers.rp_value_tx);
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH |
                         GPU_BARRIER_SHADER_IMAGE_ACCESS);
      aov_color_tx = aov_color_snapshot_tx_;
      aov_value_tx = aov_value_snapshot_tx_;
    }

    output_tx.ensure_layer_views();
    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GPU_texture_clear(output_tx, GPU_DATA_FLOAT, clear_color);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_SHADER_IMAGE_ACCESS);
    pong_tx_.ensure_2d(
        gpu::TextureFormat::SFLOAT_16_16_16_16, extent, GPU_TEXTURE_USAGE_GENERAL);
    framebuffer_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(pong_tx_));

    PassSimple pass = {"FilterMaterial.Pass"};
    pass.state_set(DRW_STATE_WRITE_COLOR);
    pass.framebuffer_set(&framebuffer_);
    update_filter_object_info_buffer(entry.gpumat);
    pass.material_set(*inst_.manager, entry.gpumat);
    pass.bind_texture("scene_color_tx", &scene_color_tx);
    pass.bind_texture("rp_color_tx", &aov_color_tx);
    pass.bind_texture("rp_value_tx", &aov_value_tx);
    pass.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
    pass.bind_texture("cryptomatte_tx", &inst_.render_buffers.cryptomatte_tx);
    pass.bind_texture("filter_graph_input_tx", &graph_input_tx_);
    pass.bind_image(FILTER_GRAPH_OUTPUT_IMG_SLOT, &output_tx);
    pass.bind_image(RBUFS_COLOR_SLOT, &inst_.render_buffers.rp_color_tx);
    pass.bind_image(RBUFS_VALUE_SLOT, &inst_.render_buffers.rp_value_tx);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_ubo(FILTER_OBJECT_INFO_BUF_SLOT, &filter_object_info_buf_);
    pass.bind_ubo(FILTER_GRAPH_INPUT_BUF_SLOT, &filter_graph_input_buf_);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.render_textures);
    pass.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
    pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);

    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
    inst_.manager->submit(pass, view);
    GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
  };

  auto resolve_filter_graph_handle = [&](const FilterGraphImageHandle &handle) -> gpu::Texture * {
    if (handle.type == FILTER_TEX_HANDLE_NULL) {
      return black_graph_output();
    }
    if (handle.type == FILTER_TEX_HANDLE_SCENE && handle.index == 0) {
      return input_tx;
    }

    gpu::Shader *resolve_shader = inst_.shaders.static_shader_get(FILTER_GRAPH_RESOLVE);
    if (resolve_shader == nullptr) {
      return black_graph_output();
    }

    Vector<FilterGraphImageHandle> resolve_inputs;
    resolve_inputs.append(handle);
    if (!prepare_filter_graph_inputs(resolve_inputs)) {
      return black_graph_output();
    }
    ping_tx_.ensure_2d(stage_format, extent, GPU_TEXTURE_USAGE_GENERAL);
    framebuffer_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(ping_tx_));

    PassSimple pass("FilterMaterial.GraphResolve");
    pass.init();
    pass.state_set(DRW_STATE_WRITE_COLOR);
    pass.framebuffer_set(&framebuffer_);
    pass.shader_set(resolve_shader);
    pass.bind_texture("scene_color_tx", &input_tx);
    pass.bind_texture("rp_color_tx", &inst_.render_buffers.rp_color_tx);
    pass.bind_texture("rp_value_tx", &inst_.render_buffers.rp_value_tx);
    pass.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
    pass.bind_texture("filter_graph_input_tx", &graph_input_tx_);
    pass.bind_ubo(FILTER_OBJECT_INFO_BUF_SLOT, &filter_object_info_buf_);
    pass.bind_ubo(FILTER_GRAPH_INPUT_BUF_SLOT, &filter_graph_input_buf_);
    pass.bind_resources(inst_.uniform_data);
    pass.barrier(GPU_BARRIER_TEXTURE_FETCH);
    pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);

    inst_.manager->submit(pass, view);
    GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
    return ping_tx_;
  };

  if (filter_graph_enabled(*inst_.scene)) {
    bNodeTree &filter_graph = *inst_.scene->eevee.filter_graph;
    filter_graph.ensure_topology_cache();

    const bNode *stage_output = filter_graph_stage_output_node_get(filter_graph, stage);
    if (stage_output == nullptr) {
      return input_tx;
    }

    const bNodeSocket *stage_input = stage_output->input_by_identifier("Image");
    const bNodeLink *stage_link = (stage_input != nullptr) ?
                                      filter_graph_socket_used_link(*stage_input) :
                                      nullptr;
    if (stage_link == nullptr) {
      return black_graph_output();
    }

    Set<const bNode *> visiting;
    Set<const bNode *> visited;
    Vector<const bNode *> order;
    if (!filter_graph_collect_dependencies(*stage_link->fromnode, visiting, visited, order)) {
      inst_.info_append_i18n(
          "Error: Filter Graph contains a cycle, invalid material, unsupported node, or too many inputs");
      return black_graph_output();
    }

    Map<const bNodeSocket *, FilterGraphImageHandle> result_by_socket;
    std::vector<std::unique_ptr<Texture>> graph_textures;

    for (const bNode *node : order) {
      if (node->type_legacy == EEVEE_FILTER_GRAPH_NODE_SCENE_COLOR) {
        for (const bNodeSocket *socket : node->output_sockets()) {
          result_by_socket.add_overwrite(socket, filter_graph_scene_output_handle(*socket));
        }
      }
      else if (node->type_legacy == EEVEE_FILTER_GRAPH_NODE_AOV_INPUT) {
        for (const bNodeSocket *socket : node->output_sockets()) {
          result_by_socket.add_overwrite(
              socket, filter_graph_aov_output_handle(*node, *socket, inst_.render_buffers.data));
        }
      }
      else if (node->is_reroute()) {
        const bNodeSocket *input_socket = static_cast<const bNodeSocket *>(node->inputs.first);
        const bNodeLink *input_link = (input_socket != nullptr) ?
                                          filter_graph_socket_used_link(*input_socket) :
                                          nullptr;
        const FilterGraphImageHandle input_handle =
            (input_link != nullptr) ?
                result_by_socket.lookup_default(input_link->fromsock, FilterGraphImageHandle::null()) :
                FilterGraphImageHandle::null();
        for (const bNodeSocket *socket : node->output_sockets()) {
          result_by_socket.add_overwrite(socket, input_handle);
        }
      }
      else if (node->type_legacy == EEVEE_FILTER_GRAPH_NODE_FILTER_MATERIAL) {
        const FilterPassEntry *entry = nullptr;
        for (const FilterPassEntry &candidate : entries_) {
          if (candidate.graph_node == node) {
            entry = &candidate;
            break;
          }
        }
        if (entry == nullptr) {
          inst_.info_append_i18n("Error: Filter Graph material node did not compile");
          return black_graph_output();
        }

        const NodeEeveeFilterGraphFilterMaterial *storage =
            static_cast<const NodeEeveeFilterGraphFilterMaterial *>(node->storage);
        if (storage == nullptr || storage->items_num > FILTER_GRAPH_INPUT_MAX) {
          inst_.info_append_i18n("Error: Filter Graph material node has too many inputs");
          return black_graph_output();
        }
        blender::Material *material = filter_graph_node_material(*node);
        const int output_count = filter_material_output_count(material);
        if (output_count <= 0) {
          inst_.info_append_i18n("Error: Filter Graph material node has no Filter Output");
          return black_graph_output();
        }
        if (output_count > FILTER_GRAPH_OUTPUT_MAX) {
          inst_.info_append_i18n("Error: Filter Graph material node has too many outputs");
          return black_graph_output();
        }

        Vector<FilterGraphImageHandle> material_inputs;
        material_inputs.reserve(storage->items_num);
        for (const int i : IndexRange(storage->items_num)) {
          const std::string identifier = "Image_" + std::to_string(storage->items[i].identifier);
          const bNodeSocket *socket = node->input_by_identifier(identifier);
          const bNodeLink *link = (socket != nullptr) ? filter_graph_socket_used_link(*socket) :
                                                       nullptr;
          material_inputs.append(
              (link != nullptr) ?
                  result_by_socket.lookup_default(link->fromsock, FilterGraphImageHandle::null()) :
                  FilterGraphImageHandle::null());
        }
        if (!prepare_filter_graph_inputs(material_inputs)) {
          return black_graph_output();
        }

        std::unique_ptr<Texture> target_tx = std::make_unique<Texture>(
            "FilterMaterial.GraphIntermediate");
        target_tx->ensure_2d_array(gpu::TextureFormat::SFLOAT_16_16_16_16,
                                   extent,
                                   output_count,
                                   GPU_TEXTURE_USAGE_GENERAL);
        render_filter_entry(*entry, input_tx, *target_tx);

        target_tx->ensure_layer_views();
        for (const bNodeSocket *output_socket : node->output_sockets()) {
          const int output_index = filter_material_output_index_from_socket(material, *output_socket);
          if (output_index < 0 || output_index >= output_count) {
            continue;
          }
          result_by_socket.add_overwrite(
              output_socket,
              FilterGraphImageHandle::graph_texture(target_tx->gpu_texture(), output_index));
        }
        graph_textures.emplace_back(std::move(target_tx));
      }
    }

    const FilterGraphImageHandle output_handle = result_by_socket.lookup_default(
        stage_link->fromsock, FilterGraphImageHandle::null());
    return resolve_filter_graph_handle(output_handle);
  }

  return input_tx;
}

}  // namespace blender::eevee
