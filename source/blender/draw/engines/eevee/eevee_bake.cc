/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Conservative Eevee color baking entry point. This intentionally reuses the
 * generic bake image/UV/margin pipeline and evaluates the V1-supported local
 * Eevee/NPR color graph subset.
 */

#include "eevee_bake.hh"

#include <cmath>
#include <string>

#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_light.h"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_mesh.hh"

#include "IMB_imbuf_types.hh"

#include "NOD_shader.h"

#include "RE_bake.h"
#include "RE_engine.h"
#include "RE_pipeline.h"
#include "render_types.h"

namespace blender::eevee {

namespace {

constexpr int eevee_bake_supported_pass_type = SCE_PASS_EMIT;

struct BakeLight {
  float3 position = float3(0.0f);
  float3 direction = float3(0.0f, 0.0f, -1.0f);
  float3 color = float3(0.0f);
  float energy = 0.0f;
  float diff_fac = 1.0f;
  float spec_fac = 1.0f;
  float radius = 0.0f;
  float spot_cos = 1.0f;
  float spot_blend = 0.0f;
  int type = LA_LOCAL;
  int lightgroup_id = 0;
  bool hide_diffuse = false;
  bool hide_glossy = false;
};

struct BakePixelContext {
  float3 position = float3(0.0f);
  float3 normal = float3(0.0f, 0.0f, 1.0f);
};

struct BakeSceneResources {
  Vector<BakeLight> lights;
};

struct BakeEvalContext {
  const Material *material = nullptr;
  const BakeSceneResources *resources = nullptr;
  const BakePixelContext *pixel = nullptr;
  bool used_surface_context = false;
  std::string error;
};

static void eevee_bake_report_error(RenderEngine *engine, const std::string &message)
{
  RE_engine_report(engine, RPT_ERROR, message.c_str());
  RE_engine_set_error_message(engine, message.c_str());
}

static const char *material_name(const Material *material)
{
  return material ? material->id.name + 2 : "<None>";
}

static std::string node_display_name(const bNode &node)
{
  if (node.label[0] != '\0') {
    return node.label;
  }
  if (node.name[0] != '\0') {
    return node.name;
  }
  return node.idname;
}

static bool set_error(BakeEvalContext &ctx, const std::string &message)
{
  if (ctx.error.empty()) {
    ctx.error = message;
  }
  return false;
}

static bool set_node_error(BakeEvalContext &ctx,
                           const bNode &node,
                           const StringRef reason = StringRef())
{
  std::string message =
      "Eevee Color Bake V1 supports only local Emit/Color bake nodes; material \"";
  message += material_name(ctx.material);
  message += "\" uses unsupported node \"";
  message += node_display_name(node);
  message += "\"";
  if (!reason.is_empty()) {
    message += " (";
    message += reason;
    message += ")";
  }
  return set_error(ctx, message);
}

static bool set_unsupported_node_error(BakeEvalContext &ctx,
                                       const bNode &node,
                                       const StringRef unsupported_feature)
{
  std::string message = "Eevee Color Bake V1 does not support ";
  message += unsupported_feature;
  message += " in material \"";
  message += material_name(ctx.material);
  message += "\"";
  message += " (node \"";
  message += node_display_name(node);
  message += "\")";
  return set_error(ctx, message);
}

static const bNodeLink *used_link_from_input(const bNodeSocket &socket)
{
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (link != nullptr && link->is_used() && link->fromsock != nullptr && link->fromnode != nullptr)
    {
      return link;
    }
  }
  return nullptr;
}

static const bNodeSocket *find_input(const bNode &node, const StringRef identifier)
{
  for (const bNodeSocket &socket : node.inputs) {
    if (identifier == socket.identifier) {
      return &socket;
    }
  }
  return nullptr;
}

static const bNodeSocket *find_first_input(const bNode &node, const eNodeSocketDatatype type)
{
  for (const bNodeSocket &socket : node.inputs) {
    if (socket.is_available() && socket.type == type) {
      return &socket;
    }
  }
  return nullptr;
}

static const bNodeSocket *find_first_output(const bNode &node, const eNodeSocketDatatype type)
{
  for (const bNodeSocket &socket : node.outputs) {
    if (socket.is_available() && socket.type == type) {
      return &socket;
    }
  }
  return nullptr;
}

static const bNodeSocket *find_socket_by_name_or_identifier(const ListBaseT<bNodeSocket> &sockets,
                                                            const StringRef name)
{
  for (const bNodeSocket &socket : sockets) {
    if (!socket.is_available()) {
      continue;
    }
    if (name == socket.identifier || name == socket.name) {
      return &socket;
    }
  }
  return nullptr;
}

static bool socket_matches(const bNodeSocket &socket,
                           const StringRef name,
                           const eNodeSocketDatatype type)
{
  return socket.is_available() && socket.type == type && (name == socket.identifier || name == socket.name);
}

static const bNodeSocket *find_output_by_name_or_identifier(const bNode &node,
                                                            const StringRef name,
                                                            const eNodeSocketDatatype type)
{
  for (const bNodeSocket &socket : node.outputs) {
    if (socket_matches(socket, name, type)) {
      return &socket;
    }
  }
  return nullptr;
}

static bool node_tree_contains_unsupported_bake_input(const bNodeTree &ntree,
                                                      const bNode *&r_node,
                                                      const char *&r_feature)
{
  for (const bNode *node : ntree.all_nodes()) {
    if ((node->flag & NODE_MUTED) != 0) {
      continue;
    }
    switch (node->type_legacy) {
      case SH_NODE_NPR_INPUT:
        r_node = node;
        r_feature = "NPR Input screen/GBuffer reads";
        return true;
      case SH_NODE_NPR_IMAGE_SAMPLE:
        r_node = node;
        r_feature = "NPR Image Sample";
        return true;
      case SH_NODE_NPR_REFRACTION:
        r_node = node;
        r_feature = "NPR Refraction";
        return true;
      case SH_NODE_INPUT_AOV:
        r_node = node;
        r_feature = "Input AOV";
        return true;
      case SH_NODE_SCREENSPACE_INFO:
        r_node = node;
        r_feature = "Screen Space Info";
        return true;
      case SH_NODE_SCENE_COLOR:
        r_node = node;
        r_feature = "Scene Color";
        return true;
      case SH_NODE_RENDER_TEXTURE:
        r_node = node;
        r_feature = "Render Texture feedback";
        return true;
      case SH_NODE_GLSL_FUNCTION:
        r_node = node;
        r_feature = "GLSL Function evaluation";
        return true;
      default:
        break;
    }
  }
  return false;
}

static bool validate_material_node_tree_for_bake(BakeEvalContext &ctx, bNodeTree &ntree)
{
  ntree.ensure_topology_cache();
  const bNode *unsupported_node = nullptr;
  const char *unsupported_feature = nullptr;
  if (node_tree_contains_unsupported_bake_input(ntree, unsupported_node, unsupported_feature)) {
    return set_unsupported_node_error(ctx, *unsupported_node, StringRef(unsupported_feature));
  }
  return true;
}

static bool get_bake_pixel_context(const Mesh &mesh,
                                   const Object &object,
                                   const BakePixel &pixel,
                                   const Span<int3> corner_tris,
                                   BakePixelContext &r_context)
{
  if (pixel.primitive_id < 0 || pixel.primitive_id >= corner_tris.size()) {
    return false;
  }

  const int3 tri = corner_tris[pixel.primitive_id];
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> corner_normals = mesh.corner_normals();

  float local_positions[3][3];
  float local_normals[3][3];
  for (int i = 0; i < 3; i++) {
    copy_v3_v3(local_positions[i], positions[corner_verts[tri[i]]]);
    copy_v3_v3(local_normals[i], corner_normals[tri[i]]);
  }

  float local_position[3];
  float local_normal[3];
  interp_barycentric_tri_v3(local_positions, pixel.uv[0], pixel.uv[1], local_position);
  interp_barycentric_tri_v3(local_normals, pixel.uv[0], pixel.uv[1], local_normal);

  float world_position[3];
  copy_v3_v3(world_position, local_position);
  mul_m4_v3(object.object_to_world().ptr(), world_position);

  float world_normal[3];
  copy_v3_v3(world_normal, local_normal);
  mul_transposed_mat3_m4_v3(object.world_to_object().ptr(), world_normal);
  normalize_v3(world_normal);
  if (!is_finite_v3(world_normal)) {
    world_normal[0] = 0.0f;
    world_normal[1] = 0.0f;
    world_normal[2] = 1.0f;
  }

  r_context.position = float3(world_position);
  r_context.normal = float3(world_normal);
  return true;
}

static void gather_scene_lights(Depsgraph *depsgraph, BakeSceneResources &r_resources)
{
  DEGObjectIterSettings deg_iter_settings{};
  deg_iter_settings.depsgraph = depsgraph;
  deg_iter_settings.flags = DEG_OBJECT_ITER_FOR_RENDER_ENGINE_FLAGS;

  DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, object) {
    if (object->type != OB_LAMP || object->data == nullptr) {
      continue;
    }
    const Light *light = id_cast<const Light *>(object->data);
    if ((object->visibility_flag & OB_HIDE_RENDER) != 0) {
      continue;
    }

    BakeLight bake_light;
    bake_light.position = object->object_to_world().location();
    bake_light.direction = math::normalize(float3(object->object_to_world().z_axis()));
    bake_light.color = BKE_light_color(*light);
    bake_light.energy = BKE_light_power(*light);
    bake_light.diff_fac = light->diff_fac;
    bake_light.spec_fac = light->spec_fac;
    bake_light.radius = light->radius;
    bake_light.spot_cos = cosf(light->spotsize * 0.5f);
    bake_light.spot_blend = light->spotblend;
    bake_light.type = light->type;
    bake_light.lightgroup_id = max_ii(light->lightgroup_id, 0);
    bake_light.hide_diffuse = (object->visibility_flag & OB_HIDE_DIFFUSE) != 0;
    bake_light.hide_glossy = (object->visibility_flag & OB_HIDE_GLOSSY) != 0;

    r_resources.lights.append(bake_light);
  }
  DEG_OBJECT_ITER_END;
}

