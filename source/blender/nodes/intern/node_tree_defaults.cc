/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_world_types.h"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "NOD_composite.hh"
#include "NOD_defaults.hh"
#include "NOD_shader.h"

namespace blender::nodes {

static bNode *node_find_by_legacy_type(bNodeTree &ntree, const int type)
{
  for (bNode &node : ntree.nodes) {
    if (node.type_legacy == type) {
      return &node;
    }
  }
  return nullptr;
}

static bNode *output_node_find_by_legacy_type(bNodeTree &ntree, const int type)
{
  bNode *output = nullptr;
  for (bNode &node : ntree.nodes) {
    if (node.type_legacy != type) {
      continue;
    }
    if (output == nullptr || ((node.flag & NODE_DO_OUTPUT) && !(output->flag & NODE_DO_OUTPUT))) {
      output = &node;
    }
  }
  return output;
}

static bool input_has_link(const bNodeTree &ntree, const bNodeSocket &socket)
{
  for (const bNodeLink &link : ntree.links) {
    if (link.tosock == &socket) {
      return true;
    }
  }
  return false;
}

static bool float_socket_is_default_one(const bNodeSocket &socket)
{
  const bNodeSocketValueFloat *value = static_cast<const bNodeSocketValueFloat *>(
      socket.default_value);
  return value != nullptr && value->value == 1.0f;
}

static bool color_socket_is_default_white(const bNodeSocket &socket)
{
  const bNodeSocketValueRGBA *value = static_cast<const bNodeSocketValueRGBA *>(
      socket.default_value);
  return value != nullptr && value->value[0] == 1.0f && value->value[1] == 1.0f &&
         value->value[2] == 1.0f && value->value[3] == 1.0f;
}

static bool light_shader_output_inputs_are_unlinked_defaults(bNodeTree &ntree,
                                                             bNode &light_shader_output)
{
  const bNodeSocket *color = bke::node_find_socket(light_shader_output, SOCK_IN, "Color");
  const bNodeSocket *intensity = bke::node_find_socket(light_shader_output, SOCK_IN, "Intensity");
  const bNodeSocket *attenuation = bke::node_find_socket(
      light_shader_output, SOCK_IN, "Attenuation");
  return color != nullptr && intensity != nullptr && attenuation != nullptr &&
         !input_has_link(ntree, *color) && !input_has_link(ntree, *intensity) &&
         !input_has_link(ntree, *attenuation) && color_socket_is_default_white(*color) &&
         float_socket_is_default_one(*intensity) && float_socket_is_default_one(*attenuation);
}

static bool light_shader_default_nodes_link_socket(bNodeTree &ntree,
                                                   bNode &light_shader_info,
                                                   bNode &light_shader_output,
                                                   const char *from_identifier,
                                                   const char *to_identifier)
{
  bNodeSocket *from_socket = bke::node_find_socket(light_shader_info, SOCK_OUT, from_identifier);
  bNodeSocket *to_socket = bke::node_find_socket(light_shader_output, SOCK_IN, to_identifier);
  if (from_socket == nullptr || to_socket == nullptr || input_has_link(ntree, *to_socket)) {
    return false;
  }

  bke::node_add_link(ntree, light_shader_info, *from_socket, light_shader_output, *to_socket);
  return true;
}

static void light_shader_default_nodes_link(bNodeTree &ntree,
                                            bNode &light_shader_info,
                                            bNode &light_shader_output,
                                            const bool link_missing_inputs,
                                            bool &r_changed)
{
  if (!link_missing_inputs) {
    return;
  }

  r_changed |= light_shader_default_nodes_link_socket(
      ntree, light_shader_info, light_shader_output, "Default Color", "Color");
  r_changed |= light_shader_default_nodes_link_socket(
      ntree, light_shader_info, light_shader_output, "Default Intensity", "Intensity");
  r_changed |= light_shader_default_nodes_link_socket(
      ntree, light_shader_info, light_shader_output, "Default Attenuation", "Attenuation");
}

bool node_tree_light_shader_default_ensure(bNodeTree &ntree)
{
  bool changed = false;
  bool link_missing_inputs = false;
  bool added_light_shader_info = false;

  bNode *light_shader_info = node_find_by_legacy_type(ntree, SH_NODE_EEVEE_LIGHT_SHADER_INFO);
  if (light_shader_info == nullptr) {
    light_shader_info = bke::node_add_static_node(nullptr, ntree, SH_NODE_EEVEE_LIGHT_SHADER_INFO);
    if (light_shader_info == nullptr) {
      return changed;
    }
    light_shader_info->location[0] = -200.0f;
    light_shader_info->location[1] = -140.0f;
    changed = true;
    added_light_shader_info = true;
  }

  bNode *light_shader_output = output_node_find_by_legacy_type(
      ntree, SH_NODE_EEVEE_LIGHT_SHADER_OUTPUT);
  if (light_shader_output == nullptr) {
    light_shader_output = bke::node_add_static_node(
        nullptr, ntree, SH_NODE_EEVEE_LIGHT_SHADER_OUTPUT);
    if (light_shader_output == nullptr) {
      return changed;
    }
    light_shader_output->location[0] = 200.0f;
    light_shader_output->location[1] = -140.0f;
    changed = true;
    link_missing_inputs = true;
  }
  if (added_light_shader_info &&
      light_shader_output_inputs_are_unlinked_defaults(ntree, *light_shader_output))
  {
    link_missing_inputs = true;
  }
  if (!(light_shader_output->custom3 > 0.0f)) {
    light_shader_output->custom3 = 1.0f;
    changed = true;
    link_missing_inputs = true;
  }
  if (!(light_shader_output->flag & NODE_OPTIONS)) {
    light_shader_output->flag |= NODE_OPTIONS;
    changed = true;
  }

  light_shader_default_nodes_link(
      ntree, *light_shader_info, *light_shader_output, link_missing_inputs, changed);

  return changed;
}

void node_tree_shader_default(const bContext *C, Main *bmain, ID *id)
{
  if (GS(id->name) == ID_MA) {
    /* Materials */
    Object *ob = (C) ? CTX_data_active_object(C) : nullptr;
    Material *ma = reinterpret_cast<Material *>(id);
    Material *ma_default;

    if (ob && ob->type == OB_VOLUME) {
      ma_default = BKE_material_default_volume();
    }
    else {
      ma_default = BKE_material_default_surface();
    }

    if (ma->nodetree) {
      bke::node_tree_free_embedded_tree(ma->nodetree);
      MEM_delete(ma->nodetree);
      ma->nodetree = nullptr;
    }
    ma->nodetree = bke::node_tree_copy_tree(bmain, *ma_default->nodetree);
    ma->nodetree->owner_id = &ma->id;
    for (bNode *node_iter : ma->nodetree->all_nodes()) {
      STRNCPY_UTF8(node_iter->name, DATA_(node_iter->name));
      bke::node_unique_name(*ma->nodetree, *node_iter);
    }

    BKE_ntree_update_after_single_tree_change(*bmain, *ma->nodetree);
  }
  else if (ELEM(GS(id->name), ID_WO, ID_LA)) {
    /* Emission */
    bNode *shader, *output;
    bNodeTree *ntree = nullptr;

    if (GS(id->name) == ID_WO) {
      World *world = reinterpret_cast<World *>(id);
      ntree = world->nodetree;

      shader = bke::node_add_static_node(nullptr, *ntree, SH_NODE_BACKGROUND);
      output = bke::node_add_static_node(nullptr, *ntree, SH_NODE_OUTPUT_WORLD);
      bke::node_add_link(*ntree,
                         *shader,
                         *bke::node_find_socket(*shader, SOCK_OUT, "Background"),
                         *output,
                         *bke::node_find_socket(*output, SOCK_IN, "Surface"));

      bNodeSocket *color_sock = bke::node_find_socket(*shader, SOCK_IN, "Color");
      copy_v3_v3((reinterpret_cast<bNodeSocketValueRGBA *>(color_sock->default_value))->value,
                 &world->horr);
    }
    else {
      ntree = bke::node_tree_add_tree_embedded(
          nullptr, id, "Shader Nodetree", ntreeType_Shader->idname);
      shader = bke::node_add_static_node(nullptr, *ntree, SH_NODE_EMISSION);
      output = bke::node_add_static_node(nullptr, *ntree, SH_NODE_OUTPUT_LIGHT);
      bke::node_add_link(*ntree,
                         *shader,
                         *bke::node_find_socket(*shader, SOCK_OUT, "Emission"),
                         *output,
                         *bke::node_find_socket(*output, SOCK_IN, "Surface"));

      node_tree_light_shader_default_ensure(*ntree);
    }

    shader->location[0] = -200.0f;
    shader->location[1] = 100.0f;
    output->location[0] = 200.0f;
    output->location[1] = 100.0f;
    bke::node_set_active(*ntree, *output);
    BKE_ntree_update_after_single_tree_change(*bmain, *ntree);
  }
  else {
    printf("node_tree_shader_default() called on wrong ID type.\n");
    return;
  }
}

void node_tree_composit_default(const bContext *C, Scene *sce)
{
  Main *bmain = CTX_data_main(C);

  /* but lets check it anyway */
  if (sce->compositing_node_group) {
    if (G.debug & G_DEBUG) {
      printf("error in composite initialize\n");
    }
    return;
  }

  sce->compositing_node_group = bke::node_tree_add_tree(
      bmain, DATA_("Compositor Nodes"), ntreeType_Composite->idname);

  node_tree_composit_default_init(C, sce->compositing_node_group);

  BKE_ntree_update_after_single_tree_change(*bmain, *sce->compositing_node_group);
}

void node_tree_composit_default_init(const bContext *C, bNodeTree *ntree)
{
  BLI_assert(ntree != nullptr && ntree->type == NTREE_COMPOSIT);
  BLI_assert(BLI_listbase_count(&ntree->nodes) == 0);

  ntree->tree_interface.add_socket(
      DATA_("Image"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  ntree->tree_interface.add_socket(
      DATA_("Image"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);

  bNode *composite = bke::node_add_node(C, *ntree, "NodeGroupOutput");
  composite->location[0] = 200.0f;
  /* The asset shelf is visible by default, so add a small offset to keep nodes centered in the
   * visible area.*/
  composite->location[1] = 100.0f;

  bNode *in = bke::node_add_static_node(C, *ntree, CMP_NODE_R_LAYERS);
  in->location[0] = -150.0f - in->width;
  in->location[1] = 100.0f;
  bke::node_set_active(*ntree, *in);
  in->flag &= ~NODE_PREVIEW;

  bNode *reroute = bke::node_add_static_node(C, *ntree, NODE_REROUTE);
  reroute->location[0] = 100.0f;
  reroute->location[1] = 65.0f;

  bNode *viewer = bke::node_add_static_node(C, *ntree, CMP_NODE_VIEWER);
  viewer->location[0] = 200.0f;
  viewer->location[1] = 20.0f;

  /* Viewer and Composite nodes are linked to Render Layer's output image socket through a reroute
   * node. */
  bke::node_add_link(*ntree,
                     *in,
                     *reinterpret_cast<bNodeSocket *>(in->outputs.first),
                     *reroute,
                     *reinterpret_cast<bNodeSocket *>(reroute->inputs.first));

  bke::node_add_link(*ntree,
                     *reroute,
                     *reinterpret_cast<bNodeSocket *>(reroute->outputs.first),
                     *composite,
                     *reinterpret_cast<bNodeSocket *>(composite->inputs.first));

  bke::node_add_link(*ntree,
                     *reroute,
                     *reinterpret_cast<bNodeSocket *>(reroute->outputs.first),
                     *viewer,
                     *reinterpret_cast<bNodeSocket *>(viewer->inputs.first));

  BKE_ntree_update_after_single_tree_change(*CTX_data_main(C), *ntree);
}

}  // namespace blender::nodes
