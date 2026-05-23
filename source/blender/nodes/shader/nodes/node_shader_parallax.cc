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

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_node_runtime.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_image_types.h"
#include "DNA_node_types.h"
#include "DNA_space_types.h"

#include "BLI_memory_utils.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "CLG_log.h"

#include "intern/gpu_node_graph.hh"

#include "NOD_sync_sockets.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_parallax_cc {

static CLG_LogRef LOG = {"node.shader.parallax"};
static constexpr const char *parallax_helper_filename = "gpu_shader_material_parallax.glsl";
static constexpr const char *parallax_plane_wrapper_filename =
    "__node_parallax_plane_offset_runtime.glsl";
static constexpr const char *parallax_plane_wrapper_name =
    "node_parallax_plane_offset_runtime";
static constexpr const char *parallax_plane_normal_wrapper_name =
    "node_parallax_plane_offset_normal_runtime";
static thread_local Set<std::string> active_height_helper_keys;
static thread_local Vector<const bNode *> active_height_helper_nodes;

static int effective_mode(const int mode)
{
  const int value = mode & 0xff;
  switch (value) {
    case SHD_PARALLAX_PLANE_OFFSET:
    case SHD_PARALLAX_OCCLUSION:
    case SHD_PARALLAX_RELIEF:
    case SHD_PARALLAX_SECANT_RELIEF:
      return value;
    default:
      return SHD_PARALLAX_OCCLUSION;
  }
}

static bool node_use_shadow(const bNode &node)
{
  const NodeShaderParallax *data = static_cast<const NodeShaderParallax *>(node.storage);
  return data && data->use_shadow != 0;
}

static bool mode_uses_refinement(const int mode)
{
  return mode == SHD_PARALLAX_RELIEF || mode == SHD_PARALLAX_SECANT_RELIEF;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  const bNode *node = b.node_or_null();
  const int mode = effective_mode(node ? node->custom1 : SHD_PARALLAX_OCCLUSION);
  const bool uses_height_source = mode != SHD_PARALLAX_PLANE_OFFSET;
  const bool uses_refinement = mode_uses_refinement(mode);
  const bool uses_shadow = uses_height_source && node && node_use_shadow(*node);

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
    b.add_input<decl::Float>("Offset")
        .default_value(0.0f)
        .min(-1.0f)
        .max(1.0f)
        .description("Bias added to the sampled height");
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
    if (uses_refinement) {
      b.add_input<decl::Float>("Refinement Steps")
          .default_value(2.0f)
          .min(0.0f)
          .max(8.0f)
          .description("Intersection refinement samples used by relief modes");
    }
    if (uses_shadow) {
      b.add_input<decl::Vector>("Sun Direction (World Space)")
          .default_value({0.0f, 0.0f, 1.0f})
          .description("Single directional light direction in world space for parallax shadow");
    }
  }
  b.add_output<decl::Vector>("UV").description("Parallax-adjusted UV coordinates");
  b.add_output<decl::Vector>("Normal").description("Normal estimated from the parallax height source");
  if (uses_shadow) {
    b.add_output<decl::Float>("Shadow").description("Directional parallax shadow factor");
  }
}