static float light_spot_factor(const BakeLight &light, const float3 &light_vector)
{
  if (light.type != LA_SPOT) {
    return 1.0f;
  }

  const float cone = math::dot(light.direction, -light_vector);
  const float blend_width = (1.0f - light.spot_cos) * light.spot_blend;
  if (blend_width <= 1e-6f) {
    return cone >= light.spot_cos ? 1.0f : 0.0f;
  }
  return clamp_f((cone - light.spot_cos) / blend_width, 0.0f, 1.0f);
}

static float light_distance_attenuation(const BakeLight &light,
                                        const float3 &position,
                                        float3 &r_light_vector)
{
  if (light.type == LA_SUN) {
    r_light_vector = light.direction;
    return 1.0f;
  }

  const float3 to_light = light.position - position;
  const float distance_squared = math::length_squared(to_light);
  if (distance_squared <= 1e-12f) {
    r_light_vector = float3(0.0f, 0.0f, 1.0f);
    return 1.0f;
  }

  const float distance = sqrtf(distance_squared);
  r_light_vector = to_light / distance;
  const float radius_squared = light.radius > 0.0f ? square_f(light.radius) : 1.0f;
  const float attenuation = 1.0f / max_ff(distance_squared + radius_squared, 1e-6f);
  return attenuation * light_spot_factor(light, r_light_vector);
}

