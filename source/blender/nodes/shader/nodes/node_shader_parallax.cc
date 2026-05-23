/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include <sstream>

#include "node_exec.hh"
#include "node_shader_util.hh"
#include "node_util.hh"

#include "BKE_image.hh"
#include "BKE_node_runtime.hh"

#include "DNA_image_types.h"
#include "DNA_node_types.h"

#include "BLI_memory_utils.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "CLG_log.h"

#include "intern/gpu_node_graph.hh"

#include "NOD_sync_sockets.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_parallax_cc {

static CLG_LogRef LOG = {"node.shader.parallax"};
static constexpr const char *parallax_helper_filename = "gpu_shader_material_parallax.glsl";
static thread_local Set<std::string> active_height_helper_keys;
static thread_local Vector<const bNode *> active_height_helper_nodes;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  const bNode *node = b.node_or_null();
  const int mode = node ? node->custom1 : SHD_PARALLAX_STEEP;
  const bool uses_height_source = mode != SHD_PARALLAX_PLANE_OFFSET;

  if (uses_height_source) {
    b.add_input<decl::Closure>("Height Source")
        .description("Closure Output or Image to Closure source sampled as height");
  }
  b.add_input<decl::Vector>("UV").implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Float>("Scale")
      .default_value(0.05f)
      .min(-FLT_MAX)
      .max(FLT_MAX)
      .description("Parallax displacement amount in UV space");
  if (uses_height_source) {
    b.add_input<decl::Float>("Midlevel")
        .default_value(0.5f)
        .min(0.0f)
        .max(1.0f)
        .description("Height value that produces no UV offset");
    b.add_input<decl::Float>("Min Steps")
        .default_value(8.0f)
        .min(1.0f)
        .max(128.0f)
        .description("Minimum samples used at near-normal view angles");
    b.add_input<decl::Float>("Max Steps")
        .default_value(32.0f)
        .min(1.0f)
        .max(128.0f)
        .description("Maximum samples used at grazing view angles");
    b.add_input<decl::Float>("Refinement Steps")
        .default_value(2.0f)
        .min(0.0f)
        .max(8.0f)
        .description("Binary refinement samples used by Parallax Occlusion mode");
  }
  b.add_output<decl::Vector>("UV").description("Parallax-adjusted UV coordinates");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = SHD_PARALLAX_STEEP;
}

static const bNodeSocket *find_node_input_socket_by_identifier(const bNode &node,
                                                               const StringRef identifier)
{
  for (const bNodeSocket *socket : node.input_sockets()) {
    if (identifier == socket->identifier) {
      return socket;
    }
  }
  return nullptr;
}

static const bNodeSocket *find_closure_output_socket_by_name(const bNode &node,
                                                             const StringRef name)
{
  if (!node.is_type("NodeClosureOutput") || node.storage == nullptr) {
    return nullptr;
  }

  const auto &storage = *static_cast<const NodeClosureOutput *>(node.storage);
  for (const int i : IndexRange(storage.output_items.items_num)) {
    const NodeClosureOutputItem &item = storage.output_items.items[i];
    if (item.name != nullptr && name == item.name) {
      return &node.input_socket(i);
    }
  }
  return nullptr;
}

static const bNode *find_localized_copy_of_original_node(const bNodeTree &tree,
                                                         const bNode &original_node)
{
  for (const bNode *node : tree.all_nodes()) {
    if (node->runtime->original == &original_node) {
      return node;
    }
  }
  return nullptr;
}

static const bNodeLink *find_used_direct_link(const bNodeSocket &socket)
{
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (link->is_used()) {
      return link;
    }
  }
  return nullptr;
}

static const bNodeLink *find_any_direct_link(const bNodeSocket &socket)
{
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (link->fromnode != nullptr) {
      return link;
    }
  }
  return nullptr;
}

