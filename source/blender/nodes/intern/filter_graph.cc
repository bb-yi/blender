/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"

#include "DEG_depsgraph.hh"

#include "NOD_filter_graph.hh"
#include "NOD_socket.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "MEM_guardedalloc.h"

namespace blender::nodes {

static bool filter_graph_stage_valid(const int stage)
{
  switch (stage) {
    case SCE_EEVEE_FILTER_STAGE_BEFORE_DEPTH_OF_FIELD:
    case SCE_EEVEE_FILTER_STAGE_BEFORE_COMPOSITE:
    case SCE_EEVEE_FILTER_STAGE_BEFORE_VOLUME_FOG:
    case SCE_EEVEE_FILTER_STAGE_BEFORE_POSTFX:
      return true;
  }
  return false;
}

static const char *filter_graph_scene_color_output_identifier(const int index)
{
  switch (index) {
    case 0:
      return "Color Image";
    case 1:
      return "Depth Image";
    case 2:
      return "Normal Image";
    case 3:
      return "Position Image";
  }
  return nullptr;
}

static int filter_graph_scene_color_output_index(const char *identifier)
{
  for (int i = 0; i < 4; i++) {
    if (STREQ(identifier, filter_graph_scene_color_output_identifier(i))) {
      return i;
    }
  }
  return -1;
}

static int filter_graph_legacy_scene_color_source_output_index(const bNode &node)
{
  switch (node.custom1) {
    case SHD_SCENE_SOURCE_DEPTH:
      return 1;
    case SHD_SCENE_SOURCE_NORMAL:
      return 2;
    case SHD_SCENE_SOURCE_POSITION:
      return 3;
    case SHD_SCENE_SOURCE_COLOR:
    case SHD_SCENE_SOURCE_SHADOW:
    default:
      return 0;
  }
}

static const char *filter_graph_legacy_scene_color_sample_output_identifier(
    const char *identifier)
{
  if (STREQ(identifier, "Color")) {
    return "Color";
  }
  if (STREQ(identifier, "Alpha")) {
    return "Alpha";
  }
  return nullptr;
}

static bNodeSocket *filter_graph_find_first_image_socket(bNode &node,
                                                         const eNodeSocketInOut in_out)
{
  ListBase &sockets = (in_out == SOCK_IN) ? node.inputs : node.outputs;
  for (bNodeSocket *socket = static_cast<bNodeSocket *>(sockets.first); socket != nullptr;
       socket = socket->next)
  {
    if (socket->type == SOCK_IMAGE && !STREQ(socket->idname, "NodeSocketVirtual")) {
      return socket;
    }
  }
  return nullptr;
}

static bNodeSocket *filter_graph_input_socket_for_identifier(bNode &node, const int identifier)
{
  const std::string socket_identifier = "Image_" + std::to_string(identifier);
  return bke::node_find_socket(node, SOCK_IN, UString(socket_identifier.c_str()));
}

static bNodeSocket *filter_graph_main_output_socket(bNode &node)
{
  if (bNodeSocket *socket = bke::node_find_socket(node, SOCK_OUT, "Image"_ustr)) {
    return socket;
  }
  if (bNodeSocket *socket = bke::node_find_socket(node, SOCK_OUT, "Image_0"_ustr)) {
    return socket;
  }
  return filter_graph_find_first_image_socket(node, SOCK_OUT);
}

static bool filter_graph_node_link_exists(const bNodeTree &ntree,
                                          const bNode &from_node,
                                          const bNodeSocket &from_socket,
                                          const bNode &to_node,
                                          const bNodeSocket &to_socket)
{
  for (const bNodeLink &link : ntree.links) {
    if (link.fromnode == &from_node && link.fromsock == &from_socket &&
        link.tonode == &to_node && link.tosock == &to_socket)
    {
      return true;
    }
  }
  return false;
}

static bNode *filter_graph_find_pass_input_node(Material &material)
{
  if (material.nodetree == nullptr) {
    return nullptr;
  }
  for (bNode &node : material.nodetree->nodes) {
    if (STREQ(node.idname, ShaderFilterGraphInputItemsAccessor::node_idname.c_str()) &&
        node.storage != nullptr)
    {
      return &node;
    }
  }
  return nullptr;
}

static bNode *filter_graph_ensure_pass_input_node(Material &material,
                                                  const bool used_scene_color_outputs[4])
{
  bNodeTree *ntree = material.nodetree;
  if (ntree == nullptr) {
    return nullptr;
  }
  bNode *node = filter_graph_find_pass_input_node(material);

  if (node == nullptr) {
    node = bke::node_add_node(
        nullptr, *ntree, UString(ShaderFilterGraphInputItemsAccessor::node_idname));
    if (node == nullptr || node->storage == nullptr) {
      return nullptr;
    }

    node->location[0] = -520.0f;
    node->location[1] = 220.0f;
    for (bNode &other_node : ntree->nodes) {
      if (other_node.type_legacy == SH_NODE_SCENE_COLOR ||
          STREQ(other_node.idname, "ShaderNodeSceneColor"))
      {
        node->location[0] = other_node.location[0] - 260.0f;
        node->location[1] = other_node.location[1];
        break;
      }
    }
  }

  NodeShaderFilterGraphInput &storage = *static_cast<NodeShaderFilterGraphInput *>(node->storage);
  for (int i = 0; i < storage.items_num; i++) {
    MEM_SAFE_DELETE(storage.items[i].name);
  }
  MEM_SAFE_DELETE(storage.items);

  int items_num = 0;
  for (int i = 0; i < 4; i++) {
    if (used_scene_color_outputs[i]) {
      items_num++;
    }
  }

  storage.items_num = items_num;
  storage.items = items_num > 0 ? MEM_new_array<NodeEeveeFilterGraphSocketItem>(items_num,
                                                                                 __func__) :
                                  nullptr;
  storage.active_index = items_num > 0 ? 0 : -1;
  storage.next_identifier = 4;

  int item_index = 0;
  for (int i = 0; i < 4; i++) {
    if (!used_scene_color_outputs[i]) {
      continue;
    }
    NodeEeveeFilterGraphSocketItem &item = storage.items[item_index++];
    item.name = BLI_strdup(filter_graph_scene_color_output_identifier(i));
    item.identifier = i;
  }

  update_node_declaration_and_sockets(*ntree, *node);
  return node;
}

static bNodeSocket *filter_graph_find_image_sample_vector_input(bNode &node)
{
  if (bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, "Vector"_ustr)) {
    return socket;
  }
  return bke::node_find_socket(node, SOCK_IN, "Offset"_ustr);
}

