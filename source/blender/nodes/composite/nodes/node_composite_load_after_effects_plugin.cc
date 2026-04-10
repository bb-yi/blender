/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_ref.hh"
#include "BLI_ghash.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "DNA_node_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_node.hh"

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

#include "after_effects_host.hh"

namespace blender::nodes::node_composite_load_after_effects_plugin_cc {

using namespace blender::compositor;

static std::mutex runtime_error_cache_mutex;
static std::unordered_map<int, std::string> runtime_error_by_node_id;

static void set_runtime_error(const int node_identifier, const StringRef error)
{
  std::scoped_lock lock(runtime_error_cache_mutex);
  if (error.is_empty()) {
    runtime_error_by_node_id.erase(node_identifier);
    return;
  }
  runtime_error_by_node_id[node_identifier] = error;
}

static std::string get_runtime_error(const int node_identifier)
{
  std::scoped_lock lock(runtime_error_cache_mutex);
  const auto it = runtime_error_by_node_id.find(node_identifier);
  return it == runtime_error_by_node_id.end() ? std::string() : it->second;
}

static const bNodeSocket *find_input_socket_by_identifier(const bNode &node,
                                                          const StringRef identifier)
{
  for (const bNodeSocket *socket : node.input_sockets()) {
    if (socket->identifier == identifier) {
      return socket;
    }
  }
  return nullptr;
}

static std::string get_plugin_path_from_node(const bNode &node)
{
  const bNodeSocket *path_socket = find_input_socket_by_identifier(node, "PLUGIN_PATH");
  if (path_socket == nullptr || path_socket->type != SOCK_STRING || path_socket->default_value == nullptr) {
    return std::string();
  }
  return path_socket->default_value_typed<bNodeSocketValueString>()->value;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::String>("Plugin", "PLUGIN_PATH")
      .subtype(PROP_FILEPATH)
      .path_filter("*.aex;*.dll")
      .description("Path to an Adobe After Effects plugin file (.aex or .dll)");

  b.add_input<decl::Color>("Image", "IMAGE")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value()
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic);

  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic).align_with_previous();

  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }

  const std::string plugin_path = get_plugin_path_from_node(*node);
  if (plugin_path.empty()) {
    return;
  }

  const after_effects::PluginMetadata metadata = after_effects::parse_plugin_metadata(plugin_path);
  if (!metadata.success) {
    return;
  }

  /* Persistent storage for EnumPropertyItem strings and item arrays used by Menu sockets.
   * The declaration builder holds raw pointers into these structures, so they must outlive
   * node_declare. Using a deque for strings (push_back never invalidates existing c_str()
   * pointers) and a pre-reserved vector-of-vectors for the item arrays. */
  static std::deque<std::string> enum_string_pool;
  static std::vector<std::vector<EnumPropertyItem>> enum_item_arrays;

  enum_string_pool.clear();
  enum_item_arrays.clear();

  /* Count menu parameters so we can reserve without later reallocation. */
  size_t num_menus = 0;
  for (const after_effects::PluginParameter &p : metadata.parameters) {
    if (p.type == after_effects::ParamType::Menu && !p.popup_items.empty()) {
      num_menus++;
    }
  }
  enum_item_arrays.reserve(num_menus);

  /* Track panel nesting for GROUP_START/END. */
  std::vector<PanelDeclarationBuilder *> panel_stack;

  for (const after_effects::PluginParameter &parameter : metadata.parameters) {
    if (parameter.type == after_effects::ParamType::GroupEnd) {
      if (!panel_stack.empty()) {
        panel_stack.pop_back();
      }
      continue;
    }

    if (parameter.type == after_effects::ParamType::GroupStart) {
      PanelDeclarationBuilder &panel = panel_stack.empty() ?
          b.add_panel(parameter.name) :
          panel_stack.back()->add_panel(parameter.name);
      panel_stack.push_back(&panel);
      continue;
    }

    if (!parameter.expose_as_socket ||
        ELEM(parameter.type, after_effects::ParamType::MainInput, after_effects::ParamType::Unsupported))
    {
      continue;
    }

    switch (parameter.type) {
      case after_effects::ParamType::Layer:
        if (!panel_stack.empty()) {
          panel_stack.back()->add_input<decl::Color>(parameter.name, parameter.identifier)
              .default_value({0.0f, 0.0f, 0.0f, 0.0f})
              .hide_value()
              .compositor_realization_mode(CompositorInputRealizationMode::None)
              .structure_type(StructureType::Dynamic);
        } else {
          b.add_input<decl::Color>(parameter.name, parameter.identifier)
              .default_value({0.0f, 0.0f, 0.0f, 0.0f})
              .hide_value()
              .compositor_realization_mode(CompositorInputRealizationMode::None)
              .structure_type(StructureType::Dynamic);
        }
        break;
      case after_effects::ParamType::Int:
        if (!panel_stack.empty()) {
          panel_stack.back()->add_input<decl::Int>(parameter.name, parameter.identifier)
              .default_value(int(parameter.default_value))
              .min(int(parameter.min_value))
              .max(int(parameter.max_value));
        } else {
          b.add_input<decl::Int>(parameter.name, parameter.identifier)
              .default_value(int(parameter.default_value))
              .min(int(parameter.min_value))
              .max(int(parameter.max_value));
        }
        break;
      case after_effects::ParamType::Float:
        if (!panel_stack.empty()) {
          panel_stack.back()->add_input<decl::Float>(parameter.name, parameter.identifier)
              .default_value(parameter.default_value)
              .min(parameter.min_value)
              .max(parameter.max_value);
        } else {
          b.add_input<decl::Float>(parameter.name, parameter.identifier)
              .default_value(parameter.default_value)
              .min(parameter.min_value)
              .max(parameter.max_value);
        }
        break;
      case after_effects::ParamType::Bool:
        if (!panel_stack.empty()) {
          panel_stack.back()->add_input<decl::Bool>(parameter.name, parameter.identifier)
              .default_value(parameter.default_value >= 0.5f);
        } else {
          b.add_input<decl::Bool>(parameter.name, parameter.identifier)
              .default_value(parameter.default_value >= 0.5f);
        }
        break;
      case after_effects::ParamType::Color:
        if (!panel_stack.empty()) {
          panel_stack.back()->add_input<decl::Color>(parameter.name, parameter.identifier)
              .default_value({parameter.vector_default.x,
                              parameter.vector_default.y,
                              parameter.vector_default.z,
                              parameter.vector_default.w})
              .hide_value();
        } else {
          b.add_input<decl::Color>(parameter.name, parameter.identifier)
              .default_value({parameter.vector_default.x,
                              parameter.vector_default.y,
                              parameter.vector_default.z,
                              parameter.vector_default.w})
              .hide_value();
        }
        break;
      case after_effects::ParamType::Vector2:
        if (!panel_stack.empty()) {
          panel_stack.back()->add_input<decl::Vector>(parameter.name, parameter.identifier)
              .dimensions(2)
              .default_value(parameter.vector_default);
        } else {
          b.add_input<decl::Vector>(parameter.name, parameter.identifier)
              .dimensions(2)
              .default_value(parameter.vector_default);
        }
        break;
      case after_effects::ParamType::Vector3:
        if (!panel_stack.empty()) {
          panel_stack.back()->add_input<decl::Vector>(parameter.name, parameter.identifier)
              .dimensions(3)
              .default_value(parameter.vector_default);
        } else {
          b.add_input<decl::Vector>(parameter.name, parameter.identifier)
              .dimensions(3)
              .default_value(parameter.vector_default);
        }
        break;
      case after_effects::ParamType::Menu: {
        /* Convert popup items to EnumPropertyItem format. Store strings in the
         * static pool (deque) so c_str() pointers remain valid after node_declare
         * returns. Reserve item array upfront to avoid reallocation (which would
         * invalidate the data() pointer passed to static_items). */
        if (!parameter.popup_items.empty()) {
          enum_item_arrays.emplace_back();
          std::vector<EnumPropertyItem> &items = enum_item_arrays.back();
          items.reserve(parameter.popup_items.size() + 1);

          for (const after_effects::PopupItem &item : parameter.popup_items) {
            enum_string_pool.push_back(item.identifier);
            const char *id = enum_string_pool.back().c_str();
            enum_string_pool.push_back(item.name);
            const char *name = enum_string_pool.back().c_str();
            items.push_back({item.value, id, 0, name, ""});
          }
          items.push_back({0, nullptr, 0, nullptr, nullptr});

          if (!panel_stack.empty()) {
            panel_stack.back()->add_input<decl::Menu>(parameter.name, parameter.identifier)
                .default_value(nodes::MenuValue(int(parameter.default_value)))
                .static_items(items.data());
          } else {
            b.add_input<decl::Menu>(parameter.name, parameter.identifier)
                .default_value(nodes::MenuValue(int(parameter.default_value)))
                .static_items(items.data());
          }
        } else {
          /* Fallback to int if no items. */
          if (!panel_stack.empty()) {
            panel_stack.back()->add_input<decl::Int>(parameter.name, parameter.identifier)
                .default_value(int(parameter.default_value))
                .min(int(parameter.min_value))
                .max(int(parameter.max_value));
          } else {
            b.add_input<decl::Int>(parameter.name, parameter.identifier)
                .default_value(int(parameter.default_value))
                .min(int(parameter.min_value))
                .max(int(parameter.max_value));
          }
        }
        break;
      }
      case after_effects::ParamType::MainInput:
      case after_effects::ParamType::GroupStart:
      case after_effects::ParamType::GroupEnd:
      case after_effects::ParamType::Unsupported:
        break;
    }
  }
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  /* Detect plugin path changes and rebuild sockets here, not in the draw callback.
   * node_update is called whenever the node tree is evaluated (including when the
   * file browser sets a new path), so sockets refresh immediately on file selection. */
  const std::string plugin_path = get_plugin_path_from_node(*node);
  const int path_signature = plugin_path.empty() ?
                                 0 :
                                 int(BLI_ghashutil_strhash_p(plugin_path.c_str()));

  if (node->custom2 != path_signature) {
    node->custom2 = path_signature;
    set_runtime_error(node->identifier, StringRef());
  }

  bke::node_socket_declarations_update(node);
  BKE_ntree_update_tag_node_property(ntree, node);
}