static bool closure_output_has_required_height_signature(const bNode &closure_output_node,
                                                         std::string &r_error)
{
  if (!closure_output_node.is_type("NodeClosureOutput") || closure_output_node.storage == nullptr) {
    r_error = "Height Source must be a Closure Output or Image to Closure node";
    return false;
  }

  const auto &storage = *static_cast<const NodeClosureOutput *>(closure_output_node.storage);

  bool has_uv = false;
  for (const int i : IndexRange(storage.input_items.items_num)) {
    const NodeClosureInputItem &item = storage.input_items.items[i];
    if (item.name != nullptr && STREQ(item.name, "UV") && item.socket_type == SOCK_VECTOR) {
      has_uv = true;
      break;
    }
  }
  if (!has_uv) {
    r_error = "Closure Output must expose a Vector input item named 'UV'";
    return false;
  }

  const bNodeSocket *color_socket = find_closure_output_socket_by_name(closure_output_node, "Color");
  if (color_socket == nullptr || !ELEM(color_socket->type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA)) {
    r_error = "Closure Output must expose a Float, Vector, or RGBA output item named 'Color'";
    return false;
  }

  return true;
}

static void mark_node_upstream_for_height_helper(const bNode &node, Set<const bNode *> &r_visited_nodes);

static void mark_socket_upstream_for_height_helper(const bNodeSocket &socket,
                                                   Set<const bNode *> &r_visited_nodes)
{
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (!link->is_used() || link->fromnode == nullptr) {
      continue;
    }
    mark_node_upstream_for_height_helper(*link->fromnode, r_visited_nodes);
  }
}

static void mark_node_upstream_for_height_helper(const bNode &node,
                                                 Set<const bNode *> &r_visited_nodes)
{
  const bNode *node_to_mark = &node;
  const bNode *node_original = node.runtime->original ? node.runtime->original : &node;
  if (const bNode *localized_node = find_localized_copy_of_original_node(node.owner_tree(),
                                                                        *node_original))
  {
    node_to_mark = localized_node;
  }

  if (!r_visited_nodes.add(node_to_mark)) {
    return;
  }
  const_cast<bNode &>(*node_to_mark).runtime->need_exec = 1;
  for (const bNodeSocket *input_socket : node_to_mark->input_sockets()) {
    mark_socket_upstream_for_height_helper(*input_socket, r_visited_nodes);
  }
}

static GPUType gpu_node_link_output_type(const GPUNodeLink &link)
{
  return (link.link_type == GPU_NODE_LINK_OUTPUT && link.output != nullptr) ? link.output->type :
                                                                            GPU_NONE;
}

static const NodeShaderImageToClosure *image_to_closure_storage(const bNode &node)
{
  return static_cast<const NodeShaderImageToClosure *>(node.storage);
}

static int image_to_closure_texture_type(const bNode &node)
{
  const NodeShaderImageToClosure *storage = image_to_closure_storage(node);
  return storage ? storage->texture_type : IMA_IMAGE_TO_CLOSURE_TEXTURE_2D;
}

static int image_to_closure_interpolation(const bNode &node)
{
  const NodeShaderImageToClosure *storage = image_to_closure_storage(node);
  return storage ? storage->interpolation : SHD_INTERP_LINEAR;
}

static int image_to_closure_extension(const bNode &node)
{
  const NodeShaderImageToClosure *storage = image_to_closure_storage(node);
  return storage ? storage->extension : SHD_IMAGE_EXTENSION_REPEAT;
}

static GPUSamplerState sampler_state_from_image_to_closure_node(const bNode &node)
{
  GPUSamplerState sampler_state = GPUSamplerState::default_sampler();

  switch (image_to_closure_extension(node)) {
    case SHD_IMAGE_EXTENSION_EXTEND:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_EXTEND;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_EXTEND;
      break;
    case SHD_IMAGE_EXTENSION_REPEAT:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_REPEAT;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_REPEAT;
      break;
    case SHD_IMAGE_EXTENSION_CLIP:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
      break;
    case SHD_IMAGE_EXTENSION_MIRROR:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT;
      break;
    default:
      break;
  }

  if (image_to_closure_interpolation(node) != SHD_INTERP_CLOSEST) {
    sampler_state.filtering = GPU_SAMPLER_FILTERING_ANISOTROPIC |
                              GPU_SAMPLER_FILTERING_LINEAR | GPU_SAMPLER_FILTERING_MIPMAP;
  }

  return sampler_state;
}