static bNode *filter_graph_add_legacy_scene_color_image_sample(bNodeTree &ntree,
                                                               bNode &scene_color_node,
                                                               bNode &pass_input_node,
                                                               const int scene_output_index)
{
  bNode *image_sample = bke::node_add_node(nullptr, ntree, "ShaderNodeNPR_ImageSample"_ustr);
  if (image_sample == nullptr) {
    return nullptr;
  }

  image_sample->location[0] = scene_color_node.location[0];
  image_sample->location[1] = scene_color_node.location[1];

  const std::string pass_input_identifier = "Image_" + std::to_string(scene_output_index);
  bNodeSocket *pass_input_output = bke::node_find_socket(
      pass_input_node, SOCK_OUT, UString(pass_input_identifier.c_str()));
  bNodeSocket *sample_image_input = bke::node_find_socket(*image_sample, SOCK_IN, "Image"_ustr);
  if (pass_input_output != nullptr && sample_image_input != nullptr &&
      !filter_graph_node_link_exists(
          ntree, pass_input_node, *pass_input_output, *image_sample, *sample_image_input))
  {
    bke::node_add_link(
        ntree, pass_input_node, *pass_input_output, *image_sample, *sample_image_input);
  }

  bNodeSocket *scene_vector_input = bke::node_find_socket(
      scene_color_node, SOCK_IN, "Vector"_ustr);
  bNodeSocket *sample_vector_input = filter_graph_find_image_sample_vector_input(*image_sample);
  if (scene_vector_input == nullptr || sample_vector_input == nullptr) {
    return image_sample;
  }

  Vector<bNodeLink *> vector_links;
  for (bNodeLink &link : ntree.links) {
    if (link.tonode == &scene_color_node && link.tosock == scene_vector_input &&
        link.fromnode != nullptr && link.fromsock != nullptr)
    {
      vector_links.append(&link);
    }
  }

  if (!vector_links.is_empty()) {
    image_sample->custom1 = SHD_IMG_SAMPLE_OFFSET_UV;
  }
  for (bNodeLink *link : vector_links) {
    if (!filter_graph_node_link_exists(
            ntree, *link->fromnode, *link->fromsock, *image_sample, *sample_vector_input))
    {
      bke::node_add_link(
          ntree, *link->fromnode, *link->fromsock, *image_sample, *sample_vector_input);
    }
  }

  return image_sample;
}

