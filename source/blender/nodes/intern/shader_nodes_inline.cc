/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>
#include <algorithm>
#include <functional>
#include <type_traits>
#include <variant>

#include "DNA_material_types.h"

#include "BKE_compute_context_cache.hh"
#include "BKE_lib_id.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_zones.hh"
#include "BKE_type_conversions.hh"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_stack.hh"
#include "BLI_string.h"

#include "NOD_menu_value.hh"
#include "NOD_multi_function.hh"
#include "NOD_node_declaration.hh"
#include "NOD_node_in_compute_context.hh"
#include "NOD_shader_nodes_inline.hh"
#include "NOD_shader_glsl_bundle.hh"

namespace blender::nodes {
namespace {

struct SocketValue;
struct BundleSocketValue;
using BundleSocketValuePtr = std::shared_ptr<BundleSocketValue>;

struct FallbackValue {};

/** An authored but invalid Bundle link must not activate optional GLSL defaults. */
struct InvalidBundleValue {
  std::string message;
  bool reported = false;
};

/** This indicates that the value should be ignored when it is linked to an input socket. */
struct DanglingValue {};

struct NodeAndSocket {
  bNode *node = nullptr;
  bNodeSocket *socket = nullptr;
};

struct VectorPrimitiveSocketValue {
  float3 xyz = float3(0.0f);
  float w = 0.0f;
  int dimensions = 3;
};

struct PrimitiveSocketValue {
  std::variant<int, float, bool, ColorGeometry4f, VectorPrimitiveSocketValue, MenuValue> value;

  const void *buffer() const
  {
    return std::visit(
        [](auto &&value) -> const void * {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, VectorPrimitiveSocketValue>) {
            return &value.xyz;
          }
          else {
            return &value;
          }
        },
        value);
  }

  void *buffer()
  {
    return const_cast<void *>(const_cast<const PrimitiveSocketValue *>(this)->buffer());
  }

  static PrimitiveSocketValue from_value(const GPointer value)
  {
    const CPPType &type = *value.type();
    if (type.is<int>()) {
      return {*static_cast<const int *>(value.get())};
    }
    if (type.is<float>()) {
      return {*static_cast<const float *>(value.get())};
    }
    if (type.is<bool>()) {
      return {*static_cast<const bool *>(value.get())};
    }
    if (type.is<ColorGeometry4f>()) {
      return {*static_cast<const ColorGeometry4f *>(value.get())};
    }
    if (type.is<float3>()) {
      return {VectorPrimitiveSocketValue{*static_cast<const float3 *>(value.get()), 0.0f, 3}};
    }
    if (type.is<MenuValue>()) {
      return {*static_cast<const MenuValue *>(value.get())};
    }
    BLI_assert_unreachable();
    return {};
  }
};

/** References an output socket in the generated node tree. */
struct LinkedSocketValue {
  bNode *node = nullptr;
  bNodeSocket *socket = nullptr;
};

/** References an input socket in the source node tree. */
struct InputSocketValue {
  SocketInContext socket;
};

struct ClosureZoneValue {
  const bke::bNodeTreeZone *zone = nullptr;
  const ComputeContext *closure_creation_context = nullptr;
};

struct MultiInputValue {
  Vector<SocketValue, 0> values;
};

struct SocketValue {
  /**
   * The value of an arbitrary socket value can have one of many different types. At a high level
   * it can either have a specific constant-folded value, or it references a socket that can't be
   * constant-folded.
   */
  std::variant<FallbackValue,
               InvalidBundleValue,
               DanglingValue,
               LinkedSocketValue,
               InputSocketValue,
               PrimitiveSocketValue,
               ClosureZoneValue,
               BundleSocketValuePtr,
               MultiInputValue>
      value;

  /** Direct source only. CPU calculations deliberately do not propagate this identity. */
  std::optional<SocketInContext> uniform_source = std::nullopt;