static void node_draw_common(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  const bNode *node = ptr->data_as<bNode>();
  const std::string plugin_path = get_plugin_path_from_node(*node);

  if (!plugin_path.empty()) {
    const after_effects::PluginMetadata metadata = after_effects::parse_plugin_metadata(plugin_path);
    if (!metadata.success && !metadata.error.empty()) {
      layout.label(metadata.error.c_str(), ICON_ERROR);
      return;
    }
  }

  const std::string runtime_error = get_runtime_error(node->identifier);
  if (!runtime_error.empty()) {
    layout.label(runtime_error.c_str(), ICON_ERROR);
  }
}

static void node_draw_buttons(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  node_draw_common(layout, C, ptr);
}

static void node_draw_buttons_ex(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  node_draw_common(layout, C, ptr);
}

static void ae_host_debug_log(const char *format, ...)
{

  std::fprintf(stderr, "[AE Host] ");

  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);

  std::fprintf(stderr, "\n");
}

class LoadAfterEffectsPluginOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &image_input = this->get_input("IMAGE");
    Result &output = this->get_result("Image");

    if (!output.should_compute()) {
      return;
    }

    std::string plugin_path = this->get_input("PLUGIN_PATH").get_single_value_default<std::string>();
    const after_effects::PluginMetadata metadata = after_effects::parse_plugin_metadata(plugin_path);

    std::vector<after_effects::RenderValue> values;
    values.reserve(metadata.parameters.size() + 1);

    {
      after_effects::RenderValue value;
      value.identifier = "IMAGE";
      value.image = &image_input;
      values.push_back(std::move(value));
    }
    

    for (const after_effects::PluginParameter &parameter : metadata.parameters) {
      
      if (!parameter.expose_as_socket) {
        continue;
      }

      const bNodeSocket *socket = find_input_socket_by_identifier(this->node(), parameter.identifier);
      if (socket == nullptr) {
        continue;
      }

      const Result &input = this->get_input(parameter.identifier);

      after_effects::RenderValue value;
      value.identifier = parameter.identifier;
      value.is_linked = socket->is_directly_linked();

      

      switch (parameter.type) {
        case after_effects::ParamType::MainInput:
        case after_effects::ParamType::Layer:
          value.image = &input;
          break;
        case after_effects::ParamType::Int:
          value.integer = input.get_single_value_default<int32_t>();
          value.scalar = float(value.integer);
          // if (parameter.type == after_effects::ParamType::Menu) {
          //   value.menu_value = input.get_single_value_default<nodes::MenuValue>();
          // }
          break;
        case after_effects::ParamType::Float:
          value.scalar = input.get_single_value_default<float>();
          break;
        case after_effects::ParamType::Bool:
          value.boolean = input.get_single_value_default<bool>();
          break;
        case after_effects::ParamType::Color:
          value.vector = float4(input.get_single_value_default<compositor::Color>());
          break;
        case after_effects::ParamType::Vector2:
          value.vector = float4(input.get_single_value_default<float2>(), 0.0f, 0.0f);
          break;
        case after_effects::ParamType::Vector3:
          value.vector = float4(input.get_single_value_default<float3>(), 0.0f);
          break;
        case after_effects::ParamType::GroupStart:
        case after_effects::ParamType::GroupEnd:
        case after_effects::ParamType::Unsupported:
        case after_effects::ParamType::Menu:
          break;
      }

      values.push_back(std::move(value));
    }

    Domain output_domain = image_input.domain();
    std::string error;
    if (after_effects::render_plugin(
            this->context(), plugin_path, values, output, output_domain, error))
    {
      set_runtime_error(this->node().identifier, StringRef());
      return;
    }

    if (!plugin_path.empty() && !error.empty()) {
      set_runtime_error(this->node().identifier, error);
    }

    output.share_data(image_input);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new LoadAfterEffectsPluginOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeLoadAfterEffectsPlugin");
  ntype.ui_name = "Load After Effects Plugin";
  ntype.ui_description = "Load and evaluate an Adobe After Effects plugin (.aex/.dll)";
  ntype.enum_name_legacy = "LOAD_AFTER_EFFECTS_PLUGIN";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.updatefunc = node_update;
  ntype.draw_buttons = node_draw_buttons;
  ntype.draw_buttons_ex = node_draw_buttons_ex;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_load_after_effects_plugin_cc