struct LegacyFilterGraphInput {
  int scene_output_index;
  int input_identifier;
};

static void filter_graph_route_scene_color_to_pass_input(Main &bmain,
                                                         Material &material,
                                                         bNode &pass_input_node,
                                                         Vector<LegacyFilterGraphInput> &r_inputs)
{
  bNodeTree *ntree = material.nodetree;
  if (ntree == nullptr) {
    return;
  }

  struct LinkTarget {
    int scene_output_index;
    bNode *node;
    bNodeSocket *socket;
  };

  struct SampledLinkTarget {
    bNode *scene_color_node;
    int scene_output_index;
    const char *sample_output_identifier;
    bNode *node;
    bNodeSocket *socket;
  };

  struct ImageSampleEntry {
    bNode *scene_color_node;
    int scene_output_index;
    bNode *image_sample_node;
  };

  Vector<LinkTarget> targets;
  Vector<SampledLinkTarget> sampled_targets;
  Vector<bNodeLink *> links_to_remove;
  Vector<bNode *> scene_color_nodes;
  for (bNode &node : ntree->nodes) {
    if (node.type_legacy != SH_NODE_SCENE_COLOR && !STREQ(node.idname, "ShaderNodeSceneColor")) {
      continue;
    }
    bool has_migrated_links = false;
    bool has_kept_links = false;
    for (bNodeLink &link : ntree->links) {
      if (link.fromnode != &node || link.fromsock == nullptr || link.tonode == nullptr ||
          link.tosock == nullptr)
      {
        continue;
      }
      const int scene_output_index = filter_graph_scene_color_output_index(link.fromsock->identifier);
      if (scene_output_index >= 0) {
        targets.append({scene_output_index, link.tonode, link.tosock});
        links_to_remove.append(&link);
        has_migrated_links = true;
      }
      else if (const char *sample_output_identifier =
                   filter_graph_legacy_scene_color_sample_output_identifier(
                       link.fromsock->identifier))
      {
        sampled_targets.append({&node,
                                filter_graph_legacy_scene_color_source_output_index(node),
                                sample_output_identifier,
                                link.tonode,
                                link.tosock});
        links_to_remove.append(&link);
        has_migrated_links = true;
      }
      else {
        has_kept_links = true;
      }
    }
    if (!has_kept_links) {
      scene_color_nodes.append(&node);
    }
  }

  for (bNodeLink *link : links_to_remove) {
    bke::node_remove_link(ntree, *link);
  }
  for (const LinkTarget &target : targets) {
    const std::string pass_input_identifier = "Image_" + std::to_string(target.scene_output_index);
    bNodeSocket *pass_input_output = bke::node_find_socket(
        pass_input_node, SOCK_OUT, UString(pass_input_identifier.c_str()));
    if (pass_input_output == nullptr) {
      continue;
    }
    if (!filter_graph_node_link_exists(
            *ntree, pass_input_node, *pass_input_output, *target.node, *target.socket))
    {
      bke::node_add_link(*ntree, pass_input_node, *pass_input_output, *target.node, *target.socket);
    }
  }

  Vector<ImageSampleEntry> image_sample_entries;
  for (const SampledLinkTarget &target : sampled_targets) {
    bNode *image_sample = nullptr;
    for (const ImageSampleEntry &entry : image_sample_entries) {
      if (entry.scene_color_node == target.scene_color_node &&
          entry.scene_output_index == target.scene_output_index)
      {
        image_sample = entry.image_sample_node;
        break;
      }
    }
    if (image_sample == nullptr) {
      image_sample = filter_graph_add_legacy_scene_color_image_sample(
          *ntree, *target.scene_color_node, pass_input_node, target.scene_output_index);
      if (image_sample == nullptr) {
        continue;
      }
      image_sample_entries.append(
          {target.scene_color_node, target.scene_output_index, image_sample});
    }

    bNodeSocket *sample_output = bke::node_find_socket(
        *image_sample, SOCK_OUT, UString(target.sample_output_identifier));
    if (sample_output == nullptr) {
      continue;
    }
    if (!filter_graph_node_link_exists(
            *ntree, *image_sample, *sample_output, *target.node, *target.socket))
    {
      bke::node_add_link(*ntree, *image_sample, *sample_output, *target.node, *target.socket);
    }
  }

  for (bNode *scene_color_node : scene_color_nodes) {
    bke::node_remove_node(&bmain, *ntree, *scene_color_node, true);
  }

  NodeShaderFilterGraphInput &storage = *static_cast<NodeShaderFilterGraphInput *>(
      pass_input_node.storage);
  for (int i = 0; i < 4; i++) {
    for (int item_i = 0; item_i < storage.items_num; item_i++) {
      if (storage.items[item_i].identifier == i) {
        r_inputs.append({i, i});
        break;
      }
    }
  }
}

