/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Sync-time material proxies from Nuru c603d408e9f3.
 * Traversal uses bounded physical proxies, not the NPR display graph.
 */

#include "BKE_compute_context_cache.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_object.hh"
#include "BLI_hash.hh"
#include "DNA_node_types.h"
#include "NOD_shader.h"
#include "NOD_socket_value_inference.hh"

#include "eevee_instance.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace blender::eevee {

enum HardwareMaterialProxyClosureType : uint32_t {
  HWRT_CLOSURE_DIFFUSE = 1u,
  HWRT_CLOSURE_TRANSLUCENT = 6u,
  HWRT_CLOSURE_REFLECTION = 7u,
  HWRT_CLOSURE_REFRACTION = 12u,
};

enum HardwareMaterialProxyFlags : uint32_t {
  HWRT_PROXY_FLAG_DIELECTRIC_REFLECTION = 1u << 0,
  HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL = 1u << 1,
  HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT = 1u << 2,
  HWRT_PROXY_FLAG_PRINCIPLED_TRANSMISSION_LAYER = 1u << 3,
  HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL = 1u << 4,
  HWRT_PROXY_FLAG_CAUSTICS_DISABLED = 1u << 5,
  HWRT_PROXY_FLAG_METALLIC_BSDF_SCENE_FINAL = 1u << 6,
  HWRT_PROXY_FLAG_THIN_GLASS = 1u << 7,
};

/* Bounded RT material policy:
 * - direct-light and shadow rays never evaluate hit materials,
 * - traversal / continuation run on this cheap sync-time proxy only,
 * - sparse MAT_PIPE_HIT_EVAL replay is reserved for compacted resolved hits,
 * - indirect diffuse GI only consumes emissive + diffuse-albedo data, while direct/specular
 *   fallback keeps one dominant closure family with tint/roughness/IOR. */
struct HardwareMaterialProxy {
  float3 reflection_color = float3(0.8f);
  float reflection_roughness = 1.0f;
  float3 transmission_color = float3(0.8f);
  float transmission_roughness = 1.0f;
  float reflection_ior = 1.45f;
  float refraction_ior = 1.45f;
  float packed_thickness = 0.0f;
  float alpha = 1.0f;
  float reflection_layer_coverage = 0.0f;
  uint32_t closure_type = HWRT_CLOSURE_DIFFUSE;
  uint32_t flags = 0u;
};

static float3 socket_color_value(const bNodeSocket *socket)
{
  if (socket == nullptr || socket->default_value == nullptr) {
    return float3(0.0f);
  }
  const float *rgba = static_cast<const bNodeSocketValueRGBA *>(socket->default_value)->value;
  return float3(rgba[0], rgba[1], rgba[2]);
}

static float socket_float_value(const bNodeSocket *socket)
{
  if (socket == nullptr || socket->default_value == nullptr) {
    return 0.0f;
  }
  return static_cast<const bNodeSocketValueFloat *>(socket->default_value)->value;
}

static float3 inferred_socket_color_value(const bNodeSocket *socket,
                                          nodes::SocketValueInferencer &inferencer)
{
  if (socket == nullptr) {
    return float3(0.0f);
  }
  if (const std::optional<float3> value = inferencer.get_socket_value({nullptr, socket})
                                                .get_if_primitive<float3>())
  {
    return *value;
  }
  return socket_color_value(socket);
}

static bool inferred_socket_color_is_dynamic(const bNodeSocket *socket,
                                             nodes::SocketValueInferencer &inferencer)
{
  if (socket == nullptr) {
    return false;
  }
  if (socket->link != nullptr) {
    return true;
  }
  return !inferencer.get_socket_value({nullptr, socket}).get_if_primitive<float3>().has_value();
}