  /** Try to get the value as a primitive value. */
  std::optional<PrimitiveSocketValue> to_primitive(const bke::bNodeSocketType &type) const
  {
    if (const auto *primitive_value = std::get_if<PrimitiveSocketValue>(&this->value)) {
      return *primitive_value;
    }
    if (const auto *input_socket_value = std::get_if<InputSocketValue>(&this->value)) {
      const bNodeSocket &socket = *input_socket_value->socket;
      BLI_assert(socket.type == type.type);
      if (!socket.runtime->declaration) {
        return std::nullopt;
      }
      if (socket.runtime->declaration->default_input_type != NODE_DEFAULT_INPUT_VALUE) {
        return std::nullopt;
      }
      switch (socket.typeinfo->type) {
        case SOCK_FLOAT:
          return PrimitiveSocketValue{socket.default_value_typed<bNodeSocketValueFloat>()->value};
        case SOCK_INT:
          return PrimitiveSocketValue{socket.default_value_typed<bNodeSocketValueInt>()->value};
        case SOCK_BOOLEAN:
          return PrimitiveSocketValue{
              bool(socket.default_value_typed<bNodeSocketValueBoolean>()->value)};
        case SOCK_VECTOR: {
          const bNodeSocketValueVector &value =
              *socket.default_value_typed<bNodeSocketValueVector>();
          return PrimitiveSocketValue{
              VectorPrimitiveSocketValue{float3(value.value),
                                         value.value[3],
                                         value.dimensions > 0 ? value.dimensions : 3}};
        }
        case SOCK_RGBA:
          return PrimitiveSocketValue{
              ColorGeometry4f(socket.default_value_typed<bNodeSocketValueRGBA>()->value)};
        case SOCK_MENU:
          return PrimitiveSocketValue{
              MenuValue(socket.default_value_typed<bNodeSocketValueMenu>()->value)};
        default:
          return std::nullopt;
      }
    }
    if (std::get_if<FallbackValue>(&this->value)) {
      switch (type.type) {
        case SOCK_INT:
        case SOCK_BOOLEAN:
        case SOCK_VECTOR:
        case SOCK_RGBA:
        case SOCK_FLOAT:
          return PrimitiveSocketValue::from_value(
              {type.base_cpp_type, type.base_cpp_type->default_value()});
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }
};

struct BundleSocketValue {
  struct Item {
    std::string key;
    SocketValue value;
    const bke::bNodeSocketType *socket_type = nullptr;
    int dimensions = 0;
  };

  Vector<Item> items;
  bool strict = false;
};

struct PreservedZone {
  bNode *input_node = nullptr;
  bNode *output_node = nullptr;
};

struct LocalizedClosureZone {
  enum class Status {
    Materializing,
    Complete,
    Failed,
  };

  Status status = Status::Materializing;
  NodeInContext consumer_node;
  SocketInContext requester_socket;
  int64_t requester_stack_depth = 0;
  LinkedSocketValue closure_output;
};

class ShaderNodesInliner {
 private:
  /** Cache for intermediate values used during the inline process. */
  ResourceScope scope_;
  /** The original tree the has to be inlined. */
  const bNodeTree &src_tree_;
  /** The tree where the inlined nodes will be added. */
  bNodeTree &dst_tree_;
  /** Parameters passed in by the caller. */
  InlineShaderNodeTreeParams &params_;
  /** Simplifies building the all the compute contexts for nodes in zones and groups. */
  bke::ComputeContextCache compute_context_cache_;
  /**
   * Stores compute context of the direct parent of each zone. In most cases, this is just the
   * parent compute context directly, except for closures.
   */
  Map<const ComputeContext *, const ComputeContext *> parent_zone_contexts_;
  /** Stores the computed value for each socket. The final value for each socket may be constant */
  Map<SocketInContext, SocketValue> value_by_socket_;
  /**
   * Remember zone nodes that have been copied to the destination so that they can be connected
   * again in the end.
   */
  Map<NodeInContext, PreservedZone> copied_zone_by_zone_output_node_;
  /**
   * Closure zones connected to typed GLSL Function callbacks have to survive in the
   * localized
   * shader tree. The source output node and its creation context uniquely identify
   * such a zone.
   */
  Map<NodeInContext, LocalizedClosureZone> localized_closure_zones_;
  /** Sockets that still have to be evaluated. */
  Stack<SocketInContext> scheduled_sockets_stack_;
  /** Knows how to compute between different data types. */
  const bke::DataTypeConversions &data_type_conversions_;
  /** This is used to generate unique names and ids. */
  int dst_node_counter_ = 0;

  static uint32_t original_tree_uid(const bNodeTree &tree)
  {
    const ID &original = tree.id.orig_id ? *tree.id.orig_id : tree.id;
    return original.session_uid;
  }

  static int uniform_socket_components(const bNodeSocket &socket)
  {
    if (socket.default_value == nullptr) {
      return 0;
    }
    switch (socket.type) {
      case SOCK_FLOAT:
        return 1;
      case SOCK_RGBA:
        return 4;
      case SOCK_VECTOR: {
        const int dimensions = socket.default_value_typed<bNodeSocketValueVector>()->dimensions;
        return dimensions == 0 ? 3 : dimensions;
      }
      default:
        return 0;
    }
  }

  std::optional<ShaderUniformSource> uniform_source_identity(const SocketInContext &socket)
  {
    const int components = uniform_socket_components(*socket);
    if (components < 1 || components > 4) {
      return std::nullopt;
    }
    ShaderUniformSource source;
    source.tree_uid = original_tree_uid(socket->owner_tree());
    source.node_identifier = socket->owner_node().identifier;
    source.socket_identifier = socket->identifier;
    source.in_out = eNodeSocketInOut(socket->in_out);
    source.socket_type = eNodeSocketDatatype(socket->type);
    source.components = components;
    uint32_t expected_tree_uid = source.tree_uid;
    if (source.tree_uid == 0) {
      params_.r_uniform_tracking->unsupported_context = true;
      return std::nullopt;
    }
    for (const ComputeContext *context = socket.context; context != nullptr;
         context = context->parent())
    {
      const auto *group = dynamic_cast<const bke::GroupNodeComputeContext *>(context);
      const bNode *group_node = group ? group->node() : nullptr;
      if (!group || !group->tree() || !group_node || !group_node->id ||
          GS(group_node->id->name) != ID_NT)
      {
        params_.r_uniform_tracking->unsupported_context = true;
        return std::nullopt;
      }
      const uint32_t parent_uid = original_tree_uid(*group->tree());
      const uint32_t child_uid = original_tree_uid(
          *reinterpret_cast<const bNodeTree *>(group_node->id));
      if (parent_uid == 0 || child_uid != expected_tree_uid) {
        params_.r_uniform_tracking->unsupported_context = true;
        return std::nullopt;
      }
      source.group_path.append({parent_uid, group->node_id(), child_uid});
      expected_tree_uid = parent_uid;
    }
    if (expected_tree_uid != original_tree_uid(src_tree_)) {
      params_.r_uniform_tracking->unsupported_context = true;
      return std::nullopt;
    }
    std::reverse(source.group_path.begin(), source.group_path.end());
    return source;
  }

  void uniform_source_unsafe(const SocketValue &value)
  {
    if (!params_.r_uniform_tracking || !value.uniform_source) {
      return;
    }
    if (std::optional<ShaderUniformSource> source = uniform_source_identity(*value.uniform_source)) {
      if (!params_.r_uniform_tracking->unsafe_sources.contains(*source)) {
        params_.r_uniform_tracking->unsafe_sources.append(std::move(*source));
      }
    }
  }

  static bool uniform_input_supported(const bNode &node, const bNodeSocket &socket)
  {
    if (node.is_type("ShaderNodeEmission"_ustr) || node.is_type("ShaderNodeNormalMap"_ustr)) {
      return (STREQ(socket.identifier, "Strength") && socket.type == SOCK_FLOAT) ||
             (STREQ(socket.identifier, "Color") && socket.type == SOCK_RGBA);
    }
    return false;
  }

  static bool uniform_source_matches_target(const SocketValue &value, const bNodeSocket &target)
  {
    return value.uniform_source && value.uniform_source->socket->type == target.type &&
           uniform_socket_components(*value.uniform_source->socket) ==
               uniform_socket_components(target);
  }

  void track_uniform_target(const bNode &node, const bNodeSocket &socket, const SocketValue &value)
  {
    if (!params_.r_uniform_tracking || !value.uniform_source) {
      return;
    }
    if (!uniform_source_matches_target(value, socket)) {
      this->uniform_source_unsafe(value);
      return;
    }
    if (std::optional<ShaderUniformSource> source = uniform_source_identity(*value.uniform_source)) {
      params_.r_uniform_tracking->sources.add_overwrite(
          &socket,
          ShaderUniformTarget{std::move(*source),
                              node.identifier,
                              node.name,
                              socket.identifier,
                              eNodeSocketInOut(socket.in_out),
                              eNodeSocketDatatype(socket.type)});
    }
  }

 public:
  ShaderNodesInliner(const bNodeTree &src_tree,
                     bNodeTree &dst_tree,
                     InlineShaderNodeTreeParams &params)
      : src_tree_(src_tree),
        dst_tree_(dst_tree),
        params_(params),
        data_type_conversions_(bke::get_implicit_type_conversions())
  {
  }

  bool do_inline()
  {
    src_tree_.ensure_topology_cache();
    if (src_tree_.has_available_link_cycle()) {
      for (const bNode *node : src_tree_.all_nodes()) {
        if (!node->is_type("ShaderNodeGLSLFunction"_ustr)) {
          continue;
        }
        for (const bNodeSocket *socket : node->input_sockets()) {
          const StringRef identifier(socket->identifier);
          if (socket->type == SOCK_CLOSURE && socket->is_directly_linked()) {
            params_.r_error_messages.append(
                {node,
                 fmt::format("GLSL closure callback '{}' is part of a cyclic shader dependency",
                             identifier)});
          }
        }
      }
      return false;
    }

    const Vector<SocketInContext> final_output_sockets = this->find_final_output_sockets();

    /* Evaluation starts at the final output sockets which will request the evaluation of whether
     * sockets are linked to them. */
    for (const SocketInContext &socket : final_output_sockets) {
      this->schedule_socket(socket);
    }

    /* Evaluate until all scheduled sockets have a value. While evaluating a single socket, it may
     * either end up having a value, or request more other sockets that need to be evaluated first.
     *
     * This uses an explicit stack instead of recursion to avoid stack overflows which can easily
     * happen when there are long chains of nodes (or e.g. repeat zones with many iterations). */
    while (!scheduled_sockets_stack_.is_empty()) {
      const SocketInContext socket = scheduled_sockets_stack_.peek();
      const int old_stack_size = scheduled_sockets_stack_.size();

      this->handle_socket(socket);

      if (scheduled_sockets_stack_.size() == old_stack_size) {
        /* No additional dependencies were pushed, so this socket is fully handled and can be
         * popped from the stack. */
        BLI_assert(socket == scheduled_sockets_stack_.peek());
        scheduled_sockets_stack_.pop();
      }
    }

    /* Create actual output nodes. */
    Map<NodeInContext, bNode *> final_output_nodes;
    for (const SocketInContext &socket : final_output_sockets) {
      const NodeInContext src_node = socket.owner_node();
      bNode *copied_node = final_output_nodes.lookup_or_add_cb(src_node, [&]() {
        Map<const bNodeSocket *, bNodeSocket *> socket_map;
        bNode *copied_node = bke::node_copy_with_mapping(&dst_tree_,
                                                         *src_node.node,
                                                         this->node_copy_flag(),
                                                         std::nullopt,
                                                         this->get_next_node_identifier(),
                                                         socket_map);
        copied_node->parent = nullptr;
        return copied_node;
      });
      bNodeSocket *copied_socket = static_cast<bNodeSocket *>(
          BLI_findlink(&copied_node->inputs, socket.socket->index()));
      this->set_input_socket_value(
          *src_node, *copied_node, *copied_socket, value_by_socket_.lookup(socket));
    }

    this->restore_zones_in_output_tree();
    this->position_nodes_in_output_tree();
    return true;
  }

  Vector<SocketInContext> find_final_output_sockets()
  {
    Vector<TreeInContext> trees;
    this->find_trees_potentially_containing_shader_outputs_recursive(nullptr, src_tree_, trees);

    auto get_engine_target = [](const bNode *output_node) {
      if (STR_ELEM(output_node->idname,
                   "ShaderNodeOutputMaterial",
                   "ShaderNodeOutputLight",
                   "ShaderNodeOutputWorld"))
      {
        return NodeShaderOutputTarget(output_node->custom1);
      }
      return SHD_OUTPUT_ALL;
    };

    Vector<SocketInContext> output_sockets;
    const bool is_npr_tree = !src_tree_.nodes_by_type("ShaderNodeNPR_Output"_ustr).is_empty();
    auto add_output_type = [&](const UString output_type) {
      for (const TreeInContext &tree : trees) {
        const bke::bNodeTreeZones &zones = *tree->zones();
        for (const bNode *node : tree->nodes_by_type(output_type)) {
          if (!ELEM(get_engine_target(node), SHD_OUTPUT_ALL, params_.target_engine_)) {
            continue;
          }
          /* A muted node may have dangling input values that can't be assigned to a socket. */
          if (output_type == "ShaderNodeOutputOutlineShell"_ustr && node->is_muted()) {
            continue;
          }
          const bke::bNodeTreeZone *zone = zones.get_zone_by_node(node->identifier);
          if (zone) {
            params_.r_error_messages.append({node, TIP_("Output node must not be in zone")});
            continue;
          }
          for (const bNodeSocket *socket : node->input_sockets()) {
            output_sockets.append({tree.context, socket});
          }
        }
      }
    };

    if (is_npr_tree) {
      add_output_type("ShaderNodeNPR_Output"_ustr);
      add_output_type("ShaderNodeOutputAOV"_ustr);
      add_output_type("ShaderNodeOutlineControl"_ustr);
      return output_sockets;
    }

    /* owner_id can be null for DefaultSurfaceNodeTree. */
    ID_Type tree_type = src_tree_.owner_id ? GS(src_tree_.owner_id->name) : ID_MA;

    switch (tree_type) {
      case ID_MA:
        if (const Material *material = reinterpret_cast<const Material *>(src_tree_.owner_id);
            material != nullptr && material->eevee_domain == MA_EEVEE_DOMAIN_FILTER)
        {
          add_output_type("ShaderNodeOutputFilter"_ustr);
          add_output_type("ShaderNodeOutputAOV"_ustr);
        }
        else {
          add_output_type("ShaderNodeOutputMaterial"_ustr);
          add_output_type("ShaderNodeOutputLight"_ustr);
          add_output_type("ShaderNodeOutputAOV"_ustr);
          add_output_type("ShaderNodeOutlineControl"_ustr);
          add_output_type("ShaderNodeOutputOutlineShell"_ustr);
        }
        break;
      case ID_WO:
        add_output_type("ShaderNodeOutputWorld"_ustr);
        add_output_type("ShaderNodeOutputAOV"_ustr);
        break;
      case ID_LA:
        add_output_type("ShaderNodeOutputLight"_ustr);
        add_output_type("ShaderNodeEeveeLightShaderOutput"_ustr);
        break;
      case ID_NT:
        add_output_type("ShaderNodeNPR_Output"_ustr);
        add_output_type("ShaderNodeOutputAOV"_ustr);
        add_output_type("ShaderNodeOutlineControl"_ustr);
        break;
      case ID_LS:
        add_output_type("ShaderNodeOutputLineStyle"_ustr);
        break;
      default:
        BLI_assert_unreachable();
    }

    return output_sockets;
  }

  void find_trees_potentially_containing_shader_outputs_recursive(const ComputeContext *context,
                                                                  const bNodeTree &tree,
                                                                  Vector<TreeInContext> &r_trees)
  {
    const bke::bNodeTreeZones *zones = tree.zones();
    if (!zones) {
      return;
    }
    if (tree.has_available_link_cycle()) {
      return;
    }
    r_trees.append({context, &tree});
    for (const bNode *group_node : tree.group_nodes()) {
      if (group_node->is_muted()) {
        continue;
      }
      const bNodeTree *group = id_cast<const bNodeTree *>(group_node->id);
      if (!group || ID_MISSING(&group->id)) {
        continue;
      }
      group->ensure_topology_cache();
      const bke::bNodeTreeZone *zone = zones->get_zone_by_node(group_node->identifier);
      if (zone) {
        /* Node groups in zones are ignored. */
        continue;
      }
      const ComputeContext &group_context = compute_context_cache_.for_group_node(
          context, group_node->identifier, &tree);
      this->find_trees_potentially_containing_shader_outputs_recursive(
          &group_context, *group, r_trees);
    }
  }

  void handle_socket(const SocketInContext &socket)
  {
    if (!socket->is_available()) {
      return;
    }
    if (value_by_socket_.contains(socket)) {
      /* The socket already has a value, so there is nothing to do. */
      return;
    }
    if (socket->is_input()) {
      this->handle_input_socket(socket);
    }
    else {
      this->handle_output_socket(socket);
    }
  }

  void handle_input_socket(const SocketInContext &socket)
  {
    if (socket->is_multi_input()) {
      this->handle_multi_input_socket(socket);
      return;
    }

    const bNodeLink *used_link = nullptr;
    for (const bNodeLink *link : socket->directly_linked_links()) {
      if (socket->type == SOCK_BUNDLE && !(link->flag & NODE_LINK_MUTED) &&
          (!(link->flag & NODE_LINK_VALID) || !link->fromnode || link->fromnode->is_undefined()))
      {
        this->store_socket_value(socket,
                                 {InvalidBundleValue{"Invalid connection to bundle socket '" +
                                                     std::string(socket->name) + "'"}});
        return;
      }
      if (!link->is_used() || link->fromnode == nullptr || link->fromnode->is_undefined()) {
        continue;
      }
      used_link = link;
    }
    if (!used_link) {
      /* If there is no link on the input, use the value of the socket directly. */
      if (this->input_socket_may_have_dangling_value(socket)) {
        this->store_socket_value(socket, {DanglingValue{}});
      }
      else {
        this->store_socket_value(socket, {InputSocketValue{socket}});
      }
      return;
    }

    const ComputeContext *from_context = this->get_link_source_context(*used_link, socket);
    const SocketInContext origin_socket = {from_context, used_link->fromsock};
    if (const auto *value = value_by_socket_.lookup_ptr(origin_socket)) {
      if (std::holds_alternative<DanglingValue>(value->value)) {
        if (this->input_socket_may_have_dangling_value(socket)) {
          this->store_socket_value(socket, {DanglingValue{}});
        }
        else {
          /* If the input value is dangling, use the value of the socket itself. */
          this->store_socket_value(socket, {InputSocketValue{socket}});
        }
        return;
      }
      /* If the socket linked to the input has a value already, copy that value to the current
       * socket, potentially with an implicit conversion. */
      this->store_socket_value(socket,
                               this->handle_implicit_conversion(*value,
                                                                *used_link->fromsock->typeinfo,
                                                                *used_link->tosock->typeinfo));
      return;
    }
    /* If the origin socket does not have a value yet, only schedule it for evaluation for now. */
    this->schedule_socket(origin_socket);
  }

  void handle_multi_input_socket(const SocketInContext &socket)
  {
    bool all_links_ready = true;
    Vector<SocketValue, 0> values;
    for (const bNodeLink *link : socket->directly_linked_links()) {
      if (!link->is_used()) {
        continue;
      }
      const ComputeContext *from_context = this->get_link_source_context(*link, socket);
      const SocketInContext origin_socket = {from_context, link->fromsock};
      const SocketValue *value = value_by_socket_.lookup_ptr(origin_socket);
      if (!value) {
        this->schedule_socket(origin_socket);
        all_links_ready = false;
        continue;
      }
      values.append(*value);
    }
    if (!all_links_ready) {
      return;
    }
    this->store_socket_value(socket, {MultiInputValue{std::move(values)}});
  }

  /**
   * Generally, input values of a node should never be dangling because otherwise the node can't be
   * evaluated. However, if a node is never evaluated anyway, then its inputs can be dangling. This
   * allows the dangling-state to be properly forwarded through the node.
   */
  bool input_socket_may_have_dangling_value(const SocketInContext &socket)
  {
    BLI_assert(socket->is_input());
    const NodeInContext node = socket.owner_node();
    return node->is_reroute() || node->is_muted();
  }

  const ComputeContext *get_link_source_context(const bNodeLink &link,
                                                const SocketInContext &to_socket)
  {
    const bNodeTree &tree = to_socket->owner_tree();
    const bke::bNodeTreeZones *zones = tree.zones();
    if (!zones) {
      return nullptr;
    }
    const bke::bNodeTreeZone *to_zone = zones->get_zone_by_socket(*to_socket);
    const bke::bNodeTreeZone *from_zone = zones->get_zone_by_socket(*link.fromsock);
    const ComputeContext *context = to_socket.context;
    for (const bke::bNodeTreeZone *zone = to_zone; zone && zone != from_zone;
         zone = zone->parent_zone)
    {
      const bNode *zone_output_node = zone->output_node();
      if (!zone_output_node) {
        break;
      }
      if (zone_output_node->is_type("GeometryNodeRepeatOutput"_ustr) &&
          this->should_preserve_repeat_zone_node(*zone_output_node))
      {
        /* Preserved repeat zones are embedded into their outer compute context. */
        continue;
      }
      if (zone_output_node->is_type("ShaderNodeForeachLightOutput"_ustr) &&
          this->should_preserve_foreach_light_zone_node(*zone_output_node))
      {
        /* Preserved foreach-light zones are embedded into their outer compute context too. */
        continue;
      }
      const ComputeContext **parent_context = parent_zone_contexts_.lookup_ptr(context);
      if (parent_context == nullptr) {
        /* Interactive link-dragging can temporarily expose cross-zone links before the tree is
         * normalized through the zone IO sockets. In that state, keep the current context instead
         * of crashing on a missing parent mapping. */
        break;
      }
      context = *parent_context;
    }
    return context;
  }

  void handle_output_socket(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    if (params_.r_uniform_tracking &&
        (node->type_legacy == SH_NODE_PORTAL_OUT ||
         node->is_type("GeometryNodeRepeatOutput"_ustr) ||
         node->is_type("GeometryNodeRepeatInput"_ustr) ||
         node->is_type("ShaderNodeForeachLightOutput"_ustr) ||
         node->is_type("ShaderNodeForeachLightInput"_ustr) ||
         node->is_type("NodeClosureOutput"_ustr) || node->is_type("NodeClosureInput"_ustr) ||
         node->is_type("NodeEvaluateClosure"_ustr) || node->is_type("NodeCombineBundle"_ustr) ||
         node->is_type("NodeSeparateBundle"_ustr) || node->is_type("NodeJoinBundle"_ustr) ||
         node->is_type("GeometryNodeMenuSwitch"_ustr) ||
         node->is_type("FunctionNodeInputMenu"_ustr)))
    {
      /* These control/capture paths need separate provenance rules. Keep compiling them exactly
       * as before, but never claim that their uniforms can be updated independently. */
      params_.r_uniform_tracking->unsupported_context = true;
    }
    if (node->is_reroute()) {
      this->handle_output_socket__reroute(socket);
      return;
    }
    if (node->is_muted()) {
      if (!this->handle_output_socket__internal_links(socket)) {
        /* The output socket does not have a corresponding input, so the value is ignored. */
        this->store_socket_value_dangling(socket);
      }
      return;
    }
    if (node->type_legacy == SH_NODE_PORTAL_OUT) {
      this->handle_output_socket__portal_out(socket);
      return;
    }
    if (node->is_group()) {
      this->handle_output_socket__group(socket);
      return;
    }
    if (node->is_group_input()) {
      this->handle_output_socket__group_input(socket);
      return;
    }
    if (node->is_type("GeometryNodeRepeatOutput"_ustr)) {
      if (this->should_preserve_repeat_zone_node(*node)) {
        this->handle_output_socket__preserved_repeat_output(socket);
        return;
      }
      this->handle_output_socket__repeat_output(socket);
      return;
    }
    if (node->is_type("GeometryNodeRepeatInput"_ustr)) {
      if (this->should_preserve_repeat_zone_node(*node)) {
        this->handle_output_socket__preserved_repeat_input(socket);
        return;
      }
      this->handle_output_socket__repeat_input(socket);
      return;
    }
    if (node->is_type("ShaderNodeForeachLightOutput"_ustr)) {
      if (this->should_preserve_foreach_light_zone_node(*node)) {
        this->handle_output_socket__preserved_foreach_light_output(socket);
        return;
      }
    }
    if (node->is_type("ShaderNodeForeachLightInput"_ustr)) {
      if (this->should_preserve_foreach_light_zone_node(*node)) {
        this->handle_output_socket__preserved_foreach_light_input(socket);
        return;
      }
    }
    if (node->is_type("NodeClosureOutput"_ustr)) {
      this->handle_output_socket__closure_output(socket);
      return;
    }
    if (node->is_type("NodeClosureInput"_ustr)) {
      this->handle_output_socket__closure_input(socket);
      return;
    }
    if (node->is_type("NodeEvaluateClosure"_ustr)) {
      this->handle_output_socket__evaluate_closure(socket);
      return;
    }
    if (node->is_type("NodeCombineBundle"_ustr)) {
      this->handle_output_socket__combine_bundle(socket);
      return;
    }
    if (node->is_type("NodeSeparateBundle"_ustr)) {
      this->handle_output_socket__separate_bundle(socket);
      return;
    }
    if (node->is_type("GeometryNodeMenuSwitch"_ustr)) {
      this->handle_output_socket__menu_switch(socket);
      return;
    }
    if (node->is_type("FunctionNodeInputMenu"_ustr)) {
      this->handle_output_socket__input_menu(socket);
      return;
    }
    if (node->is_type("NodeJoinBundle"_ustr)) {
      this->handle_output_socket__join_bundle(socket);
      return;
    }
    if (node->is_type("NodeImplicitConversion"_ustr)) {
      this->handle_output_socket__implicit_conversion(socket);
      return;
    }
    this->handle_output_socket__eval(socket);
  }

  void handle_output_socket__reroute(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    const SocketInContext input_socket = node.input_socket(0);
    this->forward_value_or_schedule(socket, input_socket);
  }

  /* Returns whether the socket was handled. */
  [[nodiscard]] bool handle_output_socket__internal_links(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    for (const bNodeLink &internal_link : node->internal_links()) {
      if (internal_link.tosock == socket.socket) {
        const SocketInContext src_socket = {socket.context, internal_link.fromsock};
        if (src_socket->is_multi_input()) {
          const bNodeLink *src_link = nullptr;
          for (const bNodeLink *link : src_socket->directly_linked_links()) {
            if (link->is_used()) {
              src_link = link;
              break;
            }
          }
          if (!src_link) {
            return false;
          }
          const ComputeContext *from_context = this->get_link_source_context(*src_link,
                                                                             src_socket);
          const SocketInContext origin_socket = {from_context, src_link->fromsock};
          this->forward_value_or_schedule(socket, origin_socket);
          return true;
        }
        if (const SocketValue *value = value_by_socket_.lookup_ptr(src_socket)) {
          /* Pass the value of the internally linked input socket, with an implicit conversion if
           * necessary. */
          this->store_socket_value(
              socket,
              this->handle_implicit_conversion(
                  *value, *internal_link.fromsock->typeinfo, *internal_link.tosock->typeinfo));
          return true;
        }
        this->schedule_socket(src_socket);
        return true;
      }
    }
    return false;
  }

  void handle_output_socket__group(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    const bNodeTree *group = reinterpret_cast<const bNodeTree *>(node->id);
    if (!group || ID_MISSING(&group->id)) {
      this->store_socket_value_fallback(socket);
      return;
    }
    group->ensure_interface_cache();
    group->ensure_topology_cache();
    const bNode *group_output_node = group->group_output_node();
    if (!group_output_node) {
      this->store_socket_value_fallback(socket);
      return;
    }
    /* Get the value of an output of a group node by evaluating the corresponding output of the
     * node group. Since this socket is in a different tree, the compute context is different. */
    const ComputeContext &group_compute_context = compute_context_cache_.for_group_node(
        socket.context, node->identifier, &node->owner_tree());
    const SocketInContext group_output_socket_ctx = {
        &group_compute_context, &group_output_node->input_socket(socket->index())};
    this->forward_value_or_schedule(socket, group_output_socket_ctx);
  }

  void handle_output_socket__group_input(const SocketInContext &socket)
  {
    if (const auto *group_node_compute_context =
            dynamic_cast<const bke::GroupNodeComputeContext *>(socket.context))
    {
      /* Get the value of a group input from the corresponding input socket of the parent group
       * node. */
      const ComputeContext *parent_compute_context = group_node_compute_context->parent();
      const bNode *group_node = group_node_compute_context->node();
      BLI_assert(group_node);
      const bNodeSocket &group_node_input = group_node->input_socket(socket->index());
      const SocketInContext group_input_socket_ctx = {parent_compute_context, &group_node_input};
      this->forward_value_or_schedule(socket, group_input_socket_ctx);
      return;
    }
    this->store_socket_value_fallback(socket);
  }

  void handle_output_socket__portal_out(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    const bNodeSocket *origin_socket = bke::node_tree_runtime::find_shader_portal_origin_socket(
        node->owner_tree(), *node.node);
    if (origin_socket == nullptr) {
      this->store_socket_value_fallback(socket);
      return;
    }
    this->forward_value_or_schedule(socket, {socket.context, origin_socket});
  }

  bool should_preserve_repeat_zone_node(const bNode &repeat_zone_node) const
  {
    BLI_assert(repeat_zone_node.is_type("GeometryNodeRepeatOutput"_ustr) ||
               repeat_zone_node.is_type("GeometryNodeRepeatInput"_ustr));
    if (!params_.allow_preserving_repeat_zones) {
      return false;
    }
    const bNodeTree &tree = repeat_zone_node.owner_tree();
    const bke::bNodeTreeZones *zones = tree.zones();
    if (!zones) {
      return false;
    }
    const bke::bNodeTreeZone *zone = zones->get_zone_by_node(repeat_zone_node.identifier);
    if (!zone) {
      return false;
    }
    const bNode *repeat_zone_input_node = zone->input_node();
    const bNode *repeat_zone_output_node = zone->output_node();
    if (!repeat_zone_input_node || !repeat_zone_output_node) {
      return false;
    }
    const auto &storage = *static_cast<const NodeGeometryRepeatOutput *>(
        repeat_zone_output_node->storage);
    for (const int i : IndexRange(storage.items_num)) {
      const NodeRepeatItem &item = storage.items[i];
      if (!ELEM(item.socket_type, SOCK_INT, SOCK_FLOAT, SOCK_BOOLEAN, SOCK_RGBA, SOCK_VECTOR)) {
        /* Repeat zones with more special types have to be inlined. */
        return false;
      }
    }
    return true;
  }

  bool should_preserve_foreach_light_zone_node(const bNode &foreach_light_zone_node) const
  {
    BLI_assert(foreach_light_zone_node.is_type("ShaderNodeForeachLightOutput"_ustr) ||
               foreach_light_zone_node.is_type("ShaderNodeForeachLightInput"_ustr));
    UNUSED_VARS_NDEBUG(foreach_light_zone_node);
    return params_.allow_preserving_repeat_zones;
  }

  void handle_output_socket__repeat_output(const SocketInContext &socket)
  {
    const bNode &repeat_output_node = socket->owner_node();
    const bNodeTree &tree = socket->owner_tree();

    const bke::bNodeTreeZones *zones = tree.zones();
    if (!zones) {
      this->store_socket_value_fallback(socket);
      return;
    }
    const bke::bNodeTreeZone *zone = zones->get_zone_by_node(repeat_output_node.identifier);
    if (!zone) {
      this->store_socket_value_fallback(socket);
      return;
    }
    const NodeInContext repeat_input_node = {socket.context, zone->input_node()};
    const SocketInContext iterations_input = repeat_input_node.input_socket(0);
    const SocketValue *iterations_socket_value = value_by_socket_.lookup_ptr(iterations_input);
    if (!iterations_socket_value) {
      /* The number of iterations is not known yet, so only schedule that socket for now. */
      this->schedule_socket(iterations_input);
      return;
    }
    const std::optional<PrimitiveSocketValue> iterations_value_opt =
        iterations_socket_value->to_primitive(*iterations_input->typeinfo);
    if (!iterations_value_opt) {
      this->add_dynamic_repeat_zone_iterations_error(*repeat_input_node);
    }
    const int iterations = iterations_value_opt.has_value() ?
                               std::get<int>(iterations_value_opt->value) :
                               0;
    if (iterations <= 0) {
      /* If the number of iterations is zero, the values are copied directly from the repeat input
       * node. */
      const SocketInContext origin_socket = repeat_input_node.input_socket(1 + socket->index());
      this->forward_value_or_schedule(socket, origin_socket);
      return;
    }
    /* Otherwise, the value is copied from the output of the last iteration. */
    const ComputeContext &last_iteration_context = compute_context_cache_.for_repeat_zone(
        socket.context, repeat_output_node, iterations - 1);
    parent_zone_contexts_.add(&last_iteration_context, socket.context);
    const SocketInContext origin_socket = {&last_iteration_context,
                                           &repeat_output_node.input_socket(socket->index())};
    this->forward_value_or_schedule(socket, origin_socket);
  }

  void handle_output_socket__preserved_repeat_output(const SocketInContext &socket)
  {
    const bNodeTree &tree = socket->owner_tree();
    const NodeInContext repeat_output_node = socket.owner_node();
    const bke::bNodeTreeZones &zones = *tree.zones();
    const bke::bNodeTreeZone &zone = *zones.get_zone_by_node(repeat_output_node->identifier);
    const bNode &repeat_input_node = *zone.input_node();

    const EnsureInputsResult ensured_inputs = this->ensure_node_inputs(socket.owner_node());
    if (ensured_inputs.has_missing_inputs) {
      /* The node can only be evaluated if all inputs values are known. */
      return;
    }
    const NodeInContext node = socket.owner_node();
    bNode &copied_node = this->handle_output_socket__eval_copy_node(node);
    PreservedZone &preserved_zone = copied_zone_by_zone_output_node_.lookup_or_add_default(
        repeat_output_node);
    preserved_zone.output_node = &copied_node;
    /* Ensure that the repeat input node is created as well. */
    this->schedule_socket({node.context, &repeat_input_node.output_socket(0)});
  }

  void handle_output_socket__preserved_repeat_input(const SocketInContext &socket)
  {
    const EnsureInputsResult ensured_inputs = this->ensure_node_inputs(socket.owner_node());
    if (ensured_inputs.has_missing_inputs) {
      /* The node can only be evaluated if all inputs values are known. */
      return;
    }
    const bNodeTree &tree = socket->owner_tree();
    const NodeInContext node = socket.owner_node();
    bNode &copied_node = this->handle_output_socket__eval_copy_node(node);
    const auto &storage = *static_cast<const NodeGeometryRepeatInput *>(node->storage);
    const NodeInContext repeat_output_node{node.context, tree.node_by_id(storage.output_node_id)};
    PreservedZone &preserved_zone = copied_zone_by_zone_output_node_.lookup_or_add_default(
        repeat_output_node);
    preserved_zone.input_node = &copied_node;
  }

  void handle_output_socket__preserved_foreach_light_output(const SocketInContext &socket)
  {
    const bNodeTree &tree = socket->owner_tree();
    const NodeInContext foreach_light_output_node = socket.owner_node();
    const bke::bNodeTreeZones &zones = *tree.zones();
    const bke::bNodeTreeZone &zone = *zones.get_zone_by_node(foreach_light_output_node->identifier);
    const bNode &foreach_light_input_node = *zone.input_node();

    const EnsureInputsResult ensured_inputs = this->ensure_node_inputs(socket.owner_node());
    if (ensured_inputs.has_missing_inputs) {
      /* The node can only be evaluated if all inputs values are known. */
      return;
    }
    const NodeInContext node = socket.owner_node();
    bNode &copied_node = this->handle_output_socket__eval_copy_node(node);
    PreservedZone &preserved_zone = copied_zone_by_zone_output_node_.lookup_or_add_default(
        foreach_light_output_node);
    preserved_zone.output_node = &copied_node;
    /* Ensure that the paired input node is created as well. */
    this->schedule_socket({node.context, &foreach_light_input_node.output_socket(0)});
  }

  void handle_output_socket__preserved_foreach_light_input(const SocketInContext &socket)
  {
    const EnsureInputsResult ensured_inputs = this->ensure_node_inputs(socket.owner_node());
    if (ensured_inputs.has_missing_inputs) {
      /* The node can only be evaluated if all inputs values are known. */
      return;
    }
    const bNodeTree &tree = socket->owner_tree();
    const NodeInContext node = socket.owner_node();
    bNode &copied_node = this->handle_output_socket__eval_copy_node(node);
    const auto &storage = *static_cast<const NodeShaderForeachLightInput *>(node->storage);
    const NodeInContext foreach_light_output_node{
        node.context, tree.node_by_id(storage.output_node_id)};
    PreservedZone &preserved_zone = copied_zone_by_zone_output_node_.lookup_or_add_default(
        foreach_light_output_node);
    preserved_zone.input_node = &copied_node;
  }

  void add_dynamic_repeat_zone_iterations_error(const bNode &repeat_input_node)
  {
    params_.r_error_messages.append(
        {&repeat_input_node, TIP_("Iterations input has to be a constant value")});
  }

  void handle_output_socket__repeat_input(const SocketInContext &socket)
  {
    const bNode &repeat_input_node = socket->owner_node();
    const auto *repeat_zone_context = dynamic_cast<const bke::RepeatZoneComputeContext *>(
        socket.context);
    if (!repeat_zone_context) {
      this->store_socket_value_fallback(socket);
      return;
    }
    /* The index of the current iteration comes from the context. */
    const int iteration = repeat_zone_context->iteration();

    if (socket->index() == 0) {
      /* The first output is the current iteration index. */
      this->store_socket_value(socket, {PrimitiveSocketValue{iteration}});
      return;
    }

    if (iteration == 0) {
      /* In the first iteration, the values are copied from the corresponding input socket. */
      const SocketInContext origin_socket = {repeat_zone_context->parent(),
                                             &repeat_input_node.input_socket(socket->index())};
      this->forward_value_or_schedule(socket, origin_socket);
      return;
    }
    /* For later iterations, the values are copied from the corresponding output of the previous
     * iteration. */
    const bNode &repeat_output_node = *repeat_input_node.owner_tree().node_by_id(
        repeat_zone_context->output_node_id());
    const int previous_iteration = iteration - 1;
    const ComputeContext &previous_iteration_context = compute_context_cache_.for_repeat_zone(
        repeat_zone_context->parent(), repeat_output_node, previous_iteration);
    parent_zone_contexts_.add(&previous_iteration_context, repeat_zone_context->parent());
    const SocketInContext origin_socket = {&previous_iteration_context,
                                           &repeat_output_node.input_socket(socket->index() - 1)};
    this->forward_value_or_schedule(socket, origin_socket);
  }

  void handle_output_socket__closure_output(const SocketInContext &socket)
  {
    const bNode &node = socket->owner_node();
    const bke::bNodeTreeZones *zones = node.owner_tree().zones();
    if (!zones) {
      this->store_socket_value_fallback(socket);
      return;
    }
    const bke::bNodeTreeZone *zone = zones->get_zone_by_node(node.identifier);
    if (!zone) {
      this->store_socket_value_fallback(socket);
      return;
    }
    /* Just store a reference to the closure. */
    this->store_socket_value(socket, {ClosureZoneValue{zone, socket.context}});
  }

  void handle_output_socket__evaluate_image(const SocketInContext &socket, const bNode &image_node)
  {
    const NodeInContext evaluate_node = socket.owner_node();
    const auto &storage = *static_cast<const NodeEvaluateClosure *>(evaluate_node->storage);
    std::optional<SocketInContext> uv_socket;
    /* Only the native image adapter accepts the old coordinate key. The canonical key wins
     * regardless of item order or which input is linked, including when UV is invalid. */
    for (const char *key : {"UV", "Vector"}) {
      for (const int i : IndexRange(storage.input_items.items_num)) {
        if (STREQ(storage.input_items.items[i].name, key)) {
          uv_socket = evaluate_node.input_socket(i + 1);
          break;
        }
      }
      if (uv_socket) {
        break;
      }
    }

    std::string coordinate_error;
    if (!uv_socket) {
      coordinate_error =
          "Image closure coordinates require a 'UV' vector input (legacy 'Vector' "
          "is also accepted); synchronize the Evaluate Closure interface";
    }
    else {
      const bNodeSocket &uv = **uv_socket;
      const auto shape = storage.input_items.items[uv.index() - 1].structure_type;
      const int dimensions = uv.type == SOCK_VECTOR ?
                                 uv.default_value_typed<bNodeSocketValueVector>()->dimensions :
                                 0;
      if (uv.type != SOCK_VECTOR || !ELEM(dimensions, 0, 3) ||
          !ELEM(shape,
                NodeSocketInterfaceStructureType::Auto,
                NodeSocketInterfaceStructureType::Single))
      {
        coordinate_error = fmt::format(
            "Image closure coordinates '{}' must be a single 3D vector; synchronize the "
            "Evaluate Closure interface",
            uv.name);
      }
    }
    if (!coordinate_error.empty()) {
      params_.r_error_messages.append({evaluate_node.node, std::move(coordinate_error), true});
      for (const int i : IndexRange(storage.output_items.items_num)) {
        this->store_socket_value_fallback(evaluate_node.output_socket(i));
      }
      return;
    }
    if (!value_by_socket_.contains(*uv_socket)) {
      this->schedule_socket(*uv_socket);
      return;
    }

    bNode &sample_node = *this->add_node("ShaderNodeImageClosureSample"_ustr);
    MEM_delete(static_cast<NodeShaderImageToClosure *>(sample_node.storage));
    sample_node.storage = MEM_new<NodeShaderImageToClosure>(
        __func__,
        dna::shallow_copy(*static_cast<const NodeShaderImageToClosure *>(image_node.storage)));
    sample_node.id = image_node.id;
    if (!(this->node_copy_flag() & LIB_ID_CREATE_NO_USER_REFCOUNT)) {
      id_us_plus(sample_node.id);
    }
    /* The destination tree's topology cache is not built while nodes are being inserted. */
    bNodeSocket &sample_input = *static_cast<bNodeSocket *>(sample_node.inputs.first);
    const SocketValue uv = this->handle_implicit_conversion(
        value_by_socket_.lookup(*uv_socket), *(*uv_socket)->typeinfo, *sample_input.typeinfo);
    this->set_input_socket_value(*evaluate_node, sample_node, sample_input, uv);

    for (const int i : IndexRange(storage.output_items.items_num)) {
      const SocketInContext output = evaluate_node.output_socket(i);
      if (value_by_socket_.contains(output)) {
        continue;
      }
      const StringRef name = storage.output_items.items[i].name;
      const int sample_index = name == "Color" ? 0 : name == "Alpha" ? 1 : -1;
      if (sample_index == -1) {
        this->store_socket_value_fallback(output);
        continue;
      }
      bNodeSocket &sample_output = *static_cast<bNodeSocket *>(
          BLI_findlink(&sample_node.outputs, sample_index));
      this->store_socket_value(
          output,
          this->handle_implicit_conversion({LinkedSocketValue{&sample_node, &sample_output}},
                                           *sample_output.typeinfo,
                                           *output->typeinfo));
    }
  }

  void handle_output_socket__evaluate_closure(const SocketInContext &socket)
  {
    const NodeInContext evaluate_closure_node = socket.owner_node();
    const SocketInContext closure_input_socket = evaluate_closure_node.input_socket(0);
    const SocketValue *closure_input_value = value_by_socket_.lookup_ptr(closure_input_socket);
    if (!closure_input_value) {
      /* The closure to evaluate is not known yet, so schedule the closure input before it can be
       * evaluated. */
      this->schedule_socket(closure_input_socket);
      return;
    }
    if (const auto *image = std::get_if<LinkedSocketValue>(&closure_input_value->value)) {
      if (image->node->is_type("ShaderNodeImageToClosure"_ustr)) {
        this->handle_output_socket__evaluate_image(socket, *image->node);
        return;
      }
    }
    const ClosureZoneValue *closure_zone_value = std::get_if<ClosureZoneValue>(
        &closure_input_value->value);
    if (!closure_zone_value) {
      /* If the closure is null, the node behaves as if it is muted. */
      if (!this->handle_output_socket__internal_links(socket)) {
        this->store_socket_value_fallback(socket);
      }
      return;
    }
    const auto *evaluate_closure_storage = static_cast<const NodeEvaluateClosure *>(
        evaluate_closure_node->storage);
    const bNode &closure_output_node = *closure_zone_value->zone->output_node();
    const auto &closure_storage = *static_cast<const NodeClosureOutput *>(
        closure_output_node.storage);
    const StringRef key = evaluate_closure_storage->output_items.items[socket->index()].name;

    const ClosureSourceLocation closure_source_location{
        &closure_output_node.owner_tree(),
        closure_output_node.identifier,
        closure_zone_value->closure_creation_context ?
            closure_zone_value->closure_creation_context->hash() :
            ComputeContextHash{},
        closure_zone_value->closure_creation_context};
    const bke::EvaluateClosureComputeContext &closure_eval_context =
        compute_context_cache_.for_evaluate_closure(socket.context,
                                                    evaluate_closure_node->identifier,
                                                    &socket->owner_tree(),
                                                    closure_source_location);
    parent_zone_contexts_.add(&closure_eval_context, closure_zone_value->closure_creation_context);

    if (closure_eval_context.is_recursive()) {
      this->store_socket_value_fallback(socket);
      params_.r_error_messages.append(
          {&*evaluate_closure_node, TIP_("Recursive closures are not supported")});
      return;
    }

    for (const int i : IndexRange(closure_storage.output_items.items_num)) {
      const NodeClosureOutputItem &item = closure_storage.output_items.items[i];
      if (key != item.name) {
        continue;
      }
      /* Get the value of the output by evaluating the corresponding output in the closure zone. */
      const SocketInContext origin_socket = {&closure_eval_context,
                                             &closure_output_node.input_socket(i)};
      this->forward_value_or_schedule(socket, origin_socket);
      return;
    }
    this->store_socket_value_fallback(socket);
  }

  void handle_output_socket__closure_input(const SocketInContext &socket)
  {
    const bNode &closure_input_node = socket->owner_node();
    const auto *closure_eval_context = dynamic_cast<const bke::EvaluateClosureComputeContext *>(
        socket.context);
    if (!closure_eval_context) {
      this->store_socket_value_fallback(socket);
      return;
    }
    const bNode &closure_output_node = *closure_input_node.owner_tree().node_by_id(
        closure_eval_context->closure_source_location()->closure_output_node_id);
    const NodeInContext closure_eval_node = {closure_eval_context->parent(),
                                             closure_eval_context->node()};

    const auto &closure_storage = *static_cast<const NodeClosureOutput *>(
        closure_output_node.storage);
    const auto &eval_closure_storage = *static_cast<const NodeEvaluateClosure *>(
        closure_eval_node->storage);

    const StringRef key = closure_storage.input_items.items[socket->index()].name;
    for (const int i : IndexRange(eval_closure_storage.input_items.items_num)) {
      const NodeEvaluateClosureInputItem &item = eval_closure_storage.input_items.items[i];
      if (key != item.name) {
        continue;
      }
      /* The input of a closure zone gets its value from the corresponding input of the Evaluate
       * Closure node that evaluates it. */
      const SocketInContext origin_socket = closure_eval_node.input_socket(i + 1);
      this->forward_value_or_schedule(socket, origin_socket);
      return;
    }
    this->store_socket_value_fallback(socket);
  }

  void handle_output_socket__combine_bundle(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    const auto &storage = *static_cast<const NodeCombineBundle *>(node->storage);

    bool all_inputs_available = true;
    for (const bNodeSocket *input_socket : node->input_sockets()) {
      const SocketInContext input_socket_ctx = {socket.context, input_socket};
      if (!value_by_socket_.lookup_ptr(input_socket_ctx)) {
        this->schedule_socket(input_socket_ctx);
        all_inputs_available = false;
      }
    }
    if (!all_inputs_available) {
      /* Can't create the bundle yet. Wait until all inputs are available. */
      return;
    }
    /* Build the actual bundle socket value from the input values. */
    auto bundle_value = std::make_shared<BundleSocketValue>();
    for (const int i : IndexRange(storage.items_num)) {
      const SocketInContext input_socket = node.input_socket(i);
      const NodeCombineBundleItem &item = storage.items[i];
      const StringRef key = item.name;
      const auto &socket_value = value_by_socket_.lookup(input_socket);
      const int dimensions =
          input_socket->type == SOCK_VECTOR ?
              input_socket->default_value_typed<bNodeSocketValueVector>()->dimensions :
              0;
      bundle_value->items.append({key, socket_value, input_socket->typeinfo, dimensions});
    }
    this->store_socket_value(socket, {bundle_value});
  }

  void handle_output_socket__separate_bundle(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    const auto &storage = *static_cast<const NodeSeparateBundle *>(node->storage);

    const SocketInContext input_socket = node.input_socket(0);
    const SocketValue *socket_value = value_by_socket_.lookup_ptr(input_socket);
    if (!socket_value) {
      /* The input bundle is not known yet, so schedule it for now. */
      this->schedule_socket(input_socket);
      return;
    }
    const auto *bundle_value_ptr = std::get_if<BundleSocketValuePtr>(&socket_value->value);
    if (!bundle_value_ptr) {
      if (const auto *invalid = std::get_if<InvalidBundleValue>(&socket_value->value);
          invalid && !invalid->reported)
      {
        params_.r_error_messages.append({node.node, invalid->message, true});
      }
      /* The bundle is empty. Use the fallback value. */
      this->store_socket_value_fallback(socket);
      return;
    }
    const BundleSocketValue &bundle_value = **bundle_value_ptr;

    const StringRef key = storage.items[socket->index()].name;
    for (const BundleSocketValue::Item &item : bundle_value.items) {
      if (key != item.key) {
        continue;
      }
      if (bundle_value.strict) {
        const int dimensions =
            socket->type == SOCK_VECTOR ?
                socket->default_value_typed<bNodeSocketValueVector>()->dimensions :
                0;
        const bool color_alias = (item.socket_type->type == SOCK_RGBA &&
                                  socket->type == SOCK_VECTOR && ELEM(dimensions, 3, 4)) ||
                                 (item.socket_type->type == SOCK_VECTOR &&
                                  socket->type == SOCK_RGBA && ELEM(item.dimensions, 3, 4));
        const bool same = item.socket_type->type == socket->type &&
                          (socket->type != SOCK_VECTOR ||
                           (item.dimensions == 0 ? 3 : item.dimensions) ==
                               (dimensions == 0 ? 3 : dimensions));
        if (!same && !color_alias) {
          params_.r_error_messages.append(
              {node.node,
               fmt::format("GLSL bundle field '{}' has an incompatible Separate Bundle type or "
                           "dimension; synchronize its interface",
                           key),
               true});
          this->store_socket_value_fallback(socket);
          return;
        }
      }
      /* Extract the value from the bundle. */
      const SocketValue converted_value = this->handle_implicit_conversion(
          item.value, *item.socket_type, *socket->typeinfo);
      this->store_socket_value(socket, converted_value);
      return;
    }
    if (bundle_value.strict) {
      params_.r_error_messages.append(
          {node.node,
           fmt::format(
               "GLSL bundle field '{}' is no longer present; synchronize this Separate Bundle",
               key),
           true});
    }
    /* Legacy, untyped bundles retain their existing missing-key behavior. */
    this->store_socket_value_fallback(socket);
  }

  void handle_output_socket__join_bundle(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    const SocketInContext input_socket = node.input_socket(0);
    const SocketValue *socket_value = value_by_socket_.lookup_ptr(input_socket);
    if (!socket_value) {
      /* The input bundles are not known yet, so schedule them for now. */
      this->schedule_socket(input_socket);
      return;
    }
    const auto &multi_input_value = *std::get_if<MultiInputValue>(&socket_value->value);
    if (multi_input_value.values.is_empty()) {
      /* The input is empty, so use the fallback value. */
      this->store_socket_value_fallback(socket);
      return;
    }

    Set<StringRef> existing_keys;
    auto joined_bundle = std::make_shared<BundleSocketValue>();
    for (const SocketValue &value : multi_input_value.values) {
      if (std::holds_alternative<InvalidBundleValue>(value.value)) {
        this->store_socket_value(socket, value);
        return;
      }
      const auto *bundle_value = std::get_if<BundleSocketValuePtr>(&value.value);
      if (!bundle_value || !*bundle_value) {
        /* Ignore invalid values. */
        continue;
      }
      joined_bundle->strict |= (*bundle_value)->strict;
      for (const BundleSocketValue::Item &item : (*bundle_value)->items) {
        if (existing_keys.add(item.key)) {
          joined_bundle->items.append(item);
        }
      }
    }
    this->store_socket_value(socket, {BundleSocketValuePtr{joined_bundle}});
  }

  void handle_output_socket__menu_switch(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    const auto &storage = *static_cast<const NodeMenuSwitch *>(node->storage);

    const SocketInContext menu_input = node.input_socket(0);
    const SocketValue *menu_socket_value = value_by_socket_.lookup_ptr(menu_input);
    if (!menu_socket_value) {
      /* The menu value is not known yet, so schedule it for now. */
      this->schedule_socket(menu_input);
      return;
    }

    const std::optional<PrimitiveSocketValue> menu_value_opt = menu_socket_value->to_primitive(
        *menu_input->typeinfo);
    if (!menu_value_opt) {
      /* This limitation may be lifted in the future. Menu Switch nodes could be supported natively
       * by render engines or we convert them to a bunch of mix nodes. */
      this->store_socket_value_fallback(socket);
      params_.r_error_messages.append({node.node, TIP_("Menu value has to be a constant value")});
      return;
    }
    const MenuValue menu_value = std::get<MenuValue>(menu_value_opt->value);
    /* Find the selected item index. */
    std::optional<int> selected_index;
    for (const int item_i : IndexRange(storage.enum_definition.items_num)) {
      const NodeEnumItem &item = storage.enum_definition.items_array[item_i];
      if (MenuValue(item.identifier) == menu_value) {
        selected_index = item_i;
        break;
      }
    }
    if (!selected_index.has_value()) {
      /* The input value does not exist in the menu. */
      this->store_socket_value_fallback(socket);
      return;
    }
    if (socket->index() == 0) {
      /* Handle forwarding the selected value. */
      this->forward_value_or_schedule(socket, node.input_socket(*selected_index + 1));
      return;
    }
    /* Set the value of the mask output. */
    const bool is_selected = selected_index == socket->index() - 1;
    this->store_socket_value(socket, {PrimitiveSocketValue{is_selected}});
  }

  void handle_output_socket__input_menu(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    const auto &storage = *static_cast<const NodeInputMenu *>(node->storage);
    SocketInContext output_socket = node.output_socket(0);
    this->store_socket_value(output_socket,
                             {PrimitiveSocketValue::from_value(
                                 {output_socket->typeinfo->base_cpp_type, &storage.value})});
  }

  void handle_output_socket__implicit_conversion(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();

    const SocketInContext input_socket = node.input_socket(0);
    const SocketValue *socket_value = value_by_socket_.lookup_ptr(input_socket);
    if (!socket_value) {
      /* The input bundle is not known yet, so schedule it for now. */
      this->schedule_socket(input_socket);
      return;
    }
    this->uniform_source_unsafe(*socket_value);
    const SocketValue converted_value = this->handle_implicit_conversion(
        *socket_value, *socket->typeinfo, *socket->typeinfo);
    this->store_socket_value(socket, converted_value);
  }

  /**
   * Evaluate a node to compute the value of the given output socket. This may also compute all the
   * other outputs of the node.
   */
  void handle_output_socket__eval(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    const EnsureInputsResult ensured_inputs = this->ensure_node_inputs(node);
    if (ensured_inputs.has_missing_inputs) {
      /* The node can only be evaluated if all inputs values are known. */
      return;
    }
    if (node->is_type("ShaderNodeGLSLFunction"_ustr)) {
      bool has_bundle = false;
      for (const bNodeSocket *input : node->input_sockets()) {
        has_bundle |= input->type == SOCK_BUNDLE;
      }
      for (const bNodeSocket *output : node->output_sockets()) {
        has_bundle |= output->type == SOCK_BUNDLE;
      }
      if (has_bundle) {
        this->handle_glsl_bundle_node(node);
        return;
      }
    }
    const bke::bNodeType &node_type = *node->typeinfo;
    if (node_type.build_multi_function && ensured_inputs.all_inputs_primitive) {
      /* Do constant folding. */
      this->handle_output_socket__eval_multi_function(node);
      return;
    }
    /* The node can't be constant-folded. So copy it to the destination tree instead. */
    this->handle_output_socket__eval_copy_node(node);
  }

  static bNodeSocket *bundle_socket_by_identifier(bNode &node,
                                                  const eNodeSocketInOut direction,
                                                  StringRef identifier)
  {
    for (bNodeSocket *socket : direction == SOCK_IN ? node.input_sockets() : node.output_sockets())
    {
      if (identifier == socket->identifier) {
        return socket;
      }
    }
    return nullptr;
  }

  void handle_glsl_bundle_node(const NodeInContext &node)
  {
    GLSLBundleInterface interface;
    std::string error;
    bool upstream_error_reported = false;
    auto fail = [&]() {
      std::string location = node->name;
      for (const ComputeContext *context = node.context; context; context = context->parent()) {
        if (const auto *group = dynamic_cast<const bke::GroupNodeComputeContext *>(context)) {
          if (group->node()) {
            location = std::string(group->node()->name) + " / " + location;
          }
        }
      }
      const std::string message = "GLSL Function '" + location + "': " + error;
      if (!upstream_error_reported) {
        params_.r_error_messages.append({node.node, message, true});
      }
      for (const bNodeSocket *socket : node->output_sockets()) {
        if (socket->type == SOCK_BUNDLE) {
          this->store_socket_value({node.context, socket}, {InvalidBundleValue{message, true}});
        }
        else {
          this->store_socket_value_fallback({node.context, socket});
        }
      }
    };
    if (!shader_glsl_bundle_interface(*node, interface, error)) {
      fail();
      return;
    }
    auto lowering = std::make_shared<GLSLBundleLowering>();
    struct Binding {
      std::string identifier;
      SocketValue value;
      const bke::bNodeSocketType *source_type = nullptr;
    };
    Vector<Binding> bindings;
    std::function<bool(const GLSLBundleField &,
                       StringRef,
                       const std::string &,
                       const SocketValue *,
                       const bke::bNodeSocketType *,
                       int)>
        bind;
    bind = [&](const GLSLBundleField &field,
               StringRef root,
               const std::string &path,
               const SocketValue *value,
               const bke::bNodeSocketType *actual_type,
               int dimensions) {
      if (value) {
        if (const auto *invalid = std::get_if<InvalidBundleValue>(&value->value)) {
          upstream_error_reported = invalid->reported;
          error = "Bundle field '" + path + "': " + invalid->message;
          return false;
        }
      }
      const int expected_dimensions = field.dimensions;
      const eNodeSocketDatatype actual = actual_type ? actual_type->type : field.socket_type;
      bool compatible = actual == field.socket_type;
      if (ELEM(field.socket_type, SOCK_VECTOR, SOCK_RGBA)) {
        compatible = (actual == SOCK_VECTOR &&
                      (dimensions == 0 ? 3 : dimensions) == expected_dimensions) ||
                     (actual == SOCK_RGBA && ELEM(expected_dimensions, 3, 4));
      }
      if (value && !compatible) {
        error = "Bundle field '" + path + "' expects " + field.type_name + ", got " +
                (actual_type ? std::string(actual_type->idname.c_str()) : "unknown") +
                (actual == SOCK_VECTOR ?
                     " (" + std::to_string(dimensions == 0 ? 3 : dimensions) + "D)" :
                     "");
        return false;
      }
      const auto *input_value = value ? std::get_if<InputSocketValue>(&value->value) : nullptr;
      const bool missing = value == nullptr ||
                           (field.socket_type == SOCK_BUNDLE &&
                            (std::get_if<FallbackValue>(&value->value) ||
                             (input_value && input_value->socket->type == SOCK_BUNDLE)));
      if (missing && field.default_expression) {
        lowering->default_fields.add(path);
        std::function<void(const GLSLBundleField &, const std::string &)> empty;
        empty = [&](const GLSLBundleField &f, const std::string &p) {
          if (f.members.is_empty()) {
            bindings.append({shader_glsl_bundle_leaf_identifier(root, p), {FallbackValue{}}});
          }
          else {
            for (const auto &child : f.members) {
              empty(child, p + "." + child.name);
            }
          }
        };
        empty(field, path);
        return true;
      }
      if (field.socket_type == SOCK_BUNDLE) {
        const BundleSocketValue *bundle = nullptr;
        if (!missing) {
          const auto *ptr = std::get_if<BundleSocketValuePtr>(&value->value);
          if (!ptr || !*ptr || (actual_type && actual_type->type != SOCK_BUNDLE)) {
            error = "Field '" + path + "' expects " + field.type_name + " (Bundle)";
            return false;
          }
          bundle = ptr->get();
        }
        for (const GLSLBundleField &member : field.members) {
          const BundleSocketValue::Item *item = nullptr;
          if (bundle) {
            for (const auto &candidate : bundle->items) {
              if (candidate.key == member.name) {
                item = &candidate;
                break;
              }
            }
          }
          if (!bind(member,
                    root,
                    path + "." + member.name,
                    item ? &item->value : nullptr,
                    item ? item->socket_type : nullptr,
                    item ? item->dimensions : 0))
          {
            return false;
          }
        }
        return true;
      }
      if (missing) {
        error = "Missing required bundle field '" + path + "' (" + field.type_name + ")";
        return false;
      }
      bindings.append({shader_glsl_bundle_leaf_identifier(root, path), *value, actual_type});
      return true;
    };
    for (const GLSLBundleSocket &socket : interface.sockets) {
      if (socket.in_out != SOCK_IN) {
        continue;
      }
      bNodeSocket *source = bundle_socket_by_identifier(
          *const_cast<bNode *>(node.node), SOCK_IN, socket.identifier);
      if (!source) {
        error = "Stale bundle interface '" + socket.identifier + "'; refresh the source";
        fail();
        return;
      }
      const SocketValue &value = value_by_socket_.lookup({node.context, source});
      if (!bind(socket.field, socket.identifier, socket.field.name, &value, source->typeinfo, 0)) {
        fail();
        return;
      }
    }

    Map<const bNodeSocket *, bNodeSocket *> unused_socket_map;
    const int identifier = this->get_next_node_identifier();
    const std::string name = fmt::format("{}_{}", identifier, node->name);
    bNode &copy = *bke::node_copy_with_mapping(
        &dst_tree_,
        *node,
        this->node_copy_flag(),
        name.size() < sizeof(bNode::name) ? std::make_optional<StringRefNull>(name) : std::nullopt,
        identifier,
        unused_socket_map);
    copy.parent = nullptr;
    copy.runtime->original = node->runtime->original ? node->runtime->original :
                                                       const_cast<bNode *>(node.node);
    shader_glsl_bundle_lower_node(dst_tree_, copy, std::move(lowering));
    dst_tree_.ensure_topology_cache();
    for (const bNodeSocket *source : node->input_sockets()) {
      if (source->type == SOCK_BUNDLE || !source->is_available()) {
        continue;
      }
      if (bNodeSocket *target = bundle_socket_by_identifier(copy, SOCK_IN, source->identifier)) {
        this->set_input_socket_value(
            *node, copy, *target, value_by_socket_.lookup({node.context, source}));
      }
    }
    for (const Binding &binding : bindings) {
      bNodeSocket *target = bundle_socket_by_identifier(copy, SOCK_IN, binding.identifier);
      if (!target) {
        error = "Could not materialize bundle leaf '" + binding.identifier + "'";
        fail();
        return;
      }
      const SocketValue converted = binding.source_type ?
                                        this->handle_implicit_conversion(binding.value,
                                                                         *binding.source_type,
                                                                         *target->typeinfo) :
                                        binding.value;
      this->set_input_socket_value(*node, copy, *target, converted);
    }

    std::function<SocketValue(const GLSLBundleField &, StringRef, const std::string &)>
        output_value;
    output_value =
        [&](const GLSLBundleField &field, StringRef root, const std::string &path) -> SocketValue {
      if (field.members.is_empty()) {
        bNodeSocket *socket = bundle_socket_by_identifier(
            copy, SOCK_OUT, shader_glsl_bundle_leaf_identifier(root, path));
        if (!socket) {
          error = "Could not materialize output bundle field '" + path + "'";
          return {FallbackValue{}};
        }
        return {LinkedSocketValue{&copy, socket}};
      }
      auto bundle = std::make_shared<BundleSocketValue>();
      bundle->strict = true;
      for (const GLSLBundleField &member : field.members) {
        bundle->items.append({member.name,
                              output_value(member, root, path + "." + member.name),
                              bke::node_socket_type_find_static(member.socket_type),
                              member.dimensions});
      }
      return {bundle};
    };
    for (const bNodeSocket *source : node->output_sockets()) {
      if (!source->is_available()) {
        continue;
      }
      if (source->type == SOCK_BUNDLE) {
        bool found = false;
        for (const GLSLBundleSocket &schema : interface.sockets) {
          if (schema.in_out == SOCK_OUT && schema.identifier == source->identifier) {
            this->store_socket_value(
                {node.context, source},
                output_value(schema.field, schema.identifier, schema.field.name));
            found = true;
            break;
          }
        }
        if (!found) {
          error = "Stale output bundle interface";
          fail();
          return;
        }
      }
      else if (bNodeSocket *target = bundle_socket_by_identifier(
                   copy, SOCK_OUT, source->identifier))
      {
        this->store_socket_value({node.context, source}, {LinkedSocketValue{&copy, target}});
      }
    }
    if (!error.empty()) {
      fail();
    }
  }

  struct EnsureInputsResult {
    bool has_missing_inputs = false;
    bool all_inputs_primitive = false;
  };

  static bool is_glsl_closure_callback_input(const NodeInContext &node, const bNodeSocket &socket)
  {
    /* Sampler closures also capture values from their creation context. Preserving their raw
     * source zone loses group instance inputs when the GPU helper is compiled later. */
    return node->is_type("ShaderNodeGLSLFunction"_ustr) && socket.type == SOCK_CLOSURE;
  }

  std::optional<SocketValue> ensure_glsl_closure_callback_localized(
      const NodeInContext &consumer_node,
      const bNodeSocket &callback_socket,
      const ClosureZoneValue closure_zone_value)
  {
    const StringRef socket_identifier(callback_socket.identifier);
    const StringRef helper_name = socket_identifier.startswith("closure.") ?
                                      socket_identifier.drop_prefix(8) :
                                      socket_identifier;

    const bke::bNodeTreeZone *zone = closure_zone_value.zone;
    const bNode *closure_input_node = zone ? zone->input_node() : nullptr;
    const bNode *closure_output_node = zone ? zone->output_node() : nullptr;
    if (!closure_input_node || !closure_output_node) {
      params_.r_error_messages.append(
          {consumer_node.node,
           fmt::format("GLSL closure callback '{}' has no valid Closure Input/Output zone",
                       helper_name)});
      return SocketValue{FallbackValue{}};
    }

    const NodeInContext closure_output_node_ctx{closure_zone_value.closure_creation_context,
                                                closure_output_node};
    BLI_assert(!scheduled_sockets_stack_.is_empty());
    const SocketInContext requester_socket = scheduled_sockets_stack_.peek();
    LocalizedClosureZone *localization = localized_closure_zones_.lookup_ptr(
        closure_output_node_ctx);
    if (localization) {
      if (localization->status == LocalizedClosureZone::Status::Complete) {
        return SocketValue{localization->closure_output};
      }
      if (localization->status == LocalizedClosureZone::Status::Failed) {
        return SocketValue{FallbackValue{}};
      }
      const bool is_normal_resume = localization->consumer_node == consumer_node &&
                                    localization->requester_socket == requester_socket &&
                                    localization->requester_stack_depth ==
                                        scheduled_sockets_stack_.size();
      if (!is_normal_resume) {
        localization->status = LocalizedClosureZone::Status::Failed;
        params_.r_error_messages.append(
            {consumer_node.node,
             fmt::format("Recursive GLSL closure callback '{}' is not supported", helper_name)});
        return SocketValue{FallbackValue{}};
      }
    }
    else {
      localized_closure_zones_.add_new(
          closure_output_node_ctx,
          LocalizedClosureZone{LocalizedClosureZone::Status::Materializing,
                               consumer_node,
                               requester_socket,
                               scheduled_sockets_stack_.size(),
                               {}});
      localization = localized_closure_zones_.lookup_ptr(closure_output_node_ctx);

      const NodeInContext closure_input_node_ctx{closure_zone_value.closure_creation_context,
                                                 closure_input_node};
      Map<const bNodeSocket *, bNodeSocket *> socket_map;
      bNode &copied_input_node = this->copy_node_with_input_values(closure_input_node_ctx,
                                                                   socket_map);
      for (const bNodeSocket *src_output_socket : closure_input_node->output_sockets()) {
        if (!src_output_socket->is_available()) {
          continue;
        }
        bNodeSocket *copied_output_socket = socket_map.lookup(src_output_socket);
        value_by_socket_.add_overwrite(
            {closure_zone_value.closure_creation_context, src_output_socket},
            {LinkedSocketValue{&copied_input_node, copied_output_socket}});
      }
      PreservedZone &preserved_zone = copied_zone_by_zone_output_node_.lookup_or_add_default(
          closure_output_node_ctx);
      preserved_zone.input_node = &copied_input_node;
    }

    const EnsureInputsResult ensured_inputs = this->ensure_node_inputs(closure_output_node_ctx);
    if (ensured_inputs.has_missing_inputs) {
      return std::nullopt;
    }
    /* Nested callback localization may have grown the map, so don't retain the old map pointer. */
    localization = localized_closure_zones_.lookup_ptr(closure_output_node_ctx);
    BLI_assert(localization != nullptr);
    if (localization->status == LocalizedClosureZone::Status::Failed) {
      return SocketValue{FallbackValue{}};
    }
    if (localization->status == LocalizedClosureZone::Status::Complete) {
      return SocketValue{localization->closure_output};
    }

    Map<const bNodeSocket *, bNodeSocket *> socket_map;
    bNode &copied_output_node = this->copy_node_with_input_values(closure_output_node_ctx,
                                                                  socket_map);
    bNodeSocket *copied_closure_socket = socket_map.lookup(&closure_output_node->output_socket(0));
    localization->closure_output = {&copied_output_node, copied_closure_socket};
    localization->status = LocalizedClosureZone::Status::Complete;

    PreservedZone &preserved_zone = copied_zone_by_zone_output_node_.lookup_or_add_default(
        closure_output_node_ctx);
    preserved_zone.output_node = &copied_output_node;
    return SocketValue{localization->closure_output};
  }

  EnsureInputsResult ensure_node_inputs(const NodeInContext &node)
  {
    EnsureInputsResult result;
    result.has_missing_inputs = false;
    result.all_inputs_primitive = true;

    /* Resolve ordinary inputs first so callback materialization always records the stable socket
     * and stack depth of the node evaluation, not a sibling dependency scheduled earlier here. */
    for (const bNodeSocket *input_socket : node->input_sockets()) {
      if (!input_socket->is_available()) {
        continue;
      }
      const SocketInContext input_socket_ctx = {node.context, input_socket};
      const SocketValue *value = value_by_socket_.lookup_ptr(input_socket_ctx);
      if (!value) {
        this->schedule_socket(input_socket_ctx);
        result.has_missing_inputs = true;
      }
    }
    if (result.has_missing_inputs) {
      result.all_inputs_primitive = false;
      return result;
    }

    for (const bNodeSocket *input_socket : node->input_sockets()) {
      if (!input_socket->is_available()) {
        continue;
      }
      const SocketInContext input_socket_ctx = {node.context, input_socket};
      const SocketValue *value = value_by_socket_.lookup_ptr(input_socket_ctx);
      BLI_assert(value != nullptr);
      if (is_glsl_closure_callback_input(node, *input_socket)) {
        if (const auto *closure_zone_value = std::get_if<ClosureZoneValue>(&value->value)) {
          const std::optional<SocketValue> localized_value =
              this->ensure_glsl_closure_callback_localized(
                  node, *input_socket, *closure_zone_value);
          if (!localized_value) {
            result.has_missing_inputs = true;
            result.all_inputs_primitive = false;
            return result;
          }
          value_by_socket_.add_overwrite(input_socket_ctx, *localized_value);
          value = value_by_socket_.lookup_ptr(input_socket_ctx);
        }
      }
      if (!value->to_primitive(*input_socket->typeinfo)) {
        result.all_inputs_primitive = false;
      }
    }
    return result;
  }

  void handle_output_socket__eval_multi_function(const NodeInContext &node)
  {
    NodeMultiFunctionBuilder builder{*node.node, node->owner_tree()};
    node->typeinfo->build_multi_function(builder);
    const mf::MultiFunction &fn = builder.function();
    mf::ContextBuilder context;
    IndexMask mask(1);
    mf::ParamsBuilder params{fn, &mask};

    /* Prepare inputs to the multi-function evaluation. */
    for (const bNodeSocket *input_socket : node->input_sockets()) {
      if (!input_socket->is_available()) {
        continue;
      }
      const SocketInContext input_socket_ctx = {node.context, input_socket};
      const SocketValue &input_value = value_by_socket_.lookup(input_socket_ctx);
      this->uniform_source_unsafe(input_value);
      const PrimitiveSocketValue value = *input_value.to_primitive(*input_socket->typeinfo);
      params.add_readonly_single_input(
          GVArray::from_single(*input_socket->typeinfo->base_cpp_type, 1, value.buffer()));
    }

    /* Prepare output buffers. */
    Vector<void *> output_values;
    for (const bNodeSocket *output_socket : node->output_sockets()) {
      if (!output_socket->is_available()) {
        continue;
      }
      void *value = scope_.allocate_owned(*output_socket->typeinfo->base_cpp_type);
      output_values.append(value);
      params.add_uninitialized_single_output(
          GMutableSpan(output_socket->typeinfo->base_cpp_type, value, 1));
    }

    fn.call(mask, params, context);

    /* Store constant-folded values for the output sockets. */
    int current_output_i = 0;
    for (const bNodeSocket *output_socket : node->output_sockets()) {
      if (!output_socket->is_available()) {
        continue;
      }
      const void *value = output_values[current_output_i++];
      SocketValue output_value{
          PrimitiveSocketValue::from_value({output_socket->typeinfo->base_cpp_type, value})};
      if (params_.r_uniform_tracking && node->is_type("ShaderNodeValue"_ustr) &&
          output_socket == node->outputs.first && output_socket->type == SOCK_FLOAT)
      {
        /* Value's multi-function only copies this socket default. All other CPU-folded outputs
         * intentionally lose direct provenance, even when numerically equal to an input. */
        output_value.uniform_source = SocketInContext{node.context, output_socket};
      }
      this->store_socket_value({node.context, output_socket}, std::move(output_value));
    }
  }

  bNode &copy_node_with_input_values(const NodeInContext &node,
                                     Map<const bNodeSocket *, bNodeSocket *> &r_socket_map)
  {
    /* We generate our own identifier and name here to get unique values without having to scan
     * all
     * already existing nodes. */
    const int identifier = this->get_next_node_identifier();
    const std::string unique_name = fmt::format("{}_{}", identifier, node.node->name);
    bNode &copied_node = *bke::node_copy_with_mapping(
        &dst_tree_,
        *node.node,
        this->node_copy_flag(),
        unique_name.size() < sizeof(bNode::name) ? std::make_optional<StringRefNull>(unique_name) :
                                                   std::nullopt,
        identifier,
        r_socket_map);

    /* Clear the parent frame pointer, because it does not exist in the destination tree. */
    copied_node.parent = nullptr;
    copied_node.runtime->original = node.node->runtime->original ?
                                        const_cast<bNode *>(node.node->runtime->original) :
                                        const_cast<bNode *>(node.node);

    /* Setup input sockets for the copied node. */
    for (const bNodeSocket *src_input_socket : node->input_sockets()) {
      if (!src_input_socket->is_available()) {
        continue;
      }
      bNodeSocket &dst_input_socket = *r_socket_map.lookup(src_input_socket);
      const SocketInContext input_socket_ctx = {node.context, src_input_socket};
      const SocketValue &value = value_by_socket_.lookup(input_socket_ctx);
      this->set_input_socket_value(*node, copied_node, dst_input_socket, value);
    }
    return copied_node;
  }

  bNode &handle_output_socket__eval_copy_node(const NodeInContext &node)
  {
    Map<const bNodeSocket *, bNodeSocket *> socket_map;
    bNode &copied_node = this->copy_node_with_input_values(node, socket_map);
    for (const bNodeSocket *src_output_socket : node->output_sockets()) {
      if (!src_output_socket->is_available()) {
        continue;
      }
      bNodeSocket &dst_output_socket = *socket_map.lookup(src_output_socket);
      const SocketInContext output_socket_ctx = {node.context, src_output_socket};
      SocketValue output_value{LinkedSocketValue{&copied_node, &dst_output_socket}};
      if (params_.r_uniform_tracking && node->is_type("ShaderNodeRGB"_ustr) &&
          src_output_socket == node->outputs.first && src_output_socket->type == SOCK_RGBA)
      {
        output_value.uniform_source = output_socket_ctx;
        this->track_uniform_target(copied_node, dst_output_socket, output_value);
      }
      this->store_socket_value(output_socket_ctx, std::move(output_value));
    }
    return copied_node;
  }

  /** Converts the given socket value if necessary. */
  SocketValue handle_implicit_conversion(const SocketValue &src_value,
                                         const bke::bNodeSocketType &from_socket_type,
                                         const bke::bNodeSocketType &to_socket_type)
  {
    if (from_socket_type.type == to_socket_type.type) {
      return src_value;
    }
    this->uniform_source_unsafe(src_value);
    if (std::get_if<LinkedSocketValue>(&src_value.value)) {
      return src_value;
    }
    if (std::get_if<DanglingValue>(&src_value.value)) {
      return src_value;
    }
    const std::optional<PrimitiveSocketValue> src_primitive_value = src_value.to_primitive(
        from_socket_type);
    if (src_primitive_value && to_socket_type.base_cpp_type) {
      if (from_socket_type.type == SOCK_RGBA && to_socket_type.type == SOCK_VECTOR &&
          to_socket_type.idname == "NodeSocketVector4D")
      {
        if (const ColorGeometry4f *color =
                std::get_if<ColorGeometry4f>(&src_primitive_value->value))
        {
          return {PrimitiveSocketValue{
              VectorPrimitiveSocketValue{float3(color->r, color->g, color->b), color->a, 4}}};
        }
      }
      if (from_socket_type.type == SOCK_VECTOR && to_socket_type.type == SOCK_RGBA) {
        if (const auto *vector = std::get_if<VectorPrimitiveSocketValue>(
                &src_primitive_value->value))
        {
          return {
              PrimitiveSocketValue{ColorGeometry4f(vector->xyz.x,
                                                   vector->xyz.y,
                                                   vector->xyz.z,
                                                   vector->dimensions == 4 ? vector->w : 1.0f)}};
        }
      }
      if (data_type_conversions_.is_convertible(*from_socket_type.base_cpp_type,
                                                *to_socket_type.base_cpp_type))
      {
        const void *src_buffer = src_primitive_value->buffer();
        BUFFER_FOR_CPP_TYPE_VALUE(*to_socket_type.base_cpp_type, dst_buffer);
        data_type_conversions_.convert_to_uninitialized(*from_socket_type.base_cpp_type,
                                                        *to_socket_type.base_cpp_type,
                                                        src_buffer,
                                                        dst_buffer);
        return {
            PrimitiveSocketValue::from_value(GPointer{to_socket_type.base_cpp_type, dst_buffer})};
      }
    }
    if (src_primitive_value && to_socket_type.type == SOCK_SHADER) {
      /* Insert a Color node when converting a primitive value to a shader. */
      bNode *color_node = this->add_node("ShaderNodeRGB"_ustr);
      const void *src_buffer = src_primitive_value->buffer();
      ColorGeometry4f color;
      data_type_conversions_.convert_to_uninitialized(
          *from_socket_type.base_cpp_type, CPPType::get<ColorGeometry4f>(), src_buffer, &color);
      bNodeSocket *output_socket = static_cast<bNodeSocket *>(color_node->outputs.first);
      auto *socket_storage = static_cast<bNodeSocketValueRGBA *>(output_socket->default_value);
      copy_v4_v4(socket_storage->value, color);
      return {LinkedSocketValue{color_node, output_socket}};
    }

    return SocketValue{FallbackValue{}};
  }

  void set_input_socket_value(const bNode &original_node,
                              bNode &dst_node,
                              bNodeSocket &dst_socket,
                              const SocketValue &value)
  {
    BLI_assert(dst_socket.is_input());
    const bool track_direct_input = params_.r_uniform_tracking && value.uniform_source &&
                                    uniform_input_supported(dst_node, dst_socket) &&
                                    !(dst_socket.flag & SOCK_HIDE_VALUE) &&
                                    uniform_source_matches_target(value, dst_socket);
    if (params_.r_uniform_tracking && value.uniform_source && !track_direct_input) {
      /* A second, untracked consumer (for example Mix factor pruning) invalidates the source,
       * even if a different consumer already produced a valid uniform binding. */
      this->uniform_source_unsafe(value);
    }
    if (dst_socket.flag & SOCK_HIDE_VALUE) {
      if (const auto *input_socket_value = std::get_if<InputSocketValue>(&value.value)) {
        if (input_socket_value->socket->flag & SOCK_HIDE_VALUE) {
          /* Don't add a value or link of the source and destination sockets don't have a value. */
          return;
        }
      }
    }
    if (const std::optional<PrimitiveSocketValue> primitive_value = value.to_primitive(
            *dst_socket.typeinfo))
    {
      if (dst_socket.flag & SOCK_HIDE_VALUE) {
        /* Can't store the primitive value directly on the socket. So create a new input node and
         * link it instead. */
        const NodeAndSocket node_and_socket = this->primitive_value_to_output_socket(
            *primitive_value);
        if (dst_tree_.typeinfo->validate_link(node_and_socket.socket->typeinfo->type,
                                              dst_socket.typeinfo->type))
        {
          bke::node_add_link(
              dst_tree_, *node_and_socket.node, *node_and_socket.socket, dst_node, dst_socket);
        }
      }
      else {
        this->set_primitive_value_on_socket(dst_socket, *primitive_value);
        if (track_direct_input) {
          this->track_uniform_target(dst_node, dst_socket, value);
        }
      }
      return;
    }
    if (!params_.allow_preserving_repeat_zones) {
      const bool is_iterations_input = dst_node.inputs.first == &dst_socket &&
                                       dst_node.is_type("GeometryNodeRepeatInput"_ustr);
      if (is_iterations_input) {
        this->add_dynamic_repeat_zone_iterations_error(original_node);
        this->set_primitive_value_on_socket(dst_socket, PrimitiveSocketValue{0});
        return;
      }
    }
    if (std::get_if<InputSocketValue>(&value.value)) {
      /* Cases were the input has a primitive value are handled above. */
      return;
    }
    if (std::get_if<FallbackValue>(&value.value)) {
      /* Cases were the input has a primitive fallback value are handled above. */
      return;
    }
    if (std::get_if<DanglingValue>(&value.value)) {
      /* Input sockets should never have a dangling value, because they are replaced by the socket
       * value in #handle_input_socket. */
      BLI_assert_unreachable();
      return;
    }
    if (std::get_if<BundleSocketValuePtr>(&value.value)) {
      /* This type can't be assigned to a socket. The bundle has to be separated first. */
      BLI_assert_unreachable();
      return;
    }
    if (std::get_if<ClosureZoneValue>(&value.value)) {
      if ((original_node.is_type("ShaderNodeGLSLFunction"_ustr) ||
           original_node.is_type("ShaderNodeParallax"_ustr)) &&
          dst_socket.type == SOCK_CLOSURE)
      {
        /* Closure sources are validated later by the consuming shader-node compiler path. */
        return;
      }
      /* This type can't be assigned to a socket. One has to evaluate a closure. */
      BLI_assert_unreachable();
      return;
    }
    if (std::get_if<MultiInputValue>(&value.value)) {
      /* This type can't be assigned to a socket. */
      BLI_assert_unreachable();
      return;
    }
    if (const auto *src_socket_value = std::get_if<LinkedSocketValue>(&value.value)) {
      if (dst_tree_.typeinfo->validate_link(src_socket_value->socket->typeinfo->type,
                                            dst_socket.typeinfo->type))
      {
        bke::node_add_link(
            dst_tree_, *src_socket_value->node, *src_socket_value->socket, dst_node, dst_socket);
      }
      return;
    }
    BLI_assert_unreachable();
  }

  NodeAndSocket primitive_value_to_output_socket(const PrimitiveSocketValue &value)
  {
    if (const float *value_float = std::get_if<float>(&value.value)) {
      bNode *node = this->add_node("ShaderNodeValue"_ustr);
      bNodeSocket *socket = static_cast<bNodeSocket *>(node->outputs.first);
      socket->default_value_typed<bNodeSocketValueFloat>()->value = *value_float;
      return {node, socket};
    }
    if (const int *value_int = std::get_if<int>(&value.value)) {
      bNode *node = this->add_node("FunctionNodeInputInt"_ustr);
      auto &storage = *static_cast<NodeInputInt *>(node->storage);
      storage.integer = *value_int;
      return {node, static_cast<bNodeSocket *>(node->outputs.first)};
    }
    if (const bool *value_bool = std::get_if<bool>(&value.value)) {
      bNode *node = this->add_node("FunctionNodeInputBool"_ustr);
      auto &storage = *static_cast<NodeInputBool *>(node->storage);
      storage.boolean = int(*value_bool);
      return {node, static_cast<bNodeSocket *>(node->outputs.first)};
    }
    if (const VectorPrimitiveSocketValue *value_vector =
            std::get_if<VectorPrimitiveSocketValue>(&value.value))
    {
      bNode *node = this->add_node("FunctionNodeInputVector"_ustr);
      auto &storage = *static_cast<NodeInputVector *>(node->storage);
      copy_v3_v3(storage.vector, value_vector->xyz);
      storage.vector[3] = value_vector->w;
      storage.dimensions = value_vector->dimensions;
      return {node, static_cast<bNodeSocket *>(node->outputs.first)};
    }
    if (const ColorGeometry4f *value_color = std::get_if<ColorGeometry4f>(&value.value)) {
      bNode *node = this->add_node("ShaderNodeRGB"_ustr);
      bNodeSocket *output_socket = static_cast<bNodeSocket *>(node->outputs.first);
      auto *socket_storage = static_cast<bNodeSocketValueRGBA *>(output_socket->default_value);
      copy_v4_v4(socket_storage->value, *value_color);
      return {node, output_socket};
    }
    BLI_assert_unreachable();
    return {};
  }

  bNode *add_node(const UString idname)
  {
    return bke::node_add_node(nullptr, dst_tree_, idname, this->get_next_node_identifier());
  }

  int get_next_node_identifier()
  {
    return ++dst_node_counter_;
  }

  void set_primitive_value_on_socket(bNodeSocket &socket, const PrimitiveSocketValue &value)
  {
    switch (socket.type) {
      case SOCK_FLOAT: {
        socket.default_value_typed<bNodeSocketValueFloat>()->value = std::get<float>(value.value);
        break;
      }
      case SOCK_INT: {
        socket.default_value_typed<bNodeSocketValueInt>()->value = std::get<int>(value.value);
        break;
      }
      case SOCK_BOOLEAN: {
        socket.default_value_typed<bNodeSocketValueBoolean>()->value = std::get<bool>(value.value);
        break;
      }
      case SOCK_VECTOR: {
        const VectorPrimitiveSocketValue &vector_value = std::get<VectorPrimitiveSocketValue>(
            value.value);
        bNodeSocketValueVector *socket_value = socket.default_value_typed<bNodeSocketValueVector>();
        copy_v3_v3(socket_value->value, vector_value.xyz);
        socket_value->value[3] = vector_value.w;
        if (socket_value->dimensions == 0) {
          socket_value->dimensions = vector_value.dimensions;
        }
        break;
      }
      case SOCK_RGBA: {
        copy_v4_v4(socket.default_value_typed<bNodeSocketValueRGBA>()->value,
                   std::get<ColorGeometry4f>(value.value));
        break;
      }
      default: {
        BLI_assert_unreachable();
        break;
      }
    }
  }

  void restore_zones_in_output_tree()
  {
    for (const PreservedZone &copied_zone : copied_zone_by_zone_output_node_.values()) {
      if (!copied_zone.input_node || !copied_zone.output_node) {
        continue;
      }
      const bke::bNodeZoneType *zone_type = bke::zone_type_by_node_type(
          copied_zone.input_node->type_legacy);
      if (!zone_type) {
        continue;
      }
      int &output_id = zone_type->get_corresponding_output_id(*copied_zone.input_node);
      output_id = copied_zone.output_node->identifier;
    }
  }

  void position_nodes_in_output_tree()
  {
    bNodeTree &tree = dst_tree_;
    tree.ensure_topology_cache();

    Map<int, int> num_by_depth;
    Map<bNode *, int> depth_by_node;

    /* Simple algorithm that does a very rough layout of the generated tree. This does not produce
     * great results generally, but is usually good enough when debugging smaller node trees. */
    for (bNode *node : tree.toposort_right_to_left()) {
      int depth = 0;
      for (bNodeSocket *socket : node->output_sockets()) {
        for (bNodeSocket *target : socket->directly_linked_sockets()) {
          depth = std::max(depth, depth_by_node.lookup(&target->owner_node()) + 1);
        }
      }
      depth_by_node.add_new(node, depth);
      const int index_at_depth = num_by_depth.lookup_or_add(depth, 0)++;
      node->location[0] = 200 - depth * 200;
      node->location[1] = -index_at_depth * 300;
    }
  }

  /**
   * Utility to that copies the value of the origin socket to the current socket. If the origin
   * value does not exist yet, the origin socket is only scheduled.
   */
  void forward_value_or_schedule(const SocketInContext &socket, const SocketInContext &origin)
  {
    if (const SocketValue *value = value_by_socket_.lookup_ptr(origin)) {
      if (socket->type == origin->type) {
        this->store_socket_value(socket, *value);
        return;
      }
      this->store_socket_value(
          socket, this->handle_implicit_conversion(*value, *origin->typeinfo, *socket->typeinfo));
      return;
    }
    this->schedule_socket(origin);
  }

  void store_socket_value(const SocketInContext &socket, SocketValue value)
  {
    if (params_.r_uniform_tracking && !value.uniform_source) {
      if (const auto *input = std::get_if<InputSocketValue>(&value.value)) {
        value.uniform_source = input->socket;
      }
    }
    value_by_socket_.add_new(socket, std::move(value));
  }

  void store_socket_value_fallback(const SocketInContext &socket)
  {
    value_by_socket_.add_new(socket, {FallbackValue{}});
  }

  void store_socket_value_dangling(const SocketInContext &socket)
  {
    value_by_socket_.add_new(socket, {DanglingValue{}});
  }

  void schedule_socket(const SocketInContext &socket)
  {
    scheduled_sockets_stack_.push(socket);
  }

  int node_copy_flag() const
  {
    const bool use_refcounting = !(dst_tree_.id.tag & ID_TAG_NO_MAIN);
    return use_refcounting ? 0 : LIB_ID_CREATE_NO_USER_REFCOUNT;
  }
};

}  // namespace

bool inline_shader_node_tree(const bNodeTree &src_tree,
                             bNodeTree &dst_tree,
                             InlineShaderNodeTreeParams &params)
{
  ShaderNodesInliner inliner(src_tree, dst_tree, params);

  if (inliner.do_inline()) {
    /* Update deprecated bNodeSocket.link pointers because some code still depends on it. */
    for (bNode &node : dst_tree.nodes) {
      for (bNodeSocket &sock : node.inputs) {
        sock.link = nullptr;
      }
      for (bNodeSocket &sock : node.outputs) {
        sock.link = nullptr;
      }
    }
    for (bNodeLink &link : dst_tree.links) {
      link.tosock->link = &link;
      BLI_assert(dst_tree.typeinfo->validate_link(link.fromsock->typeinfo->type,
                                                  link.tosock->typeinfo->type));
      link.flag |= NODE_LINK_VALID;
    }
    return true;
  }

  return false;
}

}  // namespace blender::nodes