static void evaluate_shader_info(BakeEvalContext &ctx,
                                 const bNodeSocket &socket,
                                 const int lightgroup_id,
                                 const float exponent,
                                 float r_color[4])
{
  ctx.used_surface_context = true;

  zero_v4(r_color);
  r_color[3] = 1.0f;

  const BakeSceneResources *resources = ctx.resources;
  const BakePixelContext *pixel = ctx.pixel;
  if (resources == nullptr || pixel == nullptr ||
      (!socket_matches(socket, "Diffuse Shading", SOCK_RGBA) &&
       !socket_matches(socket, "Shadow", SOCK_FLOAT) &&
       !socket_matches(socket, "Ambient Lighting", SOCK_RGBA) &&
       !socket_matches(socket, "Half-Lambert Factor", SOCK_FLOAT) &&
       !socket_matches(socket, "Blinn-Phong Factor", SOCK_FLOAT)))
  {
    return;
  }

  float3 diffuse_sum = float3(0.0f);
  float visibility_sum = 0.0f;
  float shadow_weight_sum = 0.0f;
  float half_lambert_sum = 0.0f;
  float blinn_phong_sum = 0.0f;
  const float3 view_vector = float3(0.0f, 0.0f, 1.0f);
  const float safe_exponent = max_ff(exponent, 1.0f);

  for (const BakeLight &light : resources->lights) {
    if (light.lightgroup_id != lightgroup_id || light.energy <= 0.0f) {
      continue;
    }

    float3 light_vector;
    const float attenuation = light_distance_attenuation(light, pixel->position, light_vector);
    if (attenuation <= 1e-6f) {
      continue;
    }

    const float ndotl = math::dot(pixel->normal, light_vector);
    const float lambert = max_ff(ndotl, 0.0f);
    const float3 radiance = light.color * light.energy * attenuation;

    if (!light.hide_diffuse && light.diff_fac > 0.0f) {
      diffuse_sum += radiance * (light.diff_fac * lambert);
      half_lambert_sum += clamp_f(ndotl * 0.5f + 0.5f, 0.0f, 1.0f) * attenuation;
      const float diffuse_weight = light.diff_fac *
                                   max_fff(light.color.x, light.color.y, light.color.z);
      shadow_weight_sum += diffuse_weight;
      visibility_sum += diffuse_weight;
    }

    if (!light.hide_glossy && light.spec_fac > 0.0f) {
      const float3 half_vector = math::normalize(light_vector + view_vector);
      const float nh = max_ff(math::dot(pixel->normal, half_vector), 0.0f);
      blinn_phong_sum += powf(nh, safe_exponent) * light.spec_fac * attenuation;
    }
  }

  const bNode &node = socket.owner_node();
  const bNodeSocket *diffuse_socket = find_output_by_name_or_identifier(
      node, "Diffuse Shading", SOCK_RGBA);
  const bNodeSocket *shadow_socket = find_output_by_name_or_identifier(node, "Shadow", SOCK_FLOAT);
  const bNodeSocket *ambient_socket = find_output_by_name_or_identifier(
      node, "Ambient Lighting", SOCK_RGBA);
  const bNodeSocket *half_lambert_socket = find_output_by_name_or_identifier(
      node, "Half-Lambert Factor", SOCK_FLOAT);
  const bNodeSocket *blinn_socket = find_output_by_name_or_identifier(
      node, "Blinn-Phong Factor", SOCK_FLOAT);

  if (&socket == diffuse_socket) {
    copy_v3_v3(r_color, &diffuse_sum.x);
  }
  else if (&socket == shadow_socket) {
    const float shadow = shadow_weight_sum > 1e-8f ? clamp_f(visibility_sum / shadow_weight_sum,
                                                            0.0f,
                                                            1.0f) :
                                                     0.0f;
    copy_v4_fl(r_color, shadow);
    r_color[3] = 1.0f;
  }
  else if (&socket == ambient_socket) {
    zero_v3(r_color);
  }
  else if (&socket == half_lambert_socket) {
    const float half_lambert = clamp_f(half_lambert_sum, 0.0f, 1.0f);
    copy_v4_fl(r_color, half_lambert);
    r_color[3] = 1.0f;
  }
  else if (&socket == blinn_socket) {
    const float blinn = clamp_f(blinn_phong_sum, 0.0f, 1.0f);
    copy_v4_fl(r_color, blinn);
    r_color[3] = 1.0f;
  }
}