static bool inferred_socket_color_is_textured(const bNodeSocket *socket,
                                              nodes::SocketValueInferencer &inferencer,
                                              const int depth = 0)
{
  if (socket == nullptr || depth > 12) {
    return false;
  }
  if (inferred_socket_color_is_dynamic(socket, inferencer)) {
    return true;
  }
  if (socket->link == nullptr) {
    return false;
  }
  const bNode *from = socket->link->fromnode;
  switch (from->type_legacy) {
    case SH_NODE_TEX_IMAGE:
      return true;
    case NODE_REROUTE: {
      const Span<const bNodeSocket *> inputs = from->input_sockets();
      return !inputs.is_empty() &&
             inferred_socket_color_is_textured(inputs[0], inferencer, depth + 1);
    }
    case SH_NODE_MIX: {
      const bNodeSocket *a_socket = bke::node_find_socket(*from, SOCK_IN, "A"_ustr);
      const bNodeSocket *b_socket = bke::node_find_socket(*from, SOCK_IN, "B"_ustr);
      return inferred_socket_color_is_textured(a_socket, inferencer, depth + 1) ||
             inferred_socket_color_is_textured(b_socket, inferencer, depth + 1);
    }
    case SH_NODE_MIX_RGB_LEGACY: {
      const bNodeSocket *color1_socket = bke::node_find_socket(*from, SOCK_IN, "Color1"_ustr);
      const bNodeSocket *color2_socket = bke::node_find_socket(*from, SOCK_IN, "Color2"_ustr);
      return inferred_socket_color_is_textured(color1_socket, inferencer, depth + 1) ||
             inferred_socket_color_is_textured(color2_socket, inferencer, depth + 1);
    }
    case SH_NODE_VALTORGB: {
      const bNodeSocket *color_socket = bke::node_find_socket(*from, SOCK_IN, "Color"_ustr);
      return inferred_socket_color_is_textured(color_socket, inferencer, depth + 1);
    }
    case SH_NODE_GAMMA:
    case SH_NODE_BRIGHTCONTRAST:
    case SH_NODE_HUE_SAT: {
      const bNodeSocket *color_socket = bke::node_find_socket(*from, SOCK_IN, "Color"_ustr);
      return inferred_socket_color_is_textured(color_socket, inferencer, depth + 1);
    }
    case SH_NODE_TEX_CHECKER:
    case SH_NODE_TEX_BRICK:
    case SH_NODE_TEX_NOISE:
    case SH_NODE_TEX_VORONOI:
    case SH_NODE_TEX_WAVE:
    case SH_NODE_TEX_MAGIC:
      return true;
    default:
      return false;
  }
}

static float inferred_socket_float_value(const bNodeSocket *socket,
                                         nodes::SocketValueInferencer &inferencer)
{
  if (socket == nullptr) {
    return 0.0f;
  }
  if (const std::optional<float> value = inferencer.get_socket_value({nullptr, socket})
                                               .get_if_primitive<float>())
  {
    return *value;
  }
  return socket_float_value(socket);
}

static bool inferred_socket_float_is_dynamic(const bNodeSocket *socket,
                                             nodes::SocketValueInferencer &inferencer)
{
  if (socket == nullptr || socket->link == nullptr) {
    return false;
  }
  /* Linked float inputs only stay static when the inferencer can constant-fold them. */
  return !inferencer.get_socket_value({nullptr, socket}).get_if_primitive<float>().has_value();
}

static float proxy_thickness_pack(const float thickness, const bool slab_mode)
{
  const float clamped_thickness = std::max(0.0f, thickness);
  const float thickness_packed = clamped_thickness / (1.0f + 2.0f * clamped_thickness);
  return slab_mode ? 1.0f - thickness_packed : thickness_packed;
}

static float material_default_thickness(const Object *ob)
{
  if (ob == nullptr) {
    return 0.0f;
  }
  float dimensions[3];
  BKE_object_dimensions_get(ob, dimensions);
  return std::max(0.0f, math::reduce_min(float3(dimensions[0], dimensions[1], dimensions[2])));
}

static float material_output_thickness(const blender::Material *blender_mat,
                                       nodes::SocketValueInferencer &inferencer)
{
  if (blender_mat == nullptr || blender_mat->nodetree == nullptr) {
    return -1.0f;
  }
  const bNode *output_node = ntreeShaderOutputNode(
      const_cast<bNodeTree *>(blender_mat->nodetree), SHD_OUTPUT_EEVEE);
  if (output_node == nullptr) {
    return -1.0f;
  }
  const bNodeSocket *thickness_socket = bke::node_find_socket(*output_node, SOCK_IN, "Thickness"_ustr);
  if (thickness_socket == nullptr) {
    return -1.0f;
  }
  if (const std::optional<float> value = inferencer.get_socket_value({nullptr, thickness_socket})
                                             .get_if_primitive<float>())
  {
    return std::max(0.0f, *value);
  }
  if (thickness_socket->link == nullptr) {
    return std::max(0.0f, socket_float_value(thickness_socket));
  }
  return -1.0f;
}

static float3 material_surface_emissive_radiance(const bNodeSocket *shader_socket,
                                                 nodes::SocketValueInferencer &inferencer,
                                                 ResourceScope &scope,
                                                 bke::ComputeContextCache &compute_context_cache,
                                                 int depth);