struct HeightHelper {
  std::string helper_name;
  std::string uv_global_name;
  std::string sub_function_name;
  GPUType return_type = GPU_VEC4;
  bool uses_image = false;
};

static GPUNodeStack *find_input_stack_by_identifier(const bNode &node,
                                                    GPUNodeStack *in,
                                                    const StringRef identifier)
{
  if (in == nullptr) {
    return nullptr;
  }
  int input_index = 0;
  for (const bNodeSocket *socket : node.input_sockets()) {
    if (identifier == socket->identifier) {
      return &in[input_index];
    }
    input_index++;
  }
  return nullptr;
}

static bool resolve_height_source_link(const bNode &node, const bNodeLink *&r_link)
{
  r_link = nullptr;
  const bNodeSocket *height_socket = find_node_input_socket_by_identifier(node, "Height Source");
  if ((height_socket == nullptr || !height_socket->is_directly_linked()) && node.runtime->original) {
    height_socket = find_node_input_socket_by_identifier(*node.runtime->original, "Height Source");
  }
  if (height_socket == nullptr || !height_socket->is_directly_linked()) {
    return false;
  }
  r_link = find_used_direct_link(*height_socket);
  return r_link != nullptr && r_link->fromnode != nullptr;
}

static bool build_closure_output_height_helper(GPUMaterial *mat,
                                               const bNode &node,
                                               const bNodeLink &height_link,
                                               const StringRef wrapper_filename,
                                               const StringRef helper_name,
                                               const StringRef uv_global_name,
                                               HeightHelper &r_helper,
                                               std::string &r_error)
{
  const bNode *logical_node = node.runtime->original ? node.runtime->original : &node;
  const bNode *eval_node = &node;
  if (const bNode *localized_node = find_localized_copy_of_original_node(node.owner_tree(),
                                                                        *logical_node))
  {
    eval_node = localized_node;
  }

  const std::string helper_key = std::to_string(reinterpret_cast<uintptr_t>(eval_node));
  if (!active_height_helper_keys.add(helper_key)) {
    r_error = "Recursive Parallax Height Source dependency detected on node '" +
              std::string(logical_node->name) + "'";
    return false;
  }
  active_height_helper_nodes.append(logical_node);
  const auto release_active_helper = [&]() {
    active_height_helper_keys.remove(helper_key);
    active_height_helper_nodes.pop_last();
  };
  bool active_helper_released = false;
  BLI_SCOPED_DEFER([&]() {
    if (!active_helper_released) {
      release_active_helper();
    }
  });

  const bNode *closure_output_node = height_link.fromnode;
  const bNode *closure_output_original = closure_output_node->runtime->original ?
                                             closure_output_node->runtime->original :
                                             closure_output_node;
  if (const bNode *localized_node = find_localized_copy_of_original_node(node.owner_tree(),
                                                                        *closure_output_original))
  {
    closure_output_node = localized_node;
  }

  std::string closure_error;
  if (!closure_output_has_required_height_signature(*closure_output_node, closure_error)) {
    r_error = "Height Source Closure Output is missing required closure items: " + closure_error;
    return false;
  }

  const bNodeSocket *color_socket = find_closure_output_socket_by_name(*closure_output_node, "Color");
  const bNodeLink *color_source_link = find_any_direct_link(*color_socket);
  const bNodeSocket *sample_socket = color_socket;

  bNodeExecContext context = {};
  bNodeTree *helper_tree = const_cast<bNodeTree *>(&closure_output_node->owner_tree());
  bNodeTreeExec *exec = ntreeShaderBeginExecTree_internal(
      &context, helper_tree, bke::NODE_INSTANCE_KEY_BASE);
  if (exec == nullptr) {
    r_error = "Could not build a helper shader tree for Parallax Height Source";
    return false;
  }
  BLI_SCOPED_DEFER([&]() { ntreeShaderEndExecTree_internal(exec); });

  Vector<int> previous_need_exec;
  for (bNode &tree_node : helper_tree->nodes) {
    previous_need_exec.append(tree_node.runtime->need_exec);
    tree_node.runtime->need_exec = 0;
  }
  BLI_SCOPED_DEFER([&]() {
    int node_index = 0;
    for (bNode &tree_node : helper_tree->nodes) {
      tree_node.runtime->need_exec = previous_need_exec[node_index++];
    }
  });

  Set<const bNode *> visited_nodes;
  if (color_source_link != nullptr && color_source_link->fromnode != nullptr) {
    mark_node_upstream_for_height_helper(*color_source_link->fromnode, visited_nodes);
  }
  else {
    mark_socket_upstream_for_height_helper(*color_socket, visited_nodes);
  }

  GPU_material_closure_uv_source_push(mat, StringRefNull(uv_global_name));
  BLI_SCOPED_DEFER([&]() { GPU_material_closure_uv_source_pop(mat); });

  ntreeExecGPUNodes(exec, mat, nullptr);

  bNodeStack *color_stack = node_get_socket_stack(exec->stack, const_cast<bNodeSocket *>(sample_socket));
  GPUNodeLink *color_link = (color_stack != nullptr) ? static_cast<GPUNodeLink *>(color_stack->data) :
                                                       nullptr;
  if (color_link == nullptr && color_stack != nullptr) {
    color_link = GPU_constant(color_stack->vec);
  }
  if (color_link == nullptr) {
    r_error = "Could not evaluate Parallax Height Source Color";
    return false;
  }

  GPUType helper_return_type = GPU_VEC4;
  const GPUType output_type = gpu_node_link_output_type(*color_link);
  if (output_type == GPU_FLOAT) {
    helper_return_type = GPU_FLOAT;
  }
  else if (ELEM(output_type, GPU_VEC2, GPU_VEC3)) {
    helper_return_type = GPU_VEC3;
  }
  else if (output_type == GPU_NONE) {
    if (sample_socket->type == SOCK_FLOAT) {
      helper_return_type = GPU_FLOAT;
    }
    else if (sample_socket->type == SOCK_VECTOR) {
      helper_return_type = GPU_VEC3;
    }
  }

  release_active_helper();
  active_helper_released = true;

  const char *sub_function_name = GPU_material_split_sub_function(
      mat, helper_return_type, &color_link, StringRefNull(wrapper_filename));
  r_helper.helper_name = helper_name;
  r_helper.uv_global_name = uv_global_name;
  r_helper.sub_function_name = sub_function_name;
  r_helper.return_type = helper_return_type;
  r_helper.uses_image = false;
  return true;
}