static bool eval_color_socket(BakeEvalContext &ctx,
                              const bNodeSocket &socket,
                              Set<const bNodeSocket *> &visited,
                              float r_color[4]);
static bool eval_float_socket(BakeEvalContext &ctx,
                              const bNodeSocket &socket,
                              Set<const bNodeSocket *> &visited,
                              float &r_value);
static bool eval_shader_socket(BakeEvalContext &ctx,
                               const bNodeSocket &socket,
                               Set<const bNodeSocket *> &visited,
                               float r_color[4]);

static bool eval_linked_or_default_color(BakeEvalContext &ctx,
                                         const bNodeSocket *socket,
                                         Set<const bNodeSocket *> &visited,
                                         float r_color[4])
{
  if (socket == nullptr) {
    zero_v4(r_color);
    r_color[3] = 1.0f;
    return true;
  }
  return eval_color_socket(ctx, *socket, visited, r_color);
}

static bool eval_linked_or_default_float(BakeEvalContext &ctx,
                                         const bNodeSocket *socket,
                                         Set<const bNodeSocket *> &visited,
                                         float &r_value)
{
  if (socket == nullptr) {
    r_value = 0.0f;
    return true;
  }
  return eval_float_socket(ctx, *socket, visited, r_value);
}

static bool eval_color_from_node_output(BakeEvalContext &ctx,
                                        const bNodeSocket &socket,
                                        Set<const bNodeSocket *> &visited,
                                        float r_color[4])
{
  const bNode &node = socket.owner_node();

  switch (node.type_legacy) {
    case SH_NODE_RGB: {
      const bNodeSocket *color_output = find_first_output(node, SOCK_RGBA);
      if (color_output == nullptr || color_output->default_value == nullptr) {
        return set_node_error(ctx, node, "missing RGB output");
      }
      copy_v4_v4(r_color, color_output->default_value_typed<bNodeSocketValueRGBA>()->value);
      r_color[3] = 1.0f;
      return true;
    }
    case SH_NODE_VALUE: {
      const bNodeSocket *value_output = find_first_output(node, SOCK_FLOAT);
      if (value_output == nullptr || value_output->default_value == nullptr) {
        return set_node_error(ctx, node, "missing Value output");
      }
      const float value = value_output->default_value_typed<bNodeSocketValueFloat>()->value;
      copy_v4_fl(r_color, value);
      r_color[3] = 1.0f;
      return true;
    }
    case SH_NODE_MIX: {
      const NodeShaderMix *storage = static_cast<const NodeShaderMix *>(node.storage);
      if (storage == nullptr || storage->data_type != SOCK_RGBA) {
        return set_node_error(ctx, node, "only Color Mix is supported");
      }
      if (storage->blend_type != MA_RAMP_BLEND) {
        return set_node_error(ctx, node, "only Mix blend mode is supported");
      }

      float factor = 0.0f;
      float color_a[4], color_b[4];
      if (!eval_linked_or_default_float(ctx, find_input(node, "Factor_Float"), visited, factor) ||
          !eval_linked_or_default_color(ctx, find_input(node, "A_Color"), visited, color_a) ||
          !eval_linked_or_default_color(ctx, find_input(node, "B_Color"), visited, color_b))
      {
        return false;
      }
      if (storage->clamp_factor) {
        CLAMP(factor, 0.0f, 1.0f);
      }
      interp_v4_v4v4(r_color, color_a, color_b, factor);
      if (storage->clamp_result) {
        for (int i = 0; i < 4; i++) {
          CLAMP(r_color[i], 0.0f, 1.0f);
        }
      }
      return true;
    }
    case SH_NODE_SHADER_INFO: {
      float exponent = 16.0f;
      if (!eval_linked_or_default_float(ctx, find_input(node, "Exponent"), visited, exponent)) {
        return false;
      }
      const NodeShaderShaderInfo *storage = static_cast<const NodeShaderShaderInfo *>(node.storage);
      evaluate_shader_info(
          ctx, socket, storage != nullptr ? max_ii(storage->lightgroup_id, 0) : 0, exponent, r_color);
      r_color[3] = 1.0f;
      return true;
    }
    case NODE_REROUTE: {
      const bNodeSocket *input = find_first_input(node, eNodeSocketDatatype(socket.type));
      if (input == nullptr) {
        return set_node_error(ctx, node, "reroute has no matching input");
      }
      return eval_color_socket(ctx, *input, visited, r_color);
    }
    default:
      return set_node_error(ctx, node);
  }
}