static float3 material_shader_node_emissive_radiance(const bNode &node,
                                                     int output_index,
                                                     nodes::SocketValueInferencer &inferencer,
                                                     ResourceScope &scope,
                                                     bke::ComputeContextCache &compute_context_cache,
                                                     int depth)
{
  if (depth > 16) {
    return float3(0.0f);
  }

  switch (node.type_legacy) {
    case SH_NODE_EMISSION: {
      const bNodeSocket *color_socket = bke::node_find_socket(node, SOCK_IN, "Color"_ustr);
      const bNodeSocket *strength_socket = bke::node_find_socket(node, SOCK_IN, "Strength"_ustr);
      return inferred_socket_color_value(color_socket, inferencer) *
             std::max(0.0f, inferred_socket_float_value(strength_socket, inferencer));
    }
    case SH_NODE_BSDF_PRINCIPLED: {
      const bNodeSocket *color_socket = bke::node_find_socket(node, SOCK_IN, "Emission Color"_ustr);
      const bNodeSocket *strength_socket = bke::node_find_socket(node, SOCK_IN, "Emission Strength"_ustr);
      return inferred_socket_color_value(color_socket, inferencer) *
             std::max(0.0f, inferred_socket_float_value(strength_socket, inferencer));
    }
    case SH_NODE_ADD_SHADER: {
      const Span<const bNodeSocket *> inputs = node.input_sockets();
      if (inputs.size() < 2) {
        return float3(0.0f);
      }
      return material_surface_emissive_radiance(
                 inputs[0], inferencer, scope, compute_context_cache, depth + 1) +
             material_surface_emissive_radiance(
                 inputs[1], inferencer, scope, compute_context_cache, depth + 1);
    }
    case SH_NODE_MIX_SHADER: {
      const Span<const bNodeSocket *> inputs = node.input_sockets();
      if (inputs.size() < 3) {
        return float3(0.0f);
      }
      const float fac = math::clamp(
          inferred_socket_float_value(inputs[0], inferencer), 0.0f, 1.0f);
      const float3 a = material_surface_emissive_radiance(
          inputs[1], inferencer, scope, compute_context_cache, depth + 1);
      const float3 b = material_surface_emissive_radiance(
          inputs[2], inferencer, scope, compute_context_cache, depth + 1);
      return a * (1.0f - fac) + b * fac;
    }
    case NODE_REROUTE: {
      const Span<const bNodeSocket *> inputs = node.input_sockets();
      return inputs.is_empty() ?
                 float3(0.0f) :
                 material_surface_emissive_radiance(
                     inputs[0], inferencer, scope, compute_context_cache, depth + 1);
    }
    case NODE_GROUP:
    case NODE_CUSTOM_GROUP: {
      const bNodeTree *group = reinterpret_cast<const bNodeTree *>(node.id);
      if (group == nullptr) {
        return float3(0.0f);
      }
      group->ensure_topology_cache();
      group->ensure_interface_cache();
      const bNode *group_output_node = group->group_output_node();
      if (group_output_node == nullptr) {
        return float3(0.0f);
      }
      const Span<const bNodeSocket *> group_inputs = node.input_sockets();
      const Span<const bNodeSocket *> group_outputs = group_output_node->input_sockets();
      if (output_index < 0 || output_index >= group_outputs.size()) {
        return float3(0.0f);
      }
      auto get_group_input_value = [&](const int group_input_i) -> nodes::InferenceValue {
        if (group_input_i < 0 || group_input_i >= group_inputs.size()) {
          return nodes::InferenceValue::Unknown();
        }
        return inferencer.get_socket_value({nullptr, group_inputs[group_input_i]});
      };
      nodes::SocketValueInferencer group_inferencer{
          *group, scope, compute_context_cache, get_group_input_value};
      return material_surface_emissive_radiance(
          group_outputs[output_index], group_inferencer, scope, compute_context_cache, depth + 1);
    }
    default:
      return float3(0.0f);
  }
}

static float3 material_surface_emissive_radiance(const bNodeSocket *shader_socket,
                                                 nodes::SocketValueInferencer &inferencer,
                                                 ResourceScope &scope,
                                                 bke::ComputeContextCache &compute_context_cache,
                                                 int depth)
{
  if (depth > 16 || shader_socket == nullptr || shader_socket->link == nullptr) {
    return float3(0.0f);
  }
  const bNodeLink *link = shader_socket->link;
  return material_shader_node_emissive_radiance(
      *link->fromnode, link->fromsock->index(), inferencer, scope, compute_context_cache, depth);
}