static bool build_image_height_helper(GPUMaterial *mat,
                                      const bNodeLink &height_link,
                                      const StringRef helper_name,
                                      HeightHelper &r_helper,
                                      GPUNodeStack *height_stack)
{
  bNode *image_node = height_link.fromnode;
  if (image_node == nullptr || !image_node->is_type("ShaderNodeImageToClosure") ||
      image_to_closure_texture_type(*image_node) != IMA_IMAGE_TO_CLOSURE_TEXTURE_2D)
  {
    return false;
  }

  Image *image = id_cast<Image *>(image_node->id);
  if (image == nullptr || image->source == IMA_SRC_TILED) {
    return false;
  }

  if (height_stack == nullptr) {
    return false;
  }
  height_stack->type = GPU_TEX2D;
  height_stack->link = GPU_image(
      mat, image, nullptr, sampler_state_from_image_to_closure_node(*image_node));
  r_helper.helper_name = helper_name;
  r_helper.uses_image = true;
  r_helper.return_type = GPU_VEC4;
  return true;
}

static GPUNodeStack *ensure_uv_stack(GPUMaterial *mat, bNode *node, GPUNodeStack *in)
{
  GPUNodeStack *uv_stack = find_input_stack_by_identifier(*node, in, "UV");
  if (uv_stack != nullptr && uv_stack->link == nullptr) {
    uv_stack->link = GPU_attribute(mat, CD_AUTO_FROM_NAME, "");
    node_shader_gpu_bump_tex_coord(mat, node, &uv_stack->link);
  }
  return uv_stack;
}