static bool eval_float_from_node_output(BakeEvalContext &ctx,
                                        const bNodeSocket &socket,
                                        Set<const bNodeSocket *> &visited,
                                        float &r_value)
{
  const bNode &node = socket.owner_node();

  switch (node.type_legacy) {
    case SH_NODE_VALUE: {
      const bNodeSocket *value_output = find_first_output(node, SOCK_FLOAT);
      if (value_output == nullptr || value_output->default_value == nullptr) {
        return set_node_error(ctx, node, "missing Value output");
      }
      r_value = value_output->default_value_typed<bNodeSocketValueFloat>()->value;
      return true;
    }
    case SH_NODE_RGB: {
      float color[4];
      if (!eval_color_from_node_output(ctx, socket, visited, color)) {
        return false;
      }
      r_value = (color[0] + color[1] + color[2]) / 3.0f;
      return true;
    }
    case SH_NODE_MATH: {
      float a = 0.0f, b = 0.0f, c = 0.0f;
      const bNodeSocket *input_a = find_socket_by_name_or_identifier(node.inputs, "Value");
      const bNodeSocket *input_b = input_a != nullptr ?
                                       find_socket_by_name_or_identifier(node.inputs, "Value_001") :
                                       nullptr;
      const bNodeSocket *input_c = input_a != nullptr ?
                                       find_socket_by_name_or_identifier(node.inputs, "Value_002") :
                                       nullptr;
      if (!eval_linked_or_default_float(ctx, input_a, visited, a) ||
          !eval_linked_or_default_float(ctx, input_b, visited, b) ||
          !eval_linked_or_default_float(ctx, input_c, visited, c))
      {
        return false;
      }

      switch (node.custom1) {
        case NODE_MATH_ADD:
          r_value = a + b;
          break;
        case NODE_MATH_SUBTRACT:
          r_value = a - b;
          break;
        case NODE_MATH_MULTIPLY:
          r_value = a * b;
          break;
        case NODE_MATH_DIVIDE:
          r_value = (b != 0.0f) ? a / b : 0.0f;
          break;
        case NODE_MATH_MINIMUM:
          r_value = min_ff(a, b);
          break;
        case NODE_MATH_MAXIMUM:
          r_value = max_ff(a, b);
          break;
        case NODE_MATH_MULTIPLY_ADD:
          r_value = a * b + c;
          break;
        case NODE_MATH_LESS_THAN:
          r_value = (a < b) ? 1.0f : 0.0f;
          break;
        case NODE_MATH_GREATER_THAN:
          r_value = (a > b) ? 1.0f : 0.0f;
          break;
        case NODE_MATH_COMPARE:
          r_value = (fabsf(a - b) < c) ? 1.0f : 0.0f;
          break;
        case NODE_MATH_FLOOR:
          r_value = floorf(a);
          break;
        case NODE_MATH_CEIL:
          r_value = ceilf(a);
          break;
        case NODE_MATH_ABSOLUTE:
          r_value = fabsf(a);
          break;
        default:
          return set_node_error(ctx, node, "unsupported Math operation");
      }
      if (node.custom2 & SHD_MATH_CLAMP) {
        CLAMP(r_value, 0.0f, 1.0f);
      }
      return true;
    }
    case SH_NODE_MIX: {
      const NodeShaderMix *storage = static_cast<const NodeShaderMix *>(node.storage);
      if (storage == nullptr || storage->data_type != SOCK_FLOAT) {
        return set_node_error(ctx, node, "only Float Mix output can be converted to a value");
      }
      float factor = 0.0f, a = 0.0f, b = 0.0f;
      if (!eval_linked_or_default_float(ctx, find_input(node, "Factor_Float"), visited, factor) ||
          !eval_linked_or_default_float(ctx, find_input(node, "A_Float"), visited, a) ||
          !eval_linked_or_default_float(ctx, find_input(node, "B_Float"), visited, b))
      {
        return false;
      }
      if (storage->clamp_factor) {
        CLAMP(factor, 0.0f, 1.0f);
      }
      r_value = interpf(b, a, factor);
      if (storage->clamp_result) {
        CLAMP(r_value, 0.0f, 1.0f);
      }
      return true;
    }
    case SH_NODE_SHADER_INFO: {
      float color[4];
      if (!eval_color_from_node_output(ctx, socket, visited, color)) {
        return false;
      }
      r_value = (color[0] + color[1] + color[2]) / 3.0f;
      return true;
    }
    case NODE_REROUTE: {
      const bNodeSocket *input = find_first_input(node, eNodeSocketDatatype(socket.type));
      if (input == nullptr) {
        return set_node_error(ctx, node, "reroute has no matching input");
      }
      return eval_float_socket(ctx, *input, visited, r_value);
    }
    default:
      return set_node_error(ctx, node);
  }
}