static bool filter_graph_prepare_legacy_filter_material(
    Main &bmain, Material &material, Vector<LegacyFilterGraphInput> &r_inputs)
{
  if (material.nodetree == nullptr) {
    return false;
  }

  material.eevee_domain = MA_EEVEE_DOMAIN_FILTER;

  bool used_scene_color_outputs[4] = {true, false, false, false};
  for (bNode &node : material.nodetree->nodes) {
    if (node.type_legacy != SH_NODE_SCENE_COLOR && !STREQ(node.idname, "ShaderNodeSceneColor")) {
      continue;
    }
    for (bNodeLink &link : material.nodetree->links) {
      if (link.fromnode != &node || link.fromsock == nullptr) {
        continue;
      }
      const int scene_output_index = filter_graph_scene_color_output_index(link.fromsock->identifier);
      if (scene_output_index >= 0) {
        used_scene_color_outputs[scene_output_index] = true;
      }
      else if (filter_graph_legacy_scene_color_sample_output_identifier(link.fromsock->identifier))
      {
        used_scene_color_outputs[filter_graph_legacy_scene_color_source_output_index(node)] = true;
      }
    }
  }

  bNode *pass_input_node = filter_graph_ensure_pass_input_node(material, used_scene_color_outputs);
  if (pass_input_node == nullptr) {
    return false;
  }
  filter_graph_route_scene_color_to_pass_input(bmain, material, *pass_input_node, r_inputs);
  BKE_ntree_update_tag_all(material.nodetree);
  return true;
}

static bool filter_graph_legacy_entry_is_valid(const SceneFilterMaterial &entry)
{
  if (!entry.enabled || entry.material == nullptr || entry.material->nodetree == nullptr) {
    return false;
  }
  return filter_graph_stage_valid(entry.execution_stage);
}

static bool filter_graph_scene_has_legacy_entries(const Scene &scene)
{
  for (const SceneFilterMaterial *entry = static_cast<const SceneFilterMaterial *>(
           scene.eevee.filter_materials.first);
       entry != nullptr;
       entry = entry->next)
  {
    if (filter_graph_legacy_entry_is_valid(*entry)) {
      return true;
    }
  }
  return false;
}

static bool filter_graph_scene_stage_has_legacy_entries(const Scene &scene, const int stage)
{
  for (const SceneFilterMaterial *entry = static_cast<const SceneFilterMaterial *>(
           scene.eevee.filter_materials.first);
       entry != nullptr;
       entry = entry->next)
  {
    if (filter_graph_legacy_entry_is_valid(*entry) && entry->execution_stage == stage) {
      return true;
    }
  }
  return false;
}