static void set_fallback_outputs(GPUMaterial *mat, bNode *node, GPUNodeStack *in, GPUNodeStack *out)
{
  static float zero_value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (out == nullptr) {
    return;
  }
  GPUNodeStack *uv_stack = ensure_uv_stack(mat, node, in);
  if (uv_stack != nullptr && uv_stack->link != nullptr) {
    out[0].link = uv_stack->link;
  }
  else {
    out[0].link = GPU_constant(zero_value);
  }
}

static std::string build_height_helper_block(const HeightHelper &helper)
{
  if (helper.uses_image) {
    return "";
  }

  std::stringstream ss;
  switch (helper.return_type) {
    case GPU_FLOAT:
      ss << "float " << helper.sub_function_name << "();\n";
      break;
    case GPU_VEC3:
      ss << "float3 " << helper.sub_function_name << "();\n";
      break;
    case GPU_VEC4:
    default:
      ss << "float4 " << helper.sub_function_name << "();\n";
      break;
  }
  ss << "\n";
  ss << "float2 " << helper.uv_global_name << " = float2(0.0);\n\n";
  ss << "float " << helper.helper_name << "(float2 uv)\n{\n";
  ss << "  " << helper.uv_global_name << " = uv;\n";
  switch (helper.return_type) {
    case GPU_FLOAT:
      ss << "  return " << helper.sub_function_name << "();\n";
      break;
    case GPU_VEC3:
      ss << "  return " << helper.sub_function_name << "().x;\n";
      break;
    case GPU_VEC4:
    default:
      ss << "  return " << helper.sub_function_name << "().x;\n";
      break;
  }
  ss << "}\n\n";
  return ss.str();
}