static bool eval_shader_from_node_output(BakeEvalContext &ctx,
                                         const bNodeSocket &socket,
                                         Set<const bNodeSocket *> &visited,
                                         float r_color[4])
{
  const bNode &node = socket.owner_node();

  switch (node.type_legacy) {
    case SH_NODE_EMISSION: {
      float color[4];
      float strength = 1.0f;
      if (!eval_linked_or_default_color(ctx, find_input(node, "Color"), visited, color) ||
          !eval_linked_or_default_float(ctx, find_input(node, "Strength"), visited, strength))
      {
        return false;
      }
      copy_v4_v4(r_color, color);
      mul_v3_fl(r_color, strength);
      r_color[3] = color[3];
      return true;
    }
    case SH_NODE_BSDF_PRINCIPLED: {
      float color[4];
      float strength = 1.0f;
      if (!eval_linked_or_default_color(ctx, find_input(node, "Emission Color"), visited, color) ||
          !eval_linked_or_default_float(ctx, find_input(node, "Emission Strength"), visited, strength))
      {
        return false;
      }
      copy_v4_v4(r_color, color);
      mul_v3_fl(r_color, strength);
      r_color[3] = color[3];
      return true;
    }
    case SH_NODE_RGB:
    case SH_NODE_VALUE:
    case SH_NODE_MIX:
    case SH_NODE_MATH:
    case SH_NODE_SHADER_INFO:
      return eval_color_from_node_output(ctx, socket, visited, r_color);
    case NODE_REROUTE: {
      const bNodeSocket *input = find_first_input(node, eNodeSocketDatatype(socket.type));
      if (input == nullptr) {
        return set_node_error(ctx, node, "reroute has no matching input");
      }
      return eval_shader_socket(ctx, *input, visited, r_color);
    }
    default:
      return set_node_error(ctx, node);
  }
}

static bool eval_color_socket(BakeEvalContext &ctx,
                              const bNodeSocket &socket,
                              Set<const bNodeSocket *> &visited,
                              float r_color[4])
{
  if (!visited.add(&socket)) {
    return set_error(ctx,
                     "Eevee Color Bake V1 found a cyclic color link in material \"" +
                         std::string(material_name(ctx.material)) + "\"");
  }

  const bNodeLink *link = socket.is_input() ? used_link_from_input(socket) : nullptr;
  const bNodeSocket *source_socket = link != nullptr ? link->fromsock : &socket;

  bool ok = true;
  if (source_socket != &socket) {
    ok = eval_color_socket(ctx, *source_socket, visited, r_color);
  }
  else if (source_socket->is_output()) {
    ok = eval_color_from_node_output(ctx, *source_socket, visited, r_color);
  }
  else if (socket.default_value != nullptr && ELEM(socket.type, SOCK_RGBA, SOCK_VECTOR)) {
    if (socket.type == SOCK_RGBA) {
      copy_v4_v4(r_color, socket.default_value_typed<bNodeSocketValueRGBA>()->value);
    }
    else {
      const float *value = socket.default_value_typed<bNodeSocketValueVector>()->value;
      copy_v3_v3(r_color, value);
      r_color[3] = 1.0f;
    }
  }
  else if (socket.default_value != nullptr && ELEM(socket.type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN))
  {
    float value = 0.0f;
    ok = eval_float_socket(ctx, socket, visited, value);
    if (ok) {
      copy_v4_fl(r_color, value);
      r_color[3] = 1.0f;
    }
  }
  else {
    ok = set_error(ctx,
                   "Eevee Color Bake V1 cannot read socket \"" + std::string(socket.name) +
                       "\" in material \"" + material_name(ctx.material) + "\"");
  }

  visited.remove(&socket);
  return ok;
}

static bool eval_float_socket(BakeEvalContext &ctx,
                              const bNodeSocket &socket,
                              Set<const bNodeSocket *> &visited,
                              float &r_value)
{
  if (!visited.add(&socket)) {
    return set_error(ctx,
                     "Eevee Color Bake V1 found a cyclic value link in material \"" +
                         std::string(material_name(ctx.material)) + "\"");
  }

  const bNodeLink *link = socket.is_input() ? used_link_from_input(socket) : nullptr;
  const bNodeSocket *source_socket = link != nullptr ? link->fromsock : &socket;

  bool ok = true;
  if (source_socket != &socket) {
    ok = eval_float_socket(ctx, *source_socket, visited, r_value);
  }
  else if (source_socket->is_output()) {
    ok = eval_float_from_node_output(ctx, *source_socket, visited, r_value);
  }
  else if (socket.default_value != nullptr) {
    switch (socket.type) {
      case SOCK_FLOAT:
        r_value = socket.default_value_typed<bNodeSocketValueFloat>()->value;
        break;
      case SOCK_INT:
        r_value = float(socket.default_value_typed<bNodeSocketValueInt>()->value);
        break;
      case SOCK_BOOLEAN:
        r_value = socket.default_value_typed<bNodeSocketValueBoolean>()->value ? 1.0f : 0.0f;
        break;
      case SOCK_RGBA: {
        const float *value = socket.default_value_typed<bNodeSocketValueRGBA>()->value;
        r_value = (value[0] + value[1] + value[2]) / 3.0f;
        break;
      }
      default:
        ok = set_error(ctx,
                       "Eevee Color Bake V1 cannot convert socket \"" + std::string(socket.name) +
                           "\" to a float in material \"" + material_name(ctx.material) + "\"");
    }
  }
  else {
    ok = set_error(ctx,
                   "Eevee Color Bake V1 cannot read socket \"" + std::string(socket.name) +
                       "\" in material \"" + material_name(ctx.material) + "\"");
  }

  visited.remove(&socket);
  return ok;
}

