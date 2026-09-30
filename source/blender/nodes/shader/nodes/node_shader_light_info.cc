/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "BKE_node_tree_update.hh"
#include "BLI_listbase.h"
#include "BLI_set.hh"
#include "BLI_math_bits.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "DNA_light_types.h"
#include "DNA_object_types.h"
#include "NOD_socket.hh"
#include "RNA_access.hh"
#include "RNA_types.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "../../../blenloader/BLO_read_write.hh"

namespace blender {
namespace nodes::node_shader_light_info_cc {

NODE_STORAGE_FUNCS(NodeShaderLightInfo);

static Light *selected_light(const bNode &node)
{
  auto *object = reinterpret_cast<Object *>(node.id);
  return object && object->type == OB_LAMP ? id_cast<Light *>(object->data) : nullptr;
}

static std::string socket_identifier(const NodeShaderLightInfoBinding &binding,
                                     const bool exists = false)
{
  return std::string(exists ? "Shader Parameter Exists " : "Shader Parameter ") +
         std::to_string(binding.socket_identifier);
}

static bool sync_bindings(bNodeTree &tree, bNode &node)
{
  auto &storage = node_storage(node);
  Light *light = selected_light(node);
  const bool same_source = light && storage.source_light == light;
  bool changed = false;
  if (node.custom1 == 1 && !storage.has_legacy_layout) {
    storage.has_legacy_layout = 1;
    changed = true;
  }
  Set<NodeShaderLightInfoBinding *> matched;
  if (light) {
    for (const LightShaderParameter &parameter : light->shader_parameters) {
      NodeShaderLightInfoBinding *binding = nullptr;
      for (auto &candidate : storage.bindings) {
        if (matched.contains(&candidate)) {
          continue;
        }
        if (same_source ? STREQ(candidate.parameter_identifier, parameter.identifier) :
                          STREQ(candidate.name, parameter.name))
        {
          if (!binding || candidate.available > binding->available) {
            binding = &candidate;
          }
          if (same_source) {
            break;
          }
        }
      }
      if (!binding) {
        binding = MEM_new<NodeShaderLightInfoBinding>(__func__);
        binding->socket_identifier = ++storage.next_socket_identifier;
        binding->parameter_type = parameter.type;
        BLI_addtail(&storage.bindings, binding);
        changed = true;
      }
      if (!binding->available || binding->parameter_type != parameter.type ||
          !STREQ(binding->name, parameter.name) ||
          !STREQ(binding->parameter_identifier, parameter.identifier))
      {
        /* Keep the socket identifier across type changes. The node socket updater preserves
         * links, and shader code generation performs the usual numeric conversions. */
        binding->parameter_type = parameter.type;
        STRNCPY(binding->name, parameter.name);
        STRNCPY(binding->parameter_identifier, parameter.identifier);
        binding->available = 1;
        changed = true;
      }
      matched.add(binding);
    }
    changed |= storage.source_light != light;
    storage.source_light = light;
  }
  for (auto &binding : storage.bindings) {
    if (light && binding.available && !matched.contains(&binding)) {
      binding.available = 0;
      changed = true;
    }
    if (light && !same_source && !matched.contains(&binding) && binding.parameter_identifier[0]) {
      /* Parameter identifiers are local to a Light. An unmatched interface from the previous
       * source must not alias an unrelated parameter in the new source on the next update. */
      binding.parameter_identifier[0] = '\0';
      changed = true;
    }
  }
  /* Only retain missing interfaces when they still protect an existing connection. */
  if (light) {
    for (auto *binding = static_cast<NodeShaderLightInfoBinding *>(storage.bindings.first); binding;) {
      auto *next = binding->next;
      if (!binding->available) {
        if (same_source && binding->parameter_identifier[0]) {
          /* Older files may have one interface per historical type. Merge their wires into
           * the original parameter interface instead of retaining duplicate Missing outputs. */
          for (const auto *active : matched) {
            if (!STREQ(binding->parameter_identifier, active->parameter_identifier)) {
              continue;
            }
            for (const bool exists : {false, true}) {
              bNodeSocket *from = bke::node_find_socket(
                  node, SOCK_OUT, UString(socket_identifier(*binding, exists)));
              bNodeSocket *to = bke::node_find_socket(
                  node, SOCK_OUT, UString(socket_identifier(*active, exists)));
              if (from && to) {
                for (bNodeLink &link : tree.links) {
                  if (link.fromsock == from) {
                    link.fromsock = to;
                    BKE_ntree_update_tag_link_changed(&tree);
                  }
                }
              }
            }
            break;
          }
        }
        const std::string identifier = socket_identifier(*binding);
        const std::string exists_identifier = socket_identifier(*binding, true);
        bool linked = false;
        for (const bNodeLink &link : tree.links) {
          linked |= link.fromnode == &node && link.fromsock &&
                    (identifier == link.fromsock->identifier ||
                     exists_identifier == link.fromsock->identifier);
        }
        if (!linked) {
          BLI_remlink(&storage.bindings, binding);
          MEM_delete(binding);
          changed = true;
        }
      }
      binding = next;
    }
  }
  return changed;
}

static void add_parameter_output(DeclarationListBuilder &builder,
                                 const NodeShaderLightInfoBinding &binding,
                                 const LightShaderParameter *parameter)
{
  const std::string label = parameter ? parameter->name : std::string(binding.name) + " (Missing)";
  const std::string identifier = socket_identifier(binding);
  const PropertySubType subtype = parameter ? PropertySubType(parameter->subtype) : PROP_NONE;
  BaseSocketDeclarationBuilder *socket = nullptr;
  switch (binding.parameter_type) {
    case LIGHT_SHADER_PARAMETER_FLOAT: {
      auto &output = builder.add_output<decl::Float>(UString(label), UString(identifier));
      output.subtype(subtype);
      if (parameter) {
        output.min(parameter->range_min).max(parameter->range_max);
      }
      socket = &output;
      break;
    }
    case LIGHT_SHADER_PARAMETER_INT:
      socket = &builder.add_output<decl::Int>(UString(label), UString(identifier));
      break;
    case LIGHT_SHADER_PARAMETER_BOOL:
      socket = &builder.add_output<decl::Bool>(UString(label), UString(identifier));
      break;
    case LIGHT_SHADER_PARAMETER_VECTOR2:
    case LIGHT_SHADER_PARAMETER_VECTOR3:
    case LIGHT_SHADER_PARAMETER_VECTOR4: {
      auto &output = builder.add_output<decl::Vector>(UString(label), UString(identifier));
      output.dimensions(binding.parameter_type - LIGHT_SHADER_PARAMETER_VECTOR2 + 2).subtype(subtype);
      socket = &output;
      break;
    }
    case LIGHT_SHADER_PARAMETER_COLOR:
      socket = &builder.add_output<decl::Color>(UString(label), UString(identifier));
      break;
  }
  if (socket) {
    socket->description(parameter ? parameter->description :
        "The parameter is missing; outputs zero (color alpha is one)");
  }
  builder.add_output<decl::Bool>(UString(std::string(binding.name) + " Exists"),
                                 UString(socket_identifier(binding, true)))
      .description("True when this parameter can be read from the selected light, "
                   "even when its value is zero");
}

static void declare_legacy(DeclarationListBuilder &b)
{
  b.add_input<decl::Float>("Default"_ustr, "Float Default"_ustr);
  b.add_input<decl::Int>("Default"_ustr, "Int Default"_ustr).min(-16777216).max(16777216);
  b.add_input<decl::Bool>("Default"_ustr, "Boolean Default"_ustr);
  b.add_input<decl::Vector>("Default"_ustr, "Vector2 Default"_ustr).dimensions(2);
  b.add_input<decl::Vector>("Default"_ustr, "Vector3 Default"_ustr);
  b.add_input<decl::Vector>("Default"_ustr, "Vector4 Default"_ustr).dimensions(4);
  b.add_input<decl::Color>("Default"_ustr, "Color Default"_ustr);
  b.add_output<decl::Float>("Value"_ustr, "Float Value"_ustr);
  b.add_output<decl::Int>("Value"_ustr, "Int Value"_ustr);
  b.add_output<decl::Bool>("Value"_ustr, "Boolean Value"_ustr);
  b.add_output<decl::Vector>("Value"_ustr, "Vector2 Value"_ustr).dimensions(2);
  b.add_output<decl::Vector>("Value"_ustr, "Vector3 Value"_ustr);
  b.add_output<decl::Vector>("Value"_ustr, "Vector4 Value"_ustr).dimensions(4);
  b.add_output<decl::Color>("Value"_ustr, "Color Value"_ustr);
  b.add_output<decl::Bool>("Is Valid"_ustr);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
    layout.prop(ptr, "light_object", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
  });
  auto &basic = b.add_panel("Light Information"_ustr, 0);
  basic.add_output<decl::Color>("Color"_ustr);
  basic.add_output<decl::Float>("Power"_ustr);
  basic.add_output<decl::Int>("Type"_ustr)
      .description("0 Point, 1 Sun, 2 Spot, 3 Area; -1 when no light is assigned");
  basic.add_output<decl::Vector>("Position"_ustr).description("World-space light position");
  basic.add_output<decl::Vector>("Direction"_ustr).description("World-space emission direction");
  basic.add_output<decl::Float>("Radius"_ustr);
  basic.add_output<decl::Float>("Spot Size"_ustr);
  basic.add_output<decl::Float>("Sun Angle"_ustr);
  basic.add_output<decl::Float>("Visible"_ustr);