static std::string build_wrapper_source(const StringRef wrapper_name,
                                        const HeightHelper &height_helper,
                                        const int mode)
{
  std::stringstream ss;
  ss << build_height_helper_block(height_helper);
  if (!height_helper.uses_image) {
    ss << "void node_parallax_closure_mode_" << wrapper_name
       << "(float3 uv_in, float scale, float midlevel, float min_steps, float max_steps, ";
    ss << "float refinement_steps, out float3 uv_out);\n\n";
  }
  ss << "void " << wrapper_name << "(";
  if (height_helper.uses_image) {
    ss << "sampler2D height_image, ";
  }
  ss << "float3 uv_in, float scale, float midlevel, float min_steps, float max_steps, ";
  ss << "float refinement_steps, out float3 uv_out)\n{\n";
  if (height_helper.uses_image) {
    ss << "  node_parallax_image_mode(height_image, uv_in, scale, midlevel, min_steps, ";
    ss << "max_steps, refinement_steps, " << mode << ", uv_out);\n";
  }
  else {
    ss << "  node_parallax_closure_mode_" << wrapper_name
       << "(uv_in, scale, midlevel, min_steps, max_steps, refinement_steps, uv_out);\n";
  }
  ss << "}\n";
  if (!height_helper.uses_image) {
    ss << "\nvoid node_parallax_closure_mode_" << wrapper_name
       << "(float3 uv_in, float scale, float midlevel, float min_steps, float max_steps, ";
    ss << "float refinement_steps, out float3 uv_out)\n{\n";
    ss << "  if (scale == 0.0f) {\n";
    ss << "    uv_out = uv_in;\n";
    ss << "    return;\n";
    ss << "  }\n";
    ss << "  if (" << mode << " == PARALLAX_MODE_PLANE_OFFSET) {\n";
    ss << "    node_parallax_plane_offset(uv_in, scale, uv_out);\n";
    ss << "    return;\n";
    ss << "  }\n";
    ss << "  float2 direction;\n";
    ss << "  float layer_count;\n";
    ss << "  float layer_depth;\n";
    ss << "  parallax_march_init(uv_in.xy, min_steps, max_steps, direction, layer_count, ";
    ss << "layer_depth);\n";
    ss << "  float2 delta_uv = direction * scale / max(layer_count, 1.0f);\n";
    ss << "  float2 current_uv = uv_in.xy;\n";
    ss << "  float2 previous_uv = current_uv;\n";
    ss << "  float current_depth = 0.0f;\n";
    ss << "  float previous_depth = 0.0f;\n";
    ss << "  float current_height = " << height_helper.helper_name << "(current_uv);\n";
    ss << "  for (int i = 0; i < 128; i++) {\n";
    ss << "    if (i >= int(layer_count) || current_depth >= current_height) {\n";
    ss << "      break;\n";
    ss << "    }\n";
    ss << "    previous_uv = current_uv;\n";
    ss << "    previous_depth = current_depth;\n";
    ss << "    current_uv -= delta_uv;\n";
    ss << "    current_depth += layer_depth;\n";
    ss << "    current_height = " << height_helper.helper_name << "(current_uv);\n";
    ss << "  }\n";
    ss << "  float2 result_uv = current_uv;\n";
    ss << "  if (" << mode << " == PARALLAX_MODE_OCCLUSION) {\n";
    ss << "    float2 low_uv = previous_uv;\n";
    ss << "    float2 high_uv = current_uv;\n";
    ss << "    float low_depth = previous_depth;\n";
    ss << "    float high_depth = current_depth;\n";
    ss << "    int refine_count = int(clamp(floor(refinement_steps + 0.5f), 0.0f, 8.0f));\n";
    ss << "    for (int i = 0; i < 8; i++) {\n";
    ss << "      if (i >= refine_count) {\n";
    ss << "        break;\n";
    ss << "      }\n";
    ss << "      float2 mid_uv = (low_uv + high_uv) * 0.5f;\n";
    ss << "      float mid_depth = (low_depth + high_depth) * 0.5f;\n";
    ss << "      float mid_height = " << height_helper.helper_name << "(mid_uv);\n";
    ss << "      if (mid_depth < mid_height) {\n";
    ss << "        low_uv = mid_uv;\n";
    ss << "        low_depth = mid_depth;\n";
    ss << "      }\n";
    ss << "      else {\n";
    ss << "        high_uv = mid_uv;\n";
    ss << "        high_depth = mid_depth;\n";
    ss << "      }\n";
    ss << "    }\n";
    ss << "    result_uv = (low_uv + high_uv) * 0.5f;\n";
    ss << "  }\n";
    ss << "  float2 plane_correction = direction * (midlevel * scale);\n";
    ss << "  uv_out = float3(result_uv + plane_correction, uv_in.z);\n";
    ss << "}\n";
  }
  return ss.str();
}