static bool eval_shader_socket(BakeEvalContext &ctx,
                               const bNodeSocket &socket,
                               Set<const bNodeSocket *> &visited,
                               float r_color[4])
{
  if (!visited.add(&socket)) {
    return set_error(ctx,
                     "Eevee Color Bake V1 found a cyclic shader link in material \"" +
                         std::string(material_name(ctx.material)) + "\"");
  }

  const bNodeLink *link = socket.is_input() ? used_link_from_input(socket) : nullptr;
  const bNodeSocket *source_socket = link != nullptr ? link->fromsock : &socket;

  bool ok = true;
  if (source_socket != &socket) {
    ok = eval_shader_socket(ctx, *source_socket, visited, r_color);
  }
  else if (source_socket->is_output()) {
    ok = eval_shader_from_node_output(ctx, *source_socket, visited, r_color);
  }
  else if (socket.default_value != nullptr) {
    ok = eval_color_socket(ctx, socket, visited, r_color);
  }
  else {
    ok = set_error(ctx,
                   "Eevee Color Bake V1 needs a linked emission/principled surface in material \"" +
                       std::string(material_name(ctx.material)) + "\"");
  }

  visited.remove(&socket);
  return ok;
}

static bool eval_material_surface_color(BakeEvalContext &ctx, float r_color[4])
{
  const Material *material = ctx.material;
  if (material == nullptr) {
    zero_v4(r_color);
    r_color[3] = 1.0f;
    return true;
  }

  if (material->eevee_domain == MA_EEVEE_DOMAIN_FILTER) {
    return set_error(ctx,
                     "Eevee Color Bake V1 does not support Filter-domain material \"" +
                         std::string(material_name(material)) + "\"");
  }

  if (material->nodetree == nullptr) {
    copy_v4_fl4(r_color, material->r, material->g, material->b, material->a);
    return true;
  }

  bNodeTree *ntree = material->nodetree;
  if (!validate_material_node_tree_for_bake(ctx, *ntree)) {
    return false;
  }

  if (bNodeTree *npr_tree = npr_tree_get_from_mat(const_cast<Material *>(material))) {
    if (!validate_material_node_tree_for_bake(ctx, *npr_tree)) {
      return false;
    }
    for (const bNode &node : npr_tree->nodes) {
      if (node.type_legacy != SH_NODE_NPR_OUTPUT) {
        continue;
      }
      const bNodeSocket *color_socket = find_input(node, "Color");
      if (color_socket == nullptr) {
        return set_node_error(ctx, node, "missing NPR Color input");
      }
      Set<const bNodeSocket *> visited;
      return eval_color_socket(ctx, *color_socket, visited, r_color);
    }
  }

  bNode *output = ntreeShaderOutputNode(ntree, SHD_OUTPUT_EEVEE);
  if (output == nullptr) {
    copy_v4_fl4(r_color, material->r, material->g, material->b, material->a);
    return true;
  }

  const bNodeSocket *surface_socket = find_input(*output, "Surface");
  if (surface_socket == nullptr) {
    return set_node_error(ctx, *output, "missing Surface input");
  }

  Set<const bNodeSocket *> visited;
  return eval_shader_socket(ctx, *surface_socket, visited, r_color);
}

static bool image_dimensions_match(const RenderEngine *engine, const int width, const int height)
{
  if (engine->bake.targets == nullptr) {
    return false;
  }
  const int image_id = engine->bake.image_id;
  if (image_id < 0 || image_id >= engine->bake.targets->images_num) {
    return false;
  }
  const BakeImage &image = engine->bake.targets->images[image_id];
  return image.width == width && image.height == height;
}

static Mesh *mesh_for_bake(Depsgraph *depsgraph, Object *object)
{
  Mesh *mesh = BKE_mesh_new_from_object(depsgraph, object, false, false, true);
  if (mesh == nullptr) {
    return nullptr;
  }
  if (mesh->normals_domain() == bke::MeshNormalDomain::Corner) {
    ED_mesh_split_faces(mesh);
  }
  mesh->corner_tris();
  mesh->corner_tri_faces();
  mesh->corner_normals();
  return mesh;
}

static Material *material_for_primitive(Object *object,
                                        const int primitive_id,
                                        const bke::AttributeAccessor &attributes,
                                        const Span<int> tri_faces)
{
  int material_index = 0;
  if (primitive_id >= 0 && primitive_id < tri_faces.size()) {
    const int face_i = tri_faces[primitive_id];
    const VArraySpan material_indices = *attributes.lookup<int>("material_index",
                                                                bke::AttrDomain::Face);
    if (!material_indices.is_empty() && object->totcol > 0) {
      material_index = clamp_i(material_indices[face_i], 0, object->totcol - 1);
    }
  }
  return BKE_object_material_get_eval(object, material_index + 1);
}