  auto &parameters = b.add_panel("Shader Parameters"_ustr, 1);
  const bNode *node = b.node_or_null();
  if (!node || !node->storage) {
    return;
  }
  const auto &storage = node_storage(*node);
  const Light *light = selected_light(*node);
  Set<const NodeShaderLightInfoBinding *> declared;
  if (light && light == storage.source_light) {
    for (const LightShaderParameter &parameter : light->shader_parameters) {
      for (const auto &binding : storage.bindings) {
        if (binding.available && binding.parameter_type == parameter.type &&
            STREQ(binding.parameter_identifier, parameter.identifier))
        {
          add_parameter_output(parameters, binding, &parameter);
          declared.add(&binding);
          break;
        }
      }
    }
  }
  for (const auto &binding : storage.bindings) {
    if (!declared.contains(&binding)) {
      add_parameter_output(parameters, binding, nullptr);
    }
  }
  if (storage.has_legacy_layout) {
    if (node->custom1 == 1) {
      auto &legacy = b.add_panel("Legacy Parameter"_ustr, 2).default_closed(true);
      legacy.add_layout([](ui::Layout &layout, bContext *, PointerRNA *ptr) {
        layout.prop(ptr, "parameter_name", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
        layout.prop(ptr, "parameter_type", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
      });
      declare_legacy(legacy);
    }
    else {
      /* Retain unavailable legacy sockets without displaying an empty legacy panel. */
      declare_legacy(b);
    }
  }
}

static void node_init(bNodeTree *, bNode *node)
{
  node->storage = MEM_new<NodeShaderLightInfo>(__func__);
}

static void node_update(bNodeTree *tree, bNode *node)
{
  if (sync_bindings(*tree, *node)) {
    update_node_declaration_and_sockets(*tree, *node);
  }
  const auto &storage = node_storage(*node);
  for (bNodeSocket &socket : node->outputs) {
    if (StringRef(socket.identifier).startswith("Shader Parameter ") && socket.runtime->declaration) {
      /* RNA exposes a short description; the UI declaration retains the full tooltip. */
      STRNCPY_UTF8(socket.description, socket.runtime->declaration->description.c_str());
    }
    if (StringRef(socket.identifier).startswith("Shader Parameter ") ||
        ELEM(StringRef(socket.identifier), "Color", "Power", "Type", "Position", "Direction",
             "Radius", "Spot Size", "Sun Angle", "Visible"))
    {
      bke::node_set_socket_availability(*tree, socket, true);
    }
  }
  if (storage.has_legacy_layout) {
    int index = 0;
    for (bNodeSocket &socket : node->inputs) {
      bke::node_set_socket_availability(*tree, socket,
          node->custom1 == 1 && index == storage.parameter_type - 1);
      index++;
    }
    static const char *legacy_ids[] = {"Float Value", "Int Value", "Boolean Value", "Vector2 Value",
                                       "Vector3 Value", "Vector4 Value", "Color Value", "Is Valid"};
    for (bNodeSocket &socket : node->outputs) {
      for (int i = 0; i < 8; i++) {
        if (STREQ(socket.identifier, legacy_ids[i])) {
          bke::node_set_socket_availability(*tree, socket,
              node->custom1 == 1 && (i == 7 || i == storage.parameter_type - 1));
          break;
        }
      }
    }
  }
}

static const char *parameter_function(int type)
{
  switch (type) {
    case LIGHT_SHADER_PARAMETER_VECTOR2: return "node_light_parameter_vec2";
    case LIGHT_SHADER_PARAMETER_VECTOR3: return "node_light_parameter_vec3";
    case LIGHT_SHADER_PARAMETER_VECTOR4:
    case LIGHT_SHADER_PARAMETER_COLOR: return "node_light_parameter_vec4";
    default: return "node_light_parameter_float";
  }
}

static int node_gpu(GPUMaterial *mat, bNode *node, bNodeExecData *, GPUNodeStack *in, GPUNodeStack *out)
{
  Object *object = reinterpret_cast<Object *>(node->id);
  const auto flags = eGPUReferencedObjectDataFlag(
      GPU_REFERENCED_OBJECT_DATA_TRANSFORM | GPU_REFERENCED_OBJECT_DATA_COLOR |
      GPU_REFERENCED_OBJECT_DATA_VISIBILITY | GPU_REFERENCED_OBJECT_DATA_TYPE |
      GPU_REFERENCED_OBJECT_DATA_LIGHT);
  const float uid = uint_as_float(GPU_material_referenced_object_ensure(mat, object, flags));
  const char *basic[] = {"Color", "Power", "Type", "Position", "Direction", "Radius",
                         "Spot Size", "Sun Angle", "Visible"};
  GPUNodeLink **links[9];
  for (int i = 0; i < 9; i++) {
    links[i] = &GPU_node_get_output(*node, out, basic[i]).link;
  }
  bool ok = GPU_link(mat, "node_light_info", GPU_constant(&uid),
                    links[0], links[1], links[2], links[3], links[4],
                    links[5], links[6], links[7], links[8]);
  const auto read_parameter = [&](const char *name, int type, bool available,
                                  GPUNodeLink *fallback, GPUNodeLink **value, GPUNodeLink **valid) {
    const uint64_t key = object && available ?
        GPU_material_light_shader_parameter_ensure(mat, name, object) :
        GPU_light_shader_parameter_key(name);
    const float lo = uint_as_float(uint32_t(key));
    const float hi = uint_as_float(uint32_t(key >> 32));
    const float parameter_uid = available ? uid : uint_as_float(0u);
    const float parameter_type = float(type);
    return GPU_link(mat, parameter_function(type), fallback, GPU_constant(&parameter_uid),
                    GPU_constant(&lo), GPU_constant(&hi), GPU_constant(&parameter_type), value, valid);
  };
  const auto &storage = node_storage(*node);
  for (const auto &binding : storage.bindings) {
    auto &output = GPU_node_get_output(*node, out, socket_identifier(binding).c_str());
    auto &exists = GPU_node_get_output(*node, out, socket_identifier(binding, true).c_str());
    if (!output.hasoutput && !exists.hasoutput) {
      continue;
    }
    const float fallback[4] = {0, 0, 0, binding.parameter_type == LIGHT_SHADER_PARAMETER_COLOR ? 1.0f : 0.0f};
    ok &= read_parameter(binding.name, binding.parameter_type, binding.available != 0,
                         GPU_constant(fallback), &output.link, &exists.link);
  }
  if (storage.has_legacy_layout && node->custom1 == 1) {
    static const char *inputs[] = {"Float Default", "Int Default", "Boolean Default", "Vector2 Default",
                                   "Vector3 Default", "Vector4 Default", "Color Default"};
    static const char *outputs[] = {"Float Value", "Int Value", "Boolean Value", "Vector2 Value",
                                    "Vector3 Value", "Vector4 Value", "Color Value"};
    const int index = storage.parameter_type - 1;
    if (index >= 0 && index < 7) {
      ok &= read_parameter(storage.parameter_name, storage.parameter_type, true,
          GPU_node_get_input_link(*node, in, inputs[index]),
          &GPU_node_get_output(*node, out, outputs[index]).link,
          &GPU_node_get_output(*node, out, "Is Valid").link);
    }
  }
  return ok;
}

static void node_free(bNode *node)
{
  BLI_freelistN(&node_storage(*node).bindings);
  MEM_delete(static_cast<NodeShaderLightInfo *>(node->storage));
}

static void node_copy(bNodeTree *, bNode *dst, const bNode *src)
{
  dst->storage = MEM_new<NodeShaderLightInfo>(__func__, node_storage(*src));
  BLI_duplicatelist(&node_storage(*dst).bindings, &node_storage(*src).bindings);
}

static void node_write(const bNodeTree &, const bNode &node, BlendWriter &writer)
{
  writer.write_struct_list(&node_storage(node).bindings);
}

static void node_read(bNodeTree &, bNode &node, BlendDataReader &reader)
{
  BLO_read_struct_list(&reader, NodeShaderLightInfoBinding, &node_storage(node).bindings);
}

}  // namespace nodes::node_shader_light_info_cc

void register_node_type_sh_light_info()
{
  namespace file_ns = nodes::node_shader_light_info_cc;
  static bke::bNodeType ntype;
  sh_node_type_base(&ntype, "ShaderNodeLightInfo"_ustr, SH_NODE_LIGHT_INFO);
  ntype.ui_name = "Light Info";
  ntype.ui_description = "Read light information and dynamically named shader parameters";
  ntype.enum_name_legacy = "LIGHTINFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_gpu;
  ntype.gpu_uses_referenced_object_data = true;
  ntype.blend_write_storage_content = file_ns::node_write;
  ntype.blend_data_read_storage_content = file_ns::node_read;
  bke::node_type_storage(ntype, "NodeShaderLightInfo", file_ns::node_free, file_ns::node_copy);
  bke::node_register_type(ntype);
}

}  // namespace blender