static int gpu_shader_parallax(GPUMaterial *mat,
                               bNode *node,
                               bNodeExecData * /*execdata*/,
                               GPUNodeStack *in,
                               GPUNodeStack *out)
{
  if (!active_height_helper_nodes.is_empty() && !GPU_material_closure_uv_source_get(mat).is_empty()) {
    const bNode *logical_node = node->runtime->original ? node->runtime->original : node;
    for (const bNode *active_logical_node : active_height_helper_nodes) {
      if (active_logical_node == logical_node) {
        set_fallback_outputs(mat, node, in, out);
        return 1;
      }
    }
  }

  const bNode *logical_node = node->runtime->original ? node->runtime->original : node;
  const std::string unique_suffix = std::to_string(logical_node->identifier);
  const std::string wrapper_name = "node_parallax_" + unique_suffix;
  const std::string wrapper_filename = "__node_parallax_" + unique_suffix + ".glsl";
  const std::string helper_name = "parallax_height_sample_" + unique_suffix;
  const std::string uv_global_name = "parallax_height_uv_" + unique_suffix;

  if (node->custom1 == SHD_PARALLAX_PLANE_OFFSET) {
    ensure_uv_stack(mat, node, in);
    return GPU_stack_link(mat, node, "node_parallax_plane_offset", in, out);
  }

  const bNodeLink *height_link = nullptr;
  if (!resolve_height_source_link(*node, height_link)) {
    CLOG_WARN(&LOG, "Parallax node '%s' is missing a Height Source; passing UV through", node->name);
    set_fallback_outputs(mat, node, in, out);
    return 1;
  }

  GPUNodeStack *height_stack = find_input_stack_by_identifier(*node, in, "Height Source");
  HeightHelper height_helper;
  if (height_link->fromnode->is_type("ShaderNodeImageToClosure")) {
    if (!build_image_height_helper(mat, *height_link, helper_name, height_helper, height_stack)) {
      CLOG_WARN(&LOG,
                "Parallax node '%s' requires a 2D Image to Closure source or a valid Closure "
                "Output source; passing UV through",
                node->name);
      set_fallback_outputs(mat, node, in, out);
      return 1;
    }
  }
  else if (height_link->fromnode->is_type("NodeClosureOutput")) {
    std::string error;
    if (!build_closure_output_height_helper(mat,
                                            *node,
                                            *height_link,
                                            wrapper_filename,
                                            helper_name,
                                            uv_global_name,
                                            height_helper,
                                            error))
    {
      CLOG_WARN(&LOG, "Parallax node '%s': %s; passing UV through", node->name, error.c_str());
      set_fallback_outputs(mat, node, in, out);
      return 1;
    }
    if (height_stack != nullptr) {
      height_stack->type = GPU_NONE;
      height_stack->link = nullptr;
    }
  }
  else {
    CLOG_WARN(&LOG,
              "Parallax node '%s' Height Source must be Image to Closure or Closure Output; "
              "passing UV through",
              node->name);
    set_fallback_outputs(mat, node, in, out);
    return 1;
  }

  ensure_uv_stack(mat, node, in);

  const std::string wrapper_source = build_wrapper_source(wrapper_name, height_helper, node->custom1);
  const Vector<StringRefNull> dependencies = {parallax_helper_filename};
  GPU_material_generated_source_add(
      mat, wrapper_filename.c_str(), dependencies, wrapper_source.c_str());

  return GPU_stack_link_custom(
      mat, node, wrapper_name.c_str(), wrapper_filename.c_str(), GPU_CUSTOM_NODE_DEPENDENCY_NONE, in, out);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  if (params.link.tonode != &params.node || params.link.tosock->type != SOCK_CLOSURE ||
      params.link.fromnode == nullptr || !params.link.fromnode->is_type("NodeClosureOutput"))
  {
    return true;
  }
  if (params.C == nullptr) {
    return true;
  }
  SpaceNode *snode = CTX_wm_space_node(params.C);
  if (snode == nullptr || snode->edittree != &params.ntree) {
    return true;
  }

  bNode &closure_output_node = *params.link.fromnode;
  const bke::bNodeZoneType *closure_zone_type = bke::zone_type_by_node_type(NODE_CLOSURE_OUTPUT);
  if (closure_zone_type == nullptr) {
    return true;
  }
  bNode *closure_input_node = closure_zone_type->get_corresponding_input(params.ntree,
                                                                         closure_output_node);
  if (closure_input_node == nullptr) {
    return true;
  }

  sync_sockets_closure(*snode, *closure_input_node, closure_output_node, nullptr, params.link.fromsock);
  return true;
}

}  // namespace nodes::node_shader_parallax_cc

void register_node_type_sh_parallax()
{
  namespace file_ns = nodes::node_shader_parallax_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeParallax", SH_NODE_PARALLAX);
  ntype.ui_name = "Parallax";
  ntype.ui_description = "Offset UV coordinates from a closure-backed height source";
  ntype.enum_name_legacy = "PARALLAX";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.initfunc = file_ns::node_init;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Middle);
  ntype.gpu_fn = file_ns::gpu_shader_parallax;
  ntype.insert_link = file_ns::node_insert_link;

  bke::node_register_type(ntype);
}

}  // namespace blender