static const bNodeSocket *material_output_surface_socket(const blender::Material *blender_mat)
{
  const bNode *output_node = ntreeShaderOutputNode(
      const_cast<bNodeTree *>(blender_mat->nodetree), SHD_OUTPUT_EEVEE);
  if (output_node == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(*output_node, SOCK_IN, "Surface"_ustr);
}

struct LinkedShaderNode {
  const bNode *node = nullptr;
  /** Accumulated Mix/Add Shader weight along the path from the surface output. */
  float weight = 1.0f;
};

static void linked_shader_nodes_append(const bNodeSocket *shader_socket,
                                       nodes::SocketValueInferencer &inferencer,
                                       const int depth,
                                       const float weight,
                                       Vector<LinkedShaderNode> &nodes)
{
  if (shader_socket == nullptr || shader_socket->link == nullptr || depth > 16) {
    return;
  }
  const bNode *node = shader_socket->link->fromnode;
  if (node == nullptr) {
    return;
  }

  switch (node->type_legacy) {
    case NODE_REROUTE: {
      const Span<const bNodeSocket *> inputs = node->input_sockets();
      if (!inputs.is_empty()) {
        linked_shader_nodes_append(inputs[0], inferencer, depth + 1, weight, nodes);
      }
      return;
    }
    case SH_NODE_ADD_SHADER: {
      const Span<const bNodeSocket *> inputs = node->input_sockets();
      if (inputs.size() > 0) {
        linked_shader_nodes_append(inputs[0], inferencer, depth + 1, weight, nodes);
      }
      if (inputs.size() > 1) {
        linked_shader_nodes_append(inputs[1], inferencer, depth + 1, weight, nodes);
      }
      return;
    }
    case SH_NODE_MIX_SHADER: {
      const Span<const bNodeSocket *> inputs = node->input_sockets();
      if (inputs.size() < 3) {
        return;
      }
      const float fac = math::clamp(inferred_socket_float_value(inputs[0], inferencer), 0.0f, 1.0f);
      if (fac < 1.0f - 1.0e-3f) {
        linked_shader_nodes_append(inputs[1], inferencer, depth + 1, weight * (1.0f - fac), nodes);
      }
      if (fac > 1.0e-3f) {
        linked_shader_nodes_append(inputs[2], inferencer, depth + 1, weight * fac, nodes);
      }
      return;
    }
    default:
      nodes.append({node, weight});
      return;
  }
}

static Vector<LinkedShaderNode> linked_shader_nodes_weighted_from_surface(
    const blender::Material *blender_mat, nodes::SocketValueInferencer &inferencer)
{
  Vector<LinkedShaderNode> nodes;
  linked_shader_nodes_append(
      material_output_surface_socket(blender_mat), inferencer, 0, 1.0f, nodes);
  return nodes;
}

static Vector<const bNode *> linked_shader_nodes_from_surface(const blender::Material *blender_mat,
                                                             nodes::SocketValueInferencer &inferencer)
{
  Vector<const bNode *> nodes;
  for (const LinkedShaderNode &linked :
       linked_shader_nodes_weighted_from_surface(blender_mat, inferencer))
  {
    nodes.append(linked.node);
  }
  return nodes;
}

static int linked_shader_node_proxy_priority(const bNode *node)
{
  if (node == nullptr) {
    return -1;
  }
  switch (node->type_legacy) {
    case SH_NODE_BSDF_GLASS:
    case SH_NODE_BSDF_REFRACTION:
    case SH_NODE_BSDF_TRANSPARENT:
    case SH_NODE_BSDF_TRANSLUCENT:
      return 80;
    case SH_NODE_BSDF_METALLIC:
    case SH_NODE_BSDF_GLOSSY:
      return 70;
    case SH_NODE_BSDF_PRINCIPLED:
      return 60;
    case SH_NODE_BSDF_DIFFUSE:
      return 10;
    default:
      return 0;
  }
}

static float3 material_emissive_radiance(const blender::Material *blender_mat)
{
  if (blender_mat == nullptr || blender_mat->nodetree == nullptr) {
    return float3(0.0f);
  }
  blender_mat->nodetree->ensure_topology_cache();
  blender_mat->nodetree->ensure_interface_cache();
  const bNodeSocket *surface_socket = material_output_surface_socket(blender_mat);
  if (surface_socket == nullptr) {
    return float3(0.0f);
  }

  ResourceScope scope;
  bke::ComputeContextCache compute_context_cache;
  nodes::SocketValueInferencer inferencer{
      *blender_mat->nodetree, scope, compute_context_cache};
  return material_surface_emissive_radiance(
      surface_socket, inferencer, scope, compute_context_cache, 0);
}

static float3 material_diffuse_albedo(const blender::Material *blender_mat)
{
  if (blender_mat == nullptr || blender_mat->nodetree == nullptr) {
    return float3(0.8f);
  }

  blender_mat->nodetree->ensure_topology_cache();
  blender_mat->nodetree->ensure_interface_cache();
  ResourceScope scope;
  bke::ComputeContextCache compute_context_cache;
  nodes::SocketValueInferencer inferencer{
      *blender_mat->nodetree, scope, compute_context_cache};
  float3 albedo(0.8f);
  for (const bNode *node : linked_shader_nodes_from_surface(blender_mat, inferencer)) {
    switch (node->type_legacy) {
      case SH_NODE_BSDF_PRINCIPLED: {
        const bNodeSocket *color_socket = bke::node_find_socket(*node, SOCK_IN, "Base Color"_ustr);
        return inferred_socket_color_value(color_socket, inferencer);
      }
      case SH_NODE_BSDF_DIFFUSE: {
        const bNodeSocket *color_socket = bke::node_find_socket(*node, SOCK_IN, "Color"_ustr);
        return inferred_socket_color_value(color_socket, inferencer);
      }
      default:
        break;
    }
  }

  return albedo;
}

static HardwareMaterialProxy material_hardware_proxy(const blender::Material *blender_mat,
                                                     const Object *ob)
{
  HardwareMaterialProxy proxy;
  if (blender_mat == nullptr || blender_mat->nodetree == nullptr) {
    return proxy;
  }

  blender_mat->nodetree->ensure_topology_cache();
  blender_mat->nodetree->ensure_interface_cache();
  ResourceScope scope;
  bke::ComputeContextCache compute_context_cache;
  nodes::SocketValueInferencer inferencer{
      *blender_mat->nodetree, scope, compute_context_cache};
  proxy.reflection_color = material_diffuse_albedo(blender_mat);
  proxy.transmission_color = proxy.reflection_color;
  auto finalize_proxy = [&]() -> HardwareMaterialProxy {
    if ((proxy.closure_type == HWRT_CLOSURE_REFRACTION) ||
        ((proxy.flags & HWRT_PROXY_FLAG_PRINCIPLED_TRANSMISSION_LAYER) != 0u))
    {
      const float thickness = material_output_thickness(blender_mat, inferencer);
      const bool slab_mode = (blender_mat->thickness_mode == MA_THICKNESS_SLAB);
      proxy.packed_thickness = proxy_thickness_pack(
          (thickness >= 0.0f) ? thickness : material_default_thickness(ob), slab_mode);
    }
    return proxy;
  };
  /* Keep the sync-time proxy bounded: one dominant base/specular family with coarse tint,
   * roughness, IOR, and the dielectric-reflection hint for continuation branching. */

  const Vector<const bNode *> linked_nodes = linked_shader_nodes_from_surface(blender_mat,
                                                                             inferencer);
  const bNode *node = nullptr;
  int node_priority = -1;
  for (const bNode *candidate : linked_nodes) {
    const int candidate_priority = linked_shader_node_proxy_priority(candidate);
    if (candidate_priority > node_priority) {
      node = candidate;
      node_priority = candidate_priority;
    }
  }
  if (node != nullptr && ELEM(node->type_legacy, SH_NODE_BSDF_GLOSSY, SH_NODE_BSDF_METALLIC)) {
    /* Glossy/Metallic mixed or added with a diffuse-family branch (the classic
     * `Mix Shader(Diffuse, Glossy)` leather/wood setup): the diffuse branch owns the proxy.
     * Classifying the surface as a full-coverage rough mirror loses the diffuse texture in
     * reflections, which reads far worse than losing the secondary glossy sheen there. */
    const bNode *base_node = nullptr;
    int base_priority = -1;
    for (const bNode *candidate : linked_nodes) {
      if (!ELEM(candidate->type_legacy, SH_NODE_BSDF_DIFFUSE, SH_NODE_BSDF_PRINCIPLED)) {
        continue;
      }
      const int candidate_priority = linked_shader_node_proxy_priority(candidate);
      if (candidate_priority > base_priority) {
        base_node = candidate;
        base_priority = candidate_priority;
      }
    }
    if (base_node != nullptr) {
      node = base_node;
    }
  }
  if (node != nullptr) {
    switch (node->type_legacy) {
      case SH_NODE_BSDF_PRINCIPLED: {
        const float transmission = inferred_socket_float_value(
            bke::node_find_socket(*node, SOCK_IN, "Transmission Weight"_ustr), inferencer);
        const bNodeSocket *alpha_socket = bke::node_find_socket(*node, SOCK_IN, "Alpha"_ustr);
        /* Textured/varying Alpha cannot be represented by the scalar proxy. The fallback socket
         * default (usually 1.0) made alpha-cutout materials (spider webs, leaves, fences) fully
         * opaque to every HWRT shadow and continuation ray: the whole support quad cast a solid
         * black shadow / dark GI blotches. Use a half-coverage estimate instead and mark the
         * proxy alpha-transparent below so rays attenuate-and-continue (`throughput *= 1 - alpha`)
         * rather than terminate. */
        const bool alpha_textured = inferred_socket_float_is_dynamic(alpha_socket, inferencer);
        const float alpha = alpha_textured ?
                                0.5f :
                                clamp_f(inferred_socket_float_value(alpha_socket, inferencer),
                                        0.0f,
                                        1.0f);
        const float metallic = inferred_socket_float_value(
            bke::node_find_socket(*node, SOCK_IN, "Metallic"_ustr), inferencer);
        const float coat_weight = std::max(
            0.0f,
            inferred_socket_float_value(bke::node_find_socket(*node, SOCK_IN, "Coat Weight"_ustr),
                                        inferencer));
        const float coat_roughness = inferred_socket_float_value(
            bke::node_find_socket(*node, SOCK_IN, "Coat Roughness"_ustr), inferencer);
        const float coat_ior = inferred_socket_float_value(
            bke::node_find_socket(*node, SOCK_IN, "Coat IOR"_ustr), inferencer);
        const bNodeSocket *coat_tint_socket = bke::node_find_socket(*node, SOCK_IN, "Coat Tint"_ustr);
        const float3 coat_tint = (coat_tint_socket != nullptr) ?
                                     inferred_socket_color_value(coat_tint_socket, inferencer) :
                                     float3(1.0f);
        const bNodeSocket *base_color_socket = bke::node_find_socket(*node, SOCK_IN, "Base Color"_ustr);
        const float3 base_color = inferred_socket_color_value(base_color_socket, inferencer);
        const bool textured_base_color = inferred_socket_color_is_textured(base_color_socket, inferencer);
        const float roughness = inferred_socket_float_value(
            bke::node_find_socket(*node, SOCK_IN, "Roughness"_ustr), inferencer);
        const float ior = std::max(1.0e-3f,
                                   inferred_socket_float_value(
                                       bke::node_find_socket(*node, SOCK_IN, "IOR"_ustr), inferencer));
        const bool transmissive_proxy = (transmission > 1.0e-3f) && (metallic <= 0.5f);
        const bool metallic_proxy = (metallic > 1.0e-3f) && (transmission <= 1.0e-3f);
        const bool dielectric_reflection_scene_final_proxy =
            (coat_weight <= 1.0e-3f) && (metallic < 1.0f - 1.0e-3f) &&
            (transmission <= 1.0e-3f) &&
            /* Keep the layered Principled replay eligible through the full roughness range. */
            (roughness <= 1.0f) && (ior > 1.0f + 1.0e-3f);
        const bool layered_scene_final_proxy =
            (coat_weight > 1.0e-3f) ||
            /* Keep the mirrored Principled handoff continuous through metallic=1.0. The replayed
             * first-hit shader naturally fades the diffuse/base response to zero there, while the
             * coarse pure-reflection proxy branch creates a visible hard switch exactly at 1.0. */
            ((metallic > 1.0e-3f) && (transmission <= 1.0e-3f)) ||
            /* Preserve the same replay handoff through the full Principled transmission range so
             * pure transmissive hits do not snap back to the coarse glass proxy in mirrors. */
            (transmission > 1.0e-3f) ||
            dielectric_reflection_scene_final_proxy;
        /* Sparse hit replay can only preserve one dominant specular-family lobe on the fallback
         * path. Keep transmissive Principled materials tinted on that reflection branch too so
         * mirrored colored glass/coat cases do not bleach back to untinted dielectric F0. */
        proxy.reflection_color = base_color;
        proxy.transmission_color = base_color;
        proxy.reflection_roughness = (coat_weight > 1.0e-3f) ? coat_roughness : roughness;
        proxy.transmission_roughness = roughness;
        if (coat_weight > 1.0e-3f) {
          proxy.reflection_color *= coat_tint;
        }
        proxy.reflection_ior = (coat_weight > 1.0e-3f) ? std::max(1.0e-3f, coat_ior) : ior;
        proxy.refraction_ior = ior;
        proxy.alpha = alpha;
        if (alpha < 1.0f - 1.0e-3f) {
          /* Partial coverage (textured estimate above, or a constant Alpha below 1): shadow and
           * continuation rays scale throughput by `1 - alpha` and keep going instead of treating
           * the surface as a full occluder. The kernels check the refraction branch first, so
           * transmissive Principled proxies are unaffected by this flag. */
          proxy.flags |= HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT;
        }
        proxy.reflection_layer_coverage = clamp_f(metallic, 0.0f, 1.0f);
        proxy.closure_type = metallic_proxy   ? HWRT_CLOSURE_REFLECTION :
                             transmissive_proxy ? HWRT_CLOSURE_REFRACTION :
                                                  HWRT_CLOSURE_DIFFUSE;
        if (layered_scene_final_proxy) {
          /* Mirror-style scene-final reflection should replay the first layered Principled surface
           * so the combined shader stack survives secondary hits instead of collapsing to the coarse
           * pure metal / glass proxy branch. */
          proxy.flags |= HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL;
        }
        if (transmission > 1.0e-3f) {
          /* Mixed Principled reflection+transmission needs a dedicated transmitted receiver path in
           * scene-final replay instead of collapsing both branches into one dominant proxy lobe. */
          proxy.flags |= HWRT_PROXY_FLAG_PRINCIPLED_TRANSMISSION_LAYER;
        }
        if (transmissive_proxy ||
            (dielectric_reflection_scene_final_proxy && !metallic_proxy))
        {
          proxy.flags |= HWRT_PROXY_FLAG_DIELECTRIC_REFLECTION;
        }
        if (textured_base_color &&
            (metallic_proxy || transmissive_proxy || dielectric_reflection_scene_final_proxy ||
             (coat_weight > 1.0e-3f)))
        {
          proxy.flags |= HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL;
        }
        return finalize_proxy();
      }
      case SH_NODE_BSDF_GLASS:
      case SH_NODE_BSDF_REFRACTION: {
        const bNodeSocket *color_socket = bke::node_find_socket(*node, SOCK_IN, "Color"_ustr);
        const float3 color = inferred_socket_color_value(color_socket, inferencer);
        const float roughness = inferred_socket_float_value(
            bke::node_find_socket(*node, SOCK_IN, "Roughness"_ustr), inferencer);
        const float ior = std::max(1.0e-3f,
                                   inferred_socket_float_value(
                                       bke::node_find_socket(*node, SOCK_IN, "IOR"_ustr), inferencer));
        /* Keep reflected colored glass on the material tint instead of flattening the proxy
         * reflection branch back to white. Colorless glass is unchanged because `color == 1`. */
        proxy.reflection_color = color;
        proxy.transmission_color = color;
        proxy.reflection_roughness = roughness;
        proxy.transmission_roughness = roughness;
        proxy.reflection_ior = ior;
        proxy.refraction_ior = ior;
        proxy.reflection_layer_coverage = 1.0f;
        proxy.closure_type = HWRT_CLOSURE_REFRACTION;
        if (node->type_legacy == SH_NODE_BSDF_GLASS) {
          proxy.flags |= HWRT_PROXY_FLAG_DIELECTRIC_REFLECTION;
          proxy.flags |= HWRT_PROXY_FLAG_PRINCIPLED_TRANSMISSION_LAYER;
        }
        if (inferred_socket_color_is_textured(color_socket, inferencer)) {
          proxy.flags |= HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL;
        }
        return finalize_proxy();
      }
      case SH_NODE_BSDF_METALLIC: {
        const bNodeSocket *color_socket = bke::node_find_socket(*node, SOCK_IN, "Base Color"_ustr);
        const float3 color = inferred_socket_color_value(color_socket, inferencer);
        const float roughness = inferred_socket_float_value(
            bke::node_find_socket(*node, SOCK_IN, "Roughness"_ustr), inferencer);
        proxy.reflection_color = color;
        proxy.transmission_color = color;
        proxy.reflection_roughness = roughness;
        proxy.transmission_roughness = roughness;
        proxy.reflection_layer_coverage = 1.0f;
        proxy.closure_type = HWRT_CLOSURE_REFLECTION;
        if (inferred_socket_color_is_textured(color_socket, inferencer)) {
          proxy.flags |= HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL;
          proxy.flags |= HWRT_PROXY_FLAG_METALLIC_BSDF_SCENE_FINAL;
        }
        return finalize_proxy();
      }
      case SH_NODE_BSDF_GLOSSY: {
        const bNodeSocket *color_socket = bke::node_find_socket(*node, SOCK_IN, "Color"_ustr);
        const float3 color = inferred_socket_color_value(color_socket, inferencer);
        const float roughness = inferred_socket_float_value(
            bke::node_find_socket(*node, SOCK_IN, "Roughness"_ustr), inferencer);
        proxy.reflection_color = color;
        proxy.transmission_color = color;
        proxy.reflection_roughness = roughness;
        proxy.transmission_roughness = roughness;
        proxy.reflection_layer_coverage = 1.0f;
        proxy.closure_type = HWRT_CLOSURE_REFLECTION;
        if (inferred_socket_color_is_textured(color_socket, inferencer)) {
          proxy.flags |= HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL;
        }
        return finalize_proxy();
      }
      case SH_NODE_BSDF_TRANSPARENT: {
        const bNodeSocket *color_socket = bke::node_find_socket(*node, SOCK_IN, "Color"_ustr);
        const float3 color = inferred_socket_color_value(color_socket, inferencer);
        proxy.reflection_color = color;
        proxy.transmission_color = color;
        proxy.reflection_roughness = 0.0f;
        proxy.transmission_roughness = 0.0f;
        proxy.reflection_ior = 1.0f;
        proxy.refraction_ior = 1.0f;
        proxy.closure_type = HWRT_CLOSURE_REFRACTION;
        if (linked_nodes.size() > 1) {
          /* Transparent mixed with other closures (the classic textured-fac cutout pattern,
           * e.g. Mix(Transparent, Diffuse) spider webs): the surface is partially covered, not
           * a clear pane. Mark the proxy alpha-transparent with a half-coverage estimate so
           * shadow/GI/continuation rays attenuate-and-continue instead of either passing fully
           * (no shadow at all) or being lit as a solid hit (dark GI blotches). */
          proxy.alpha = 0.5f;
          proxy.flags |= HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT;
        }
        if (inferred_socket_color_is_textured(color_socket, inferencer)) {
          proxy.flags |= HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL;
        }
        return finalize_proxy();
      }
      case SH_NODE_BSDF_TRANSLUCENT: {
        const bNodeSocket *color_socket = bke::node_find_socket(*node, SOCK_IN, "Color"_ustr);
        const float3 color = inferred_socket_color_value(color_socket, inferencer);
        proxy.reflection_color = color;
        proxy.transmission_color = color;
        proxy.reflection_roughness = 0.0f;
        proxy.transmission_roughness = 0.0f;
        proxy.reflection_ior = 50.0f;
        proxy.refraction_ior = 50.0f;
        proxy.closure_type = HWRT_CLOSURE_REFRACTION;
        if (inferred_socket_color_is_textured(color_socket, inferencer)) {
          proxy.flags |= HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL;
        }
        return finalize_proxy();
      }
      case SH_NODE_BSDF_DIFFUSE: {
        const float3 color = inferred_socket_color_value(
            bke::node_find_socket(*node, SOCK_IN, "Color"_ustr), inferencer);
        const float roughness = inferred_socket_float_value(
            bke::node_find_socket(*node, SOCK_IN, "Roughness"_ustr), inferencer);
        proxy.reflection_color = color;
        proxy.transmission_color = color;
        proxy.reflection_roughness = roughness;
        proxy.transmission_roughness = roughness;
        proxy.closure_type = HWRT_CLOSURE_DIFFUSE;
        return finalize_proxy();
      }
      default:
        break;
    }
  }

  return finalize_proxy();
}

static float3 hardware_raytrace_diffuse_albedo(const blender::Material *blender_mat,
                                               const HardwareMaterialProxy &proxy)
{
  /* Fast GI and receiver gathers only need coarse diffuse transport. Transmissive proxy hits are
   * continued through the trace instead of treated as diffuse blockers or emitters. */
  if (proxy.closure_type == HWRT_CLOSURE_REFRACTION) {
    return float3(0.0f);
  }
  if ((proxy.flags & HWRT_PROXY_FLAG_THIN_GLASS) != 0u) {
    return float3(0.0f);
  }
  const float3 albedo = material_diffuse_albedo(blender_mat);
  if (proxy.closure_type == HWRT_CLOSURE_REFLECTION) {
    return albedo * (1.0f - clamp_f(proxy.reflection_layer_coverage, 0.0f, 1.0f));
  }
  return albedo;
}


static uint64_t gpu_material_runtime_hash(GPUMaterial *gpu_material)
{
  if (gpu_material == nullptr) {
    return 0;
  }

  uint64_t hash = get_default_hash(
      GPU_material_uuid_get(gpu_material),
      GPU_material_compilation_timestamp(gpu_material),
      uint64_t(GPU_material_flag(gpu_material)));
  const ListBaseT<GPUMaterialTexture> textures = GPU_material_textures(gpu_material);
  for (const GPUMaterialTexture *texture = static_cast<const GPUMaterialTexture *>(textures.first);
       texture != nullptr;
       texture = texture->next)
  {
    hash = get_default_hash(hash,
                            texture->ima,
                            texture->iuser_available ? texture->iuser.framenr : 0,
                            texture->iuser_available ? texture->iuser.tile : 0,
                            texture->iuser_available ? texture->iuser.layer : 0);
    hash = get_default_hash(hash,
                            texture->iuser_available ? texture->iuser.pass : 0,
                            texture->iuser_available ? texture->iuser.multi_index : 0,
                            texture->iuser_available ? texture->iuser.view : 0);
  }
  return hash;
}

void SyncModule::begin_sync()
{
  hardware_raytrace_sync_generation_++;
  hardware_raytrace_scene_entries_.clear();
  hardware_raytrace_scene_signature_ = 0;
}

void SyncModule::capture_hardware_raytrace_scene_entry(const ObjectHandle &ob_handle,
                                                       const Material &material,
                                                       gpu::Batch *geom,
                                                       int material_slot,
                                                       bool is_sculpt)
{
  if (!inst_.raytracing.use_hardware_tracing() || geom == nullptr) {
    return;
  }

  GPUMaterial *gpumat = material.shading.gpumat;
  blender::Material *blender_mat = gpumat ? GPU_material_get_material(gpumat) : nullptr;
  HardwareMaterialProxy proxy = material_hardware_proxy(blender_mat, ob_handle.object);
  const uint64_t material_hash = gpu_material_runtime_hash(gpumat);
  const float3 emission = material_emissive_radiance(blender_mat);
  const float3 albedo = hardware_raytrace_diffuse_albedo(blender_mat, proxy);
  if (std::getenv("BLENDER_EEVEE_HWRT_CACHE_LOG") != nullptr) {
    std::fprintf(stderr, "EEVEE HWRT hit_material name=%s ready=%d\n",
                 blender_mat ? blender_mat->id.name + 2 : "<default>",
                 material.hit_eval.gpumat != nullptr &&
                     GPU_material_status(material.hit_eval.gpumat) == GPU_MAT_SUCCESS);
  }

  /* 5.2 batches object instances into a resource range. Nuru's single object matrix cannot
   * describe that range: retain one entry and replay handle per actual instance. */
  for (const int instance : IndexRange(ob_handle.instances_count())) {
    hardware_raytrace_scene_entries_.append(
        {ObjectKey(ob_handle, instance),
         ob_handle.stable_hit_eval_object(),
         geom,
         int(ob_handle.recalc),
         ob_handle.res_handle.sub_handle(instance),
         ob_handle.object_to_world(instance),
         1,
         material_slot,
         is_sculpt,
         emission,
         albedo,
         proxy.reflection_color,
         proxy.reflection_roughness,
         proxy.transmission_color,
         proxy.transmission_roughness,
         proxy.reflection_ior,
         proxy.refraction_ior,
         proxy.packed_thickness,
         proxy.alpha,
         proxy.reflection_layer_coverage,
         proxy.closure_type,
         proxy.flags,
         material_hash,
         material.hit_eval.gpumat});
    hardware_raytrace_scene_signature_ = get_default_hash(
        hardware_raytrace_scene_signature_, ObjectKey(ob_handle, instance).hash(),
        geom, material_slot, is_sculpt, material_hash);
  }
}

}  // namespace blender::eevee