static bool filter_graph_is_legacy_generated_shape(const bNodeTree &ntree)
{
  if (!STREQ(ntree.idname, eevee_filter_graph_tree_idname.c_str())) {
    return false;
  }
  for (const bNode *node : ntree.all_nodes()) {
    if (!ELEM(node->type_legacy,
              EEVEE_FILTER_GRAPH_NODE_SCENE_COLOR,
              EEVEE_FILTER_GRAPH_NODE_FILTER_MATERIAL,
              EEVEE_FILTER_GRAPH_NODE_STAGE_OUTPUT))
    {
      return false;
    }
  }
  return true;
}

static void filter_graph_clear_nodes(Main &bmain, bNodeTree &ntree)
{
  while (bNode *node = static_cast<bNode *>(ntree.nodes.first)) {
    bke::node_remove_node(&bmain, ntree, *node, true);
  }
  while (bNodeLink *link = static_cast<bNodeLink *>(ntree.links.first)) {
    bke::node_remove_link(&ntree, *link);
  }
}

static void filter_graph_clear_legacy_filter_materials(Scene &scene)
{
  for (SceneFilterMaterial *entry = static_cast<SceneFilterMaterial *>(
           scene.eevee.filter_materials.first);
       entry != nullptr;
       entry = entry->next)
  {
    if (entry->material != nullptr) {
      id_us_min(&entry->material->id);
    }
  }
  BLI_freelistN(&scene.eevee.filter_materials);
}

static void filter_graph_set_filter_pass_material(bNode &node, Material &material)
{
  if (node.id == &material.id) {
    return;
  }
  if (node.id != nullptr) {
    id_us_min(node.id);
  }
  node.id = &material.id;
  id_us_plus(node.id);
}