static const char *node_uv_map(const bNode &node)
{
  const NodeShaderParallax *data = static_cast<const NodeShaderParallax *>(node.storage);
  return data ? data->uv_map : "";
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  bNode *node = static_cast<bNode *>(ptr->data);
  if (node && effective_mode(node->custom1) != SHD_PARALLAX_PLANE_OFFSET) {
    layout.prop(ptr, "use_shadow", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }

  PointerRNA obptr = CTX_data_pointer_get(C, "active_object");
  Object *object = static_cast<Object *>(obptr.data);
  if (object && object->type == OB_MESH) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    if (depsgraph) {
      Object *object_eval = DEG_get_evaluated(depsgraph, object);
      PointerRNA dataptr = RNA_id_pointer_create(object_eval->data);
      layout.prop_search(ptr, "uv_map", &dataptr, "uv_layers", "", ICON_GROUP_UVS);
      return;
    }
  }

  layout.prop(ptr, "uv_map", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_GROUP_UVS);
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = SHD_PARALLAX_OCCLUSION;
  node->custom2 = 0;
  node->storage = MEM_new<NodeShaderParallax>("NodeShaderParallax");
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

  const bNodeSocket *height_socket = find_closure_output_socket_by_name(closure_output_node,
                                                                       "Height");
  if (height_socket == nullptr || height_socket->type != SOCK_FLOAT) {
    r_error = "Closure Output must expose a Float output item named 'Height'";
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

  const bNodeSocket *height_socket = find_closure_output_socket_by_name(*closure_output_node,
                                                                       "Height");
  const bNodeLink *height_source_link = find_any_direct_link(*height_socket);
  const bNodeSocket *sample_socket = height_socket;

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
  if (height_source_link != nullptr && height_source_link->fromnode != nullptr) {
    mark_node_upstream_for_height_helper(*height_source_link->fromnode, visited_nodes);
  }
  else {
    mark_socket_upstream_for_height_helper(*height_socket, visited_nodes);
  }

  GPU_material_closure_uv_source_push(mat, StringRefNull(uv_global_name));
  BLI_SCOPED_DEFER([&]() { GPU_material_closure_uv_source_pop(mat); });
  const std::string uv_dx_global_name = uv_global_name + "_dx";
  const std::string uv_dy_global_name = uv_global_name + "_dy";
  GPU_material_closure_uv_gradient_source_push(
      mat, StringRefNull(uv_dx_global_name), StringRefNull(uv_dy_global_name));
  BLI_SCOPED_DEFER([&]() { GPU_material_closure_uv_gradient_source_pop(mat); });

  ntreeExecGPUNodes(exec, mat, nullptr);

  bNodeStack *height_stack = node_get_socket_stack(exec->stack,
                                                  const_cast<bNodeSocket *>(sample_socket));
  GPUNodeLink *height_gpu_link = (height_stack != nullptr) ?
                                     static_cast<GPUNodeLink *>(height_stack->data) :
                                     nullptr;
  if (height_gpu_link == nullptr && height_stack != nullptr) {
    height_gpu_link = GPU_constant(height_stack->vec);
  }
  if (height_gpu_link == nullptr) {
    r_error = "Could not evaluate Parallax Height Source Height";
    return false;
  }

  release_active_helper();
  active_helper_released = true;

  const char *sub_function_name = GPU_material_split_sub_function(
      mat, GPU_FLOAT, &height_gpu_link, StringRefNull(wrapper_filename));
  r_helper.helper_name = helper_name;
  r_helper.uv_global_name = uv_global_name;
  r_helper.sub_function_name = sub_function_name;
  r_helper.return_type = GPU_FLOAT;
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
  static float one_value[4] = {1.0f, 1.0f, 1.0f, 1.0f};
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
  if (!out[1].end) {
    GPU_link(mat, "world_normals_get", &out[1].link);
  }
  if (!out[2].end) {
    out[2].link = GPU_constant(one_value);
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
  ss << "float2 " << helper.uv_global_name << " = float2(0.0);\n";
  ss << "float2 " << helper.uv_global_name << "_dx = float2(0.0);\n";
  ss << "float2 " << helper.uv_global_name << "_dy = float2(0.0);\n\n";
  ss << "float " << helper.helper_name << "(float2 uv, float2 uv_dx, float2 uv_dy)\n{\n";
  ss << "  " << helper.uv_global_name << " = uv;\n";
  ss << "  " << helper.uv_global_name << "_dx = uv_dx;\n";
  ss << "  " << helper.uv_global_name << "_dy = uv_dy;\n";
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
                                        const int mode,
                                        const bool use_normal,
                                        const bool has_shadow_input,
                                        const bool compute_shadow)
{
  std::stringstream ss;
  const bool uses_refinement = mode_uses_refinement(mode);
  ss << build_height_helper_block(height_helper);
  if (!height_helper.uses_image) {
    ss << "void node_parallax_closure_mode_" << wrapper_name
       << "(float3 uv_in, float scale, float height_offset, ";
    ss << "float min_steps, float max_steps, ";
    if (uses_refinement) {
      ss << "float refinement_steps, ";
    }
    if (has_shadow_input) {
      ss << "float3 sun_direction, ";
    }
    ss << "float4 tangent, out float3 uv_out, out float3 normal_out, out float shadow_out";
    ss << ");\n\n";
  }
  ss << "void " << wrapper_name << "(";
  if (height_helper.uses_image) {
    ss << "sampler2D height_image, ";
  }
  ss << "float3 uv_in, float scale, float height_offset, ";
  ss << "float min_steps, float max_steps, ";
  if (uses_refinement) {
    ss << "float refinement_steps, ";
  }
  if (has_shadow_input) {
    ss << "float3 sun_direction, ";
  }
  ss << "float4 tangent, out float3 uv_out";
  if (use_normal) {
    ss << ", out float3 normal_out";
  }
  if (compute_shadow) {
    ss << ", out float shadow_out";
  }
  ss << ")\n{\n";
  if (height_helper.uses_image) {
    if (!use_normal) {
      ss << "  float3 normal_tmp;\n";
    }
    if (!compute_shadow) {
      ss << "  float shadow_tmp;\n";
    }
    ss << "  node_parallax_image_mode(height_image, uv_in, scale, height_offset, ";
    ss << "min_steps, max_steps, ";
    ss << (uses_refinement ? "refinement_steps" : "0.0f") << ", " << mode;
    ss << ", " << (use_normal ? "true" : "false") << ", "
       << (compute_shadow ? "true" : "false") << ", ";
    if (has_shadow_input) {
      ss << "sun_direction, ";
    }
    else {
      ss << "float3(0.0f, 0.0f, 1.0f), ";
    }
    ss << "tangent, uv_out, " << (use_normal ? "normal_out" : "normal_tmp") << ", "
       << (compute_shadow ? "shadow_out" : "shadow_tmp") << ");\n";
  }
  else {
    if (!use_normal) {
      ss << "  float3 normal_tmp;\n";
    }
    if (!compute_shadow) {
      ss << "  float shadow_tmp;\n";
    }
    ss << "  node_parallax_closure_mode_" << wrapper_name
       << "(uv_in, scale, height_offset, min_steps, max_steps, ";
    if (uses_refinement) {
      ss << "refinement_steps, ";
    }
    if (has_shadow_input) {
      ss << "sun_direction, ";
    }
    ss << "tangent, uv_out";
    ss << ", " << (use_normal ? "normal_out" : "normal_tmp");
    if (compute_shadow) {
      ss << ", shadow_out";
    }
    else {
      ss << ", shadow_tmp";
    }
    ss << ");\n";
  }
  ss << "}\n";
  if (!height_helper.uses_image) {
    ss << "\nvoid node_parallax_closure_mode_" << wrapper_name
       << "(float3 uv_in, float scale, float height_offset, ";
    ss << "float min_steps, float max_steps, ";
    if (uses_refinement) {
      ss << "float refinement_steps, ";
    }
    if (has_shadow_input) {
      ss << "float3 sun_direction, ";
    }
    ss << "float4 tangent, out float3 uv_out, out float3 normal_out, out float shadow_out";
    ss << ")\n{\n";
    ss << "  shadow_out = 1.0f;\n";
    ss << "  normal_out = parallax_base_normal(tangent);\n";
    if (!uses_refinement) {
      ss << "  float refinement_steps = 0.0f;\n";
    }
    ss << "  if (scale == 0.0f) {\n";
    ss << "    uv_out = uv_in;\n";
    ss << "    return;\n";
    ss << "  }\n";
    ss << "  if (" << mode << " == PARALLAX_MODE_PLANE_OFFSET) {\n";
    ss << "    node_parallax_plane_offset(uv_in, scale, tangent, uv_out, normal_out);\n";
    ss << "    return;\n";
    ss << "  }\n";
    ss << "  float2 direction;\n";
    ss << "  float layer_count;\n";
    ss << "  float layer_depth;\n";
    ss << "  parallax_march_init(tangent, min_steps, max_steps, direction, ";
    ss << "layer_count, layer_depth);\n";
    ss << "  float2 uv_dx;\n";
    ss << "  float2 uv_dy;\n";
    ss << "  parallax_uv_gradients(uv_in, uv_dx, uv_dy);\n";
    ss << "  float2 delta_uv = direction * scale / max(layer_count, 1.0f);\n";
    ss << "  float2 previous_uv = uv_in.xy;\n";
    ss << "  float previous_ray_height = 1.0f;\n";
    ss << "  float2 current_uv = uv_in.xy - delta_uv;\n";
    ss << "  float current_height = " << height_helper.helper_name;
    ss << "(current_uv, uv_dx, uv_dy) + height_offset;\n";
    ss << "  float previous_height = " << height_helper.helper_name;
    ss << "(uv_in.xy, uv_dx, uv_dy) + height_offset;\n";
    ss << "  float ray_height = 1.0f - layer_depth;\n";
    ss << "  for (int i = 0; i < 128; i++) {\n";
    ss << "    if (i >= int(layer_count) || current_height > ray_height) {\n";
    ss << "      break;\n";
    ss << "    }\n";
    ss << "    previous_uv = current_uv;\n";
    ss << "    previous_ray_height = ray_height;\n";
    ss << "    previous_height = current_height;\n";
    ss << "    ray_height -= layer_depth;\n";
    ss << "    current_uv -= delta_uv;\n";
    ss << "    current_height = " << height_helper.helper_name;
    ss << "(current_uv, uv_dx, uv_dy) + height_offset;\n";
    ss << "  }\n";
    ss << "  if (" << mode << " == PARALLAX_MODE_OCCLUSION) {\n";
    ss << "    uv_out = float3(parallax_interpolate_uv(current_uv, current_height, ";
    ss << "ray_height, previous_uv, previous_height, previous_ray_height), uv_in.z);\n";
    ss << "  }\n";
    ss << "  else if (" << mode << " == PARALLAX_MODE_RELIEF) {\n";
    ss << "    float2 after_uv = current_uv;\n";
    ss << "    float after_height = current_height;\n";
    ss << "    float after_ray_height = ray_height;\n";
    ss << "    float2 before_uv = previous_uv;\n";
    ss << "    float before_height = previous_height;\n";
    ss << "    float before_ray_height = previous_ray_height;\n";
    ss << "    int refine_count = int(clamp(floor(refinement_steps + 0.5f), 0.0f, 8.0f));\n";
    ss << "    for (int i = 0; i < 8; i++) {\n";
    ss << "      if (i >= refine_count) {\n";
    ss << "        break;\n";
    ss << "      }\n";
    ss << "      float2 middle_uv = (after_uv + before_uv) * 0.5f;\n";
    ss << "      float middle_ray_height = (after_ray_height + before_ray_height) * 0.5f;\n";
    ss << "      float middle_height = " << height_helper.helper_name;
    ss << "(middle_uv, uv_dx, uv_dy) + height_offset;\n";
    ss << "      float delta = middle_height - middle_ray_height;\n";
    ss << "      if (delta > 0.0f) {\n";
    ss << "        after_uv = middle_uv;\n";
    ss << "        after_height = middle_height;\n";
    ss << "        after_ray_height = middle_ray_height;\n";
    ss << "      }\n";
    ss << "      else {\n";
    ss << "        before_uv = middle_uv;\n";
    ss << "        before_height = middle_height;\n";
    ss << "        before_ray_height = middle_ray_height;\n";
    ss << "      }\n";
    ss << "      if (abs(delta) <= 0.01f) {\n";
    ss << "        break;\n";
    ss << "      }\n";
    ss << "    }\n";
    ss << "    uv_out = float3(parallax_interpolate_uv(after_uv, after_height, ";
    ss << "after_ray_height, before_uv, before_height, before_ray_height), uv_in.z);\n";
    ss << "  }\n";
    ss << "  else if (" << mode << " == PARALLAX_MODE_SECANT_RELIEF) {\n";
    ss << "    float2 before_uv = previous_uv;\n";
    ss << "    float before_ray_height = previous_ray_height;\n";
    ss << "    float before_height = previous_height;\n";
    ss << "    float before_delta = before_ray_height - before_height;\n";
    ss << "    float2 after_uv = current_uv;\n";
    ss << "    float after_ray_height = ray_height;\n";
    ss << "    float after_height = current_height;\n";
    ss << "    float after_delta = after_ray_height - after_height;\n";
    ss << "    int refine_count = int(clamp(floor(refinement_steps + 0.5f), 0.0f, 8.0f));\n";
    ss << "    for (int i = 0; i < 8; i++) {\n";
    ss << "      if (i >= refine_count || abs(after_delta - before_delta) <= 1.0e-5f) {\n";
    ss << "        break;\n";
    ss << "      }\n";
    ss << "      float intersection_ray_height = (before_ray_height * after_delta - ";
    ss << "after_ray_height * before_delta) / (after_delta - before_delta);\n";
    ss << "      float2 intersection_uv = uv_in.xy - ";
    ss << "(1.0f - intersection_ray_height) * delta_uv * layer_count;\n";
    ss << "      float intersection_height = " << height_helper.helper_name;
    ss << "(intersection_uv, uv_dx, uv_dy) + height_offset;\n";
    ss << "      float delta = intersection_ray_height - intersection_height;\n";
    ss << "      if (delta < 0.0f) {\n";
    ss << "        after_uv = intersection_uv;\n";
    ss << "        after_ray_height = intersection_ray_height;\n";
    ss << "        after_height = intersection_height;\n";
    ss << "        after_delta = delta;\n";
    ss << "      }\n";
    ss << "      else {\n";
    ss << "        before_uv = intersection_uv;\n";
    ss << "        before_ray_height = intersection_ray_height;\n";
    ss << "        before_height = intersection_height;\n";
    ss << "        before_delta = delta;\n";
    ss << "      }\n";
    ss << "      if (abs(delta) <= 0.01f) {\n";
    ss << "        break;\n";
    ss << "      }\n";
    ss << "    }\n";
    ss << "    uv_out = float3(parallax_interpolate_uv(after_uv, after_height, ";
    ss << "after_ray_height, before_uv, before_height, before_ray_height), uv_in.z);\n";
    ss << "  }\n";
    ss << "  else {\n";
    ss << "    uv_out = float3(current_uv, uv_in.z);\n";
    ss << "  }\n";
    ss << "  float hit_height = " << height_helper.helper_name;
    ss << "(uv_out.xy, uv_dx, uv_dy) + height_offset;\n";
    if (use_normal) {
      ss << "  float normal_step = max(max(length(uv_dx), length(uv_dy)), 1.0e-4f);\n";
      ss << "  float u_height = " << height_helper.helper_name;
      ss << "(uv_out.xy + float2(normal_step, 0.0f), uv_dx, uv_dy) + height_offset;\n";
      ss << "  float v_height = " << height_helper.helper_name;
      ss << "(uv_out.xy + float2(0.0f, normal_step), uv_dx, uv_dy) + height_offset;\n";
      ss << "  normal_out = parallax_normal_from_height_samples(tangent, hit_height, ";
      ss << "u_height, v_height, normal_step, normal_step, scale);\n";
    }
    if (compute_shadow) {
      ss << "  float3 L = parallax_safe_normalize(";
      ss << "parallax_world_direction_to_tangent(tangent, sun_direction));\n";
      ss << "  if (dot(L, L) <= 1.0e-12f) {\n";
      ss << "    return;\n";
      ss << "  }\n";
      ss << "  if (L.z <= 1.0e-5f) {\n";
      ss << "    shadow_out = 0.0f;\n";
      ss << "    return;\n";
      ss << "  }\n";
      ss << "  float shadow_min_count = clamp(floor(min_steps + 0.5f), 1.0f, 128.0f);\n";
      ss << "  float shadow_max_count = clamp(floor(max_steps + 0.5f), shadow_min_count, ";
      ss << "128.0f);\n";
      ss << "  float shadow_layer_count = clamp(ceil(mix(shadow_max_count, shadow_min_count, ";
      ss << "clamp(abs(L.z), 0.0f, 1.0f))), shadow_min_count, shadow_max_count);\n";
      ss << "  float shadow_layer_depth = (1.0f - hit_height) / max(shadow_layer_count, ";
      ss << "1.0f);\n";
      ss << "  if (shadow_layer_depth <= 1.0e-6f) {\n";
      ss << "    return;\n";
      ss << "  }\n";
      ss << "  float2 shadow_delta_uv = scale * L.xy / max(L.z * shadow_layer_count, ";
      ss << "1.0e-5f);\n";
      ss << "  float2 shadow_uv_offset = shadow_delta_uv;\n";
      ss << "  float shadow_ray_height = hit_height + shadow_layer_depth;\n";
      ss << "  float shadow_height = " << height_helper.helper_name;
      ss << "(uv_out.xy + shadow_uv_offset, uv_dx, uv_dy) + height_offset;\n";
      ss << "  for (int i = 0; i < 128; i++) {\n";
      ss << "    if (i >= int(shadow_layer_count) || shadow_ray_height >= 1.0f) {\n";
      ss << "      break;\n";
      ss << "    }\n";
      ss << "    if (shadow_height > shadow_ray_height) {\n";
      ss << "      shadow_out = 0.0f;\n";
      ss << "      break;\n";
      ss << "    }\n";
      ss << "    shadow_ray_height += shadow_layer_depth;\n";
      ss << "    shadow_uv_offset += shadow_delta_uv;\n";
      ss << "    shadow_height = " << height_helper.helper_name;
      ss << "(uv_out.xy + shadow_uv_offset, uv_dx, uv_dy) + height_offset;\n";
      ss << "  }\n";
    }
    ss << "}\n";
  }
  return ss.str();
}

static std::string build_plane_offset_wrapper_source()
{
  std::stringstream ss;
  ss << "void " << parallax_plane_wrapper_name
     << "(float3 uv_in, float scale, float4 tangent, out float3 uv_out)\n";
  ss << "{\n";
  ss << "  float3 normal_tmp;\n";
  ss << "  node_parallax_plane_offset(uv_in, scale, tangent, uv_out, normal_tmp);\n";
  ss << "}\n\n";
  ss << "void " << parallax_plane_normal_wrapper_name;
  ss << "(float3 uv_in, float scale, float4 tangent, out float3 uv_out, ";
  ss << "out float3 normal_out)\n";
  ss << "{\n";
  ss << "  node_parallax_plane_offset(uv_in, scale, tangent, uv_out, normal_out);\n";
  ss << "}\n";
  return ss.str();
}

static void add_plane_offset_wrapper_source(GPUMaterial *mat)
{
  const Vector<StringRefNull> dependencies = {parallax_helper_filename};
  const std::string wrapper_source = build_plane_offset_wrapper_source();
  GPU_material_generated_source_add(
      mat, parallax_plane_wrapper_filename, dependencies, wrapper_source.c_str());
}

static Vector<GPUNodeStack> input_stack_with_tangent(GPUMaterial *mat,
                                                     const bNode &node,
                                                     GPUNodeStack *in)
{
  Vector<GPUNodeStack> stack;
  if (in != nullptr) {
    for (int i = 0; !in[i].end; i++) {
      stack.append(in[i]);
    }
  }

  GPUNodeStack tangent = {};
  tangent.type = GPU_VEC4;
  tangent.vec[3] = 1.0f;
  tangent.link = GPU_attribute(mat, CD_TANGENT, node_uv_map(node));
  tangent.hasinput = true;
  tangent.sockettype = SOCK_VECTOR;
  stack.append(tangent);

  GPUNodeStack end = {};
  end.end = true;
  stack.append(end);
  return stack;
}

static Vector<GPUNodeStack> output_stack_for_used_outputs(GPUNodeStack *out,
                                                          const bool use_normal,
                                                          const bool compute_shadow)
{
  Vector<GPUNodeStack> stack;
  if (out != nullptr) {
    for (int i = 0; !out[i].end; i++) {
      stack.append(out[i]);
    }
  }
  if (stack.size() > 1 && !use_normal) {
    stack[1].type = GPU_NONE;
  }
  if (stack.size() > 2 && !compute_shadow) {
    stack[2].type = GPU_NONE;
  }

  GPUNodeStack end = {};
  end.end = true;
  stack.append(end);
  return stack;
}

static bool parallax_stack_link_custom(GPUMaterial *mat,
                                       bNode *node,
                                       const StringRefNull function_name,
                                       const StringRefNull filename,
                                       GPUNodeStack *in,
                                       GPUNodeStack *out,
                                       const bool use_normal,
                                       const bool compute_shadow)
{
  Vector<GPUNodeStack> input_stack = input_stack_with_tangent(mat, *node, in);
  Vector<GPUNodeStack> output_stack = output_stack_for_used_outputs(
      out, use_normal, compute_shadow);
  const bool linked = GPU_stack_link_custom(mat,
                                            node,
                                            function_name,
                                            filename,
                                            GPU_CUSTOM_NODE_DEPENDENCY_NONE,
                                            input_stack.data(),
                                            output_stack.data());
  if (linked && out != nullptr) {
    for (int i = 0; !out[i].end; i++) {
      if (output_stack[i].type != GPU_NONE) {
        out[i].link = output_stack[i].link;
      }
    }
  }
  return linked;
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
  const int mode = effective_mode(node->custom1);
  const bool use_normal = out != nullptr && !out[1].end && out[1].hasoutput;
  const bool has_shadow_input = mode != SHD_PARALLAX_PLANE_OFFSET && node_use_shadow(*node);
  const bool compute_shadow = has_shadow_input && out != nullptr && !out[2].end &&
                              out[2].hasoutput;

  GPU_material_flag_set(mat, GPU_MATFLAG_OBJECT_INFO);

  if (mode == SHD_PARALLAX_PLANE_OFFSET) {
    ensure_uv_stack(mat, node, in);
    add_plane_offset_wrapper_source(mat);
    return parallax_stack_link_custom(mat,
                                      node,
                                      use_normal ? parallax_plane_normal_wrapper_name :
                                                   parallax_plane_wrapper_name,
                                      parallax_plane_wrapper_filename,
                                      in,
                                      out,
                                      use_normal,
                                      false);
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

  const std::string wrapper_source = build_wrapper_source(
      wrapper_name, height_helper, mode, use_normal, has_shadow_input, compute_shadow);
  const Vector<StringRefNull> dependencies = {parallax_helper_filename};
  GPU_material_generated_source_add(
      mat, wrapper_filename.c_str(), dependencies, wrapper_source.c_str());

  return parallax_stack_link_custom(mat,
                                    node,
                                    wrapper_name.c_str(),
                                    wrapper_filename.c_str(),
                                    in,
                                    out,
                                    use_normal,
                                    compute_shadow);
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
  bke::node_type_storage(
      ntype, "NodeShaderParallax", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::gpu_shader_parallax;
  ntype.insert_link = file_ns::node_insert_link;

  bke::node_register_type(ntype);
}

}  // namespace blender