static bool validate_bake_request(RenderEngine *engine,
                                  Object *object,
                                  const int pass_type,
                                  const int width,
                                  const int height)
{
  if (pass_type != eevee_bake_supported_pass_type) {
    eevee_bake_report_error(engine, "Eevee Color Bake only supports the Emit bake type");
    return false;
  }
  if (object == nullptr || object->type != OB_MESH) {
    eevee_bake_report_error(engine, "Eevee Color Bake only supports mesh objects");
    return false;
  }
  if (engine->bake.targets == nullptr || engine->bake.pixels == nullptr ||
      engine->bake.result == nullptr)
  {
    eevee_bake_report_error(engine, "Eevee Color Bake requires image bake targets");
    return false;
  }
  if (engine->bake.targets->channels_num != 4) {
    eevee_bake_report_error(engine, "Eevee Color Bake requires RGBA bake targets");
    return false;
  }
  if (!image_dimensions_match(engine, width, height)) {
    eevee_bake_report_error(engine, "Eevee Color Bake image dimensions do not match bake target");
    return false;
  }
  if (engine->re != nullptr && engine->re->scene != nullptr) {
    const BakeData &bake = engine->re->scene->r.bake;
    if (bake.target != R_BAKE_TARGET_IMAGE_TEXTURES) {
      eevee_bake_report_error(engine, "Eevee Color Bake only supports Image Textures targets");
      return false;
    }
    if (bake.flag & R_BAKE_TO_ACTIVE) {
      eevee_bake_report_error(engine, "Eevee Color Bake does not support Selected to Active");
      return false;
    }
    if (bake.flag & R_BAKE_CAGE) {
      eevee_bake_report_error(engine, "Eevee Color Bake does not support cage baking");
      return false;
    }
  }
  return true;
}

}  // namespace

void eevee_bake(RenderEngine *engine,
                Depsgraph *depsgraph,
                Object *object,
                const int pass_type,
                const int /*pass_filter*/,
                const int width,
                const int height)
{
  if (!validate_bake_request(engine, object, pass_type, width, height)) {
    return;
  }

  const BakeImage &image = engine->bake.targets->images[engine->bake.image_id];
  char layer_name[RE_MAXNAME];
  SNPRINTF(layer_name, "Eevee Color Bake %d", engine->bake.image_id);

  RenderResult *result = RE_engine_begin_result(engine, 0, 0, width, height, layer_name, nullptr);
  if (result == nullptr || result->layers.first == nullptr) {
    eevee_bake_report_error(engine, "Eevee Color Bake failed to allocate render result");
    return;
  }

  RenderLayer *layer = static_cast<RenderLayer *>(result->layers.first);
  RenderPass *combined_pass = RE_pass_find_by_name(layer, RE_PASSNAME_COMBINED, "");
  if (combined_pass == nullptr || combined_pass->ibuf == nullptr ||
      combined_pass->ibuf->float_buffer.data == nullptr)
  {
    RE_engine_end_result(engine, result, true, false, false);
    eevee_bake_report_error(engine, "Eevee Color Bake failed to allocate Combined pass");
    return;
  }

  Mesh *mesh = mesh_for_bake(depsgraph, object);
  if (mesh == nullptr) {
    RE_engine_end_result(engine, result, true, false, false);
    eevee_bake_report_error(engine, "Eevee Color Bake failed to access evaluated mesh");
    return;
  }

  const Span<int> tri_faces = mesh->corner_tri_faces();
  const Span<int3> corner_tris = mesh->corner_tris();
  const bke::AttributeAccessor attributes = mesh->attributes();
  const BakePixel *pixels = engine->bake.pixels + image.offset;
  float *rect = combined_pass->ibuf->float_buffer.data;

  BakeSceneResources resources;
  gather_scene_lights(depsgraph, resources);

  Map<const Material *, float4> constant_material_colors;
  std::string error;

  for (int y = 0; y < height && error.empty(); y++) {
    for (int x = 0; x < width && error.empty(); x++) {
      const int pixel_index = y * width + x;
      const BakePixel &pixel = pixels[pixel_index];
      float *dst = rect + pixel_index * 4;

      if (pixel.object_id != engine->bake.object_id || pixel.primitive_id < 0) {
        zero_v4(dst);
        continue;
      }
      if (pixel.primitive_id >= tri_faces.size()) {
        error = "Eevee Color Bake primitive index is outside the evaluated mesh triangle range";
        break;
      }

      Material *material = material_for_primitive(
          object, pixel.primitive_id, attributes, tri_faces);
      float4 color;
      if (constant_material_colors.contains(material)) {
        color = constant_material_colors.lookup(material);
      }
      else {
        BakePixelContext pixel_context;
        if (!get_bake_pixel_context(*mesh, *object, pixel, corner_tris, pixel_context)) {
          error = "Eevee Color Bake failed to evaluate the bake pixel surface context";
          break;
        }

        BakeEvalContext ctx;
        ctx.material = material;
        ctx.resources = &resources;
        ctx.pixel = &pixel_context;
        float evaluated_color[4];
        if (!eval_material_surface_color(ctx, evaluated_color)) {
          error = ctx.error;
          break;
        }
        color = float4(evaluated_color);
        if (!ctx.used_surface_context) {
          constant_material_colors.add(material, color);
        }
      }
      copy_v4_v4(dst, color);
    }
  }

  BKE_id_free(nullptr, mesh);

  if (!error.empty()) {
    RE_engine_end_result(engine, result, true, false, false);
    eevee_bake_report_error(engine, error);
    return;
  }

  RE_engine_end_result(engine, result, false, false, false);
}

}  // namespace blender::eevee