bool filter_graph_sync_legacy_filter_materials(Main &bmain,
                                               Scene &scene,
                                               const bool clear_legacy_materials)
{
  bNodeTree *filter_graph = scene.eevee.filter_graph;
  if (filter_graph != nullptr && !filter_graph_is_legacy_generated_shape(*filter_graph)) {
    return false;
  }

  const bool has_legacy_entries = filter_graph_scene_has_legacy_entries(scene);
  if (!has_legacy_entries) {
    if (filter_graph == nullptr) {
      return false;
    }
    filter_graph_clear_nodes(bmain, *filter_graph);
    BKE_ntree_update_tag_all(filter_graph);
    if (clear_legacy_materials) {
      filter_graph_clear_legacy_filter_materials(scene);
    }
    return true;
  }

  if (filter_graph == nullptr) {
    filter_graph = bke::node_tree_add_tree(
        &bmain, "Eevee Filter Graph", eevee_filter_graph_tree_idname);
    if (filter_graph == nullptr) {
      return false;
    }
    scene.eevee.filter_graph = filter_graph;
    id_us_plus(&filter_graph->id);
  }
  else {
    filter_graph_clear_nodes(bmain, *filter_graph);
  }

  constexpr int stages[] = {
      SCE_EEVEE_FILTER_STAGE_BEFORE_VOLUME_FOG,
      SCE_EEVEE_FILTER_STAGE_BEFORE_POSTFX,
      SCE_EEVEE_FILTER_STAGE_BEFORE_DEPTH_OF_FIELD,
      SCE_EEVEE_FILTER_STAGE_BEFORE_COMPOSITE,
  };

  int row = 0;
  for (const int stage : stages) {
    if (!filter_graph_scene_stage_has_legacy_entries(scene, stage)) {
      continue;
    }

    const float y = -260.0f * row++;
    bNode *scene_color = bke::node_add_node(
        nullptr, *filter_graph, UString("EeveeFilterGraphNodeSceneColor"));
    bNode *stage_output = bke::node_add_node(
        nullptr, *filter_graph, UString("EeveeFilterGraphNodeStageOutput"));
    if (scene_color == nullptr || stage_output == nullptr) {
      continue;
    }
    scene_color->location[0] = -520.0f;
    scene_color->location[1] = y;
    stage_output->custom1 = stage;
    stage_output->location[1] = y;

    bNodeSocket *previous_output = bke::node_find_socket(
        *scene_color, SOCK_OUT, "Color Image"_ustr);
    bNode *previous_node = scene_color;
    int pass_index = 0;

    for (SceneFilterMaterial *entry = static_cast<SceneFilterMaterial *>(
             scene.eevee.filter_materials.first);
         entry != nullptr;
         entry = entry->next)
    {
      if (!filter_graph_legacy_entry_is_valid(*entry) || entry->execution_stage != stage) {
        continue;
      }

      Material &material = *entry->material;
      Vector<LegacyFilterGraphInput> legacy_inputs;
      if (!filter_graph_prepare_legacy_filter_material(bmain, material, legacy_inputs)) {
        continue;
      }

      bNode *filter_pass = bke::node_add_node(
          nullptr, *filter_graph, UString("EeveeFilterGraphNodeFilterMaterial"));
      if (filter_pass == nullptr) {
        continue;
      }
      filter_graph_set_filter_pass_material(*filter_pass, material);
      filter_pass->location[0] = -180.0f + 320.0f * pass_index;
      filter_pass->location[1] = y;
      if (entry->name[0] != '\0') {
        STRNCPY(filter_pass->name, entry->name);
        bke::node_unique_name(*filter_graph, *filter_pass);
      }

      filter_graph_sync_filter_pass_interface_from_material_storage(*filter_graph, *filter_pass);
      update_node_declaration_and_sockets(*filter_graph, *filter_pass);

      for (const LegacyFilterGraphInput &legacy_input : legacy_inputs) {
        bNodeSocket *input = filter_graph_input_socket_for_identifier(
            *filter_pass, legacy_input.input_identifier);
        if (input == nullptr) {
          continue;
        }
        if (legacy_input.scene_output_index == 0) {
          if (previous_node != nullptr && previous_output != nullptr) {
            bke::node_add_link(*filter_graph, *previous_node, *previous_output, *filter_pass, *input);
          }
        }
        else if (const char *scene_output_identifier = filter_graph_scene_color_output_identifier(
                     legacy_input.scene_output_index))
        {
          if (bNodeSocket *scene_output = bke::node_find_socket(
                  *scene_color, SOCK_OUT, UString(scene_output_identifier)))
          {
            bke::node_add_link(*filter_graph, *scene_color, *scene_output, *filter_pass, *input);
          }
        }
      }

      if (bNodeSocket *output = filter_graph_main_output_socket(*filter_pass)) {
        previous_node = filter_pass;
        previous_output = output;
        pass_index++;
      }
    }

    stage_output->location[0] = -180.0f + 320.0f * pass_index;
    update_node_declaration_and_sockets(*filter_graph, *stage_output);
    filter_graph_stage_output_activate(*filter_graph, *stage_output);

    bNodeSocket *stage_input = bke::node_find_socket(*stage_output, SOCK_IN, "Image"_ustr);
    if (previous_node != nullptr && previous_output != nullptr && stage_input != nullptr) {
      bke::node_add_link(*filter_graph, *previous_node, *previous_output, *stage_output, *stage_input);
    }
  }

  filter_graph_stage_outputs_ensure(*filter_graph);
  BKE_ntree_update_tag_all(filter_graph);
  if (clear_legacy_materials) {
    filter_graph_clear_legacy_filter_materials(scene);
  }
  return true;
}

void filter_graph_tag_tree_changed(Main &bmain, bNodeTree &ntree)
{
  BKE_main_ensure_invariants(bmain, ntree.id);
  DEG_id_tag_update(&ntree.id, ID_RECALC_SYNC_TO_EVAL);
  WM_main_add_notifier(NC_NODE | NA_EDITED, &ntree);

  if (!STREQ(ntree.idname, eevee_filter_graph_tree_idname.c_str())) {
    return;
  }

  for (Scene &scene : bmain.scenes) {
    if (scene.eevee.filter_graph != &ntree) {
      continue;
    }
    DEG_id_tag_update(&scene.id, ID_RECALC_SYNC_TO_EVAL);
    WM_main_add_notifier(NC_SCENE | ND_NODES, &scene);
    WM_main_add_notifier(NC_SCENE | ND_RENDER_OPTIONS, &scene);
  }
}

}  // namespace blender::nodes
