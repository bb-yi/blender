/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_scene_types.h"

#include "BKE_colorband.hh"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_node_runtime.hh"
#include "BKE_report.hh"

#include "NOD_node_extra_info.hh"
#include "NOD_principled_npr.hh"
#include "NOD_socket.hh"
#include "NOD_socket_items_ops.hh"

#include "RE_engine.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "BLI_string.h"

#include "GPU_material.hh"

#include "MEM_guardedalloc.h"

#include "node_shader_principled_npr_v2.hh"
#include "node_shader_util.hh"

#include <cmath>

namespace blender {

namespace nodes::node_shader_principled_npr_cc {

constexpr int PRINCIPLED_NPR_MAX_STOPS = 8;

enum InputSocket {
  SOCK_BASE_COLOR = 0,
  SOCK_METALLIC,
  SOCK_ROUGHNESS,
  SOCK_ALPHA,
  SOCK_NORMAL,
  SOCK_WEIGHT,

  SOCK_SHADOW_COLOR,
  SOCK_LIT_COLOR,
  SOCK_BOUNDARY,
  SOCK_SOFTNESS,
  SOCK_COORDINATE_SCALE,
  SOCK_COORDINATE_OFFSET,

  SOCK_SHADOW_STRENGTH,
  SOCK_DIRECT_STRENGTH,
  SOCK_INTENSITY_INFLUENCE,
  SOCK_LIGHT_COLOR_INFLUENCE,

  SOCK_HIGHLIGHT_COLOR,
  SOCK_HIGHLIGHT_STRENGTH,
  SOCK_HIGHLIGHT_ROUGHNESS,
  SOCK_HIGHLIGHT_SIZE,
  SOCK_HIGHLIGHT_SOFTNESS,
  SOCK_HIGHLIGHT_OFFSET,
  SOCK_HIGHLIGHT_LIGHT_COLOR_INFLUENCE,
  SOCK_ANISOTROPY,
  SOCK_ANISOTROPY_ROTATION,
  SOCK_TANGENT,

  SOCK_REFLECTION_STRENGTH,
  SOCK_AMBIENT_STRENGTH,
  SOCK_AMBIENT_DIRECTIONALITY,

  SOCK_RIM_COLOR,
  SOCK_RIM_STRENGTH,
  SOCK_RIM_ANGLE,
  SOCK_RIM_LENGTH,
  SOCK_RIM_LENGTH_FALLOFF,
  SOCK_RIM_THICKNESS,
  SOCK_RIM_THICKNESS_FALLOFF,
  SOCK_RIM_LIGHT_BIAS,
  SOCK_RIM_MASK,

  SOCK_EMISSION_COLOR,
  SOCK_EMISSION_STRENGTH,

  SOCK_DRIVEN_STOP_0,
};

static const NodeShaderPrincipledNPR &node_storage(const bNode &node)
{
  return *static_cast<const NodeShaderPrincipledNPR *>(node.storage);
}

static NodeShaderPrincipledNPR &node_storage(bNode &node)
{
  return *static_cast<NodeShaderPrincipledNPR *>(node.storage);
}

static int driven_stop_count(const bNode *node)
{
  if (node == nullptr || node->storage == nullptr) {
    return 2;
  }
  return clamp_i(node_storage(*node).driven_stop_count, 2, PRINCIPLED_NPR_MAX_STOPS);
}

static float driven_stop_default_position(const int index, const int stop_count)
{
  return (stop_count <= 1) ? 0.0f : float(index) / float(stop_count - 1);
}

static bool driven_stop_positions_match_default(const bNode &node, const int stop_count)
{
  for (int i = 0; i < PRINCIPLED_NPR_MAX_STOPS; i++) {
    char position_id[32];
    SNPRINTF(position_id, "stop_position_%d", i);
    const bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, UString(position_id));
    if (socket == nullptr || socket->link != nullptr) {
      return false;
    }
  }

  for (int i = 0; i < stop_count; i++) {
    char position_id[32];
    SNPRINTF(position_id, "stop_position_%d", i);
    const bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, UString(position_id));
    if (socket == nullptr) {
      return false;
    }
    const float value = socket->default_value_typed<bNodeSocketValueFloat>()->value;
    if (std::abs(value - driven_stop_default_position(i, stop_count)) > 1e-5f) {
      return false;
    }
  }
  return true;
}

static void update_driven_stop_default_positions(bNode &node)
{
  NodeShaderPrincipledNPR &storage = node_storage(node);
  if (storage.model_version >= 2 ||
      storage.diffuse_mapping != SHD_PRINCIPLED_NPR_DIFFUSE_DRIVEN_RAMP)
  {
    return;
  }

  const int stop_count = driven_stop_count(&node);
  int previous_stop_count = 0;
  for (int candidate = 2; candidate <= PRINCIPLED_NPR_MAX_STOPS; candidate++) {
    if (driven_stop_positions_match_default(node, candidate)) {
      previous_stop_count = candidate;
      break;
    }
  }
  if (previous_stop_count == 0 || previous_stop_count == stop_count) {
    return;
  }

  for (int i = 0; i < stop_count; i++) {
    char position_id[32];
    SNPRINTF(position_id, "stop_position_%d", i);
    if (bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, UString(position_id))) {
      socket->default_value_typed<bNodeSocketValueFloat>()->value =
          driven_stop_default_position(i, stop_count);
    }
  }
}

static GPUNodeLink *npr_input_link(GPUNodeStack &socket)
{
  if (socket.link != nullptr) {
    return socket.link;
  }
  if (socket.type == GPU_FLOAT) {
    return GPU_uniform(&socket.vec[0]);
  }
  return GPU_uniform(socket.vec);
}

static GPUNodeLink *npr_socket(const bNode &node,
                               GPUNodeStack *in,
                               const StringRef identifier)
{
  return npr_input_link(GPU_node_get_input(node, in, identifier));
}

static float ramp_preview_input(const bNode &node, const UString identifier, const float fallback)
{
  const bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, identifier);
  return socket ? socket->default_value_typed<bNodeSocketValueFloat>()->value : fallback;
}

static std::shared_ptr<ui::CustomColorRampData> ramp_ui_data(const PointerRNA node_ptr)
{
  auto data = std::make_shared<ui::CustomColorRampData>();
  data->refresh = [node_ptr](ColorBand &display) {
    const NodeShaderPrincipledNPR &storage = node_storage(*node_ptr.data_as<bNode>());
    const NodePrincipledNPRRampPoint *points[SHD_PRINCIPLED_NPR_MAX_RAMP_POINTS];
    const int count = principled_npr::sorted_ramp_points(storage, points);
    display.tot = count;
    display.cur = 0;
    for (int i = 0; i < count; i++) {
      CBData &handle = display.data[i];
      handle.pos = std::clamp(points[i]->position, 0.0f, 1.0f);
      handle.r = points[i]->color[0];
      handle.g = points[i]->color[1];
      handle.b = points[i]->color[2];
      handle.a = points[i]->color[3];
      handle.cur = points[i]->identifier;
      if (handle.cur == storage.active_point_identifier) {
        display.cur = i;
      }
    }
  };
  data->evaluate = [node_ptr](const float coordinate, float color[4]) {
    const bNode &node = *node_ptr.data_as<bNode>();
    principled_npr::evaluate_ramp(node_storage(node),
                                  coordinate,
                                  ramp_preview_input(node, "coordinate_scale"_ustr, 1.0f),
                                  ramp_preview_input(node, "coordinate_offset"_ustr, 0.0f),
                                  ramp_preview_input(node, "mapping_softness"_ustr, 0.1f),
                                  color);
  };
  data->edit = [node_ptr](const ui::CustomColorRampAction action,
                          const int identifier,
                          const float position) {
    bNode &node = *node_ptr.data_as<bNode>();
    NodeShaderPrincipledNPR &storage = node_storage(node);
    if (action == ui::CustomColorRampAction::Add) {
      float color[4];
      principled_npr::evaluate_ramp(storage,
                                    position,
                                    1.0f,
                                    0.0f,
                                    ramp_preview_input(node, "mapping_softness"_ustr, 0.1f),
                                    color);
      if (NodePrincipledNPRRampPoint *point = principled_npr::add_point(storage, position)) {
        std::copy_n(color, 4, point->color);
        return true;
      }
      return false;
    }
    if (action == ui::CustomColorRampAction::Remove) {
      return principled_npr::remove_point(storage, identifier);
    }
    NodePrincipledNPRRampPoint *point = principled_npr::find_point(storage, identifier);
    if (point == nullptr || !point->enabled) {
      return false;
    }
    if (action == ui::CustomColorRampAction::Select) {
      storage.active_point_identifier = identifier;
      return true;
    }
    const float clamped = std::clamp(position, 0.0f, 1.0f);
    const bool changed = point->position != clamped;
    point->position = clamped;
    return changed;
  };
  data->update = [node_ptr](bContext &C) {
    PointerRNA ptr = node_ptr;
    RNA_property_update(&C, &ptr, RNA_struct_find_property(&ptr, "mapping_interpolation"));
  };
  return data;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *ntree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  const bool is_gpu_internal = ntree && (ntree->flag & NTREE_IS_GPU_SHADER_INTERNAL);
  const NodeShaderPrincipledNPR *storage = node && node->storage ?
                                                &node_storage(*node) :
                                                nullptr;
  const int diffuse_mapping = storage ? storage->diffuse_mapping :
                                        SHD_PRINCIPLED_NPR_DIFFUSE_SIMPLE;
  const int stop_count = driven_stop_count(node);
  const bool v2 = storage == nullptr || storage->model_version >= 2;
  const bool energy_response = v2 &&
                               (storage == nullptr || storage->energy_response_version >= 1);
  const bool depth_rim = v2 && storage &&
                         storage->rim_mode == SHD_PRINCIPLED_NPR_RIM_SCREEN_DEPTH;
  PanelDeclarationBuilder *stop_panels[PRINCIPLED_NPR_MAX_STOPS] = {};

  b.use_custom_socket_order();
  b.add_output<decl::Shader>("Shader"_ustr, "shader"_ustr);
  b.add_output<decl::Color>(v2 ? "Local Color"_ustr : "Color"_ustr, "color"_ustr)
      .description(
          v2 ? "Local NPR base lighting, rim and emission; excludes native coat/sheen/transmission "
               "lighting and later SSS, SSR or refraction" :
               "Final scene-linear HDR NPR color before it is wrapped into a Shader");
  b.add_output<decl::Float>("Alpha"_ustr, "alpha"_ustr);

  b.add_input<decl::Color>("Base Color"_ustr, "base_color"_ustr)
      .default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_input<decl::Float>("Metallic"_ustr, "metallic"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Blend dielectric diffuse into tinted metallic reflection and suppress diffuse");
  b.add_input<decl::Float>("Roughness"_ustr, "roughness"_ustr)
      .default_value(0.4f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Control the base direct highlight width and peak, and probe reflection blur");
  b.add_input<decl::Float>("Alpha"_ustr, "alpha"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Vector>("Normal"_ustr, "normal"_ustr).hide_value();
  b.add_input<decl::Float>("Weight"_ustr).available(is_gpu_internal);

  PanelDeclarationBuilder &shading =
      b.add_panel(v2 ? "Color Mapping"_ustr : "Shading"_ustr).default_closed(false);
  shading.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
    const bool is_v2 = RNA_int_get(ptr, "model_version") >= 2;
    layout.prop(ptr,
                "diffuse_mapping",
                ui::ITEM_R_SPLIT_EMPTY_NAME,
                is_v2 ? IFACE_("Mode") : IFACE_("Diffuse Mapping"),
                ICON_NONE);
    if (!is_v2) {
      layout.prop(ptr, "coordinate_range", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      layout.prop(ptr, "color_application", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      layout.label(IFACE_("Legacy V1"), ICON_INFO);
      PointerRNA op = layout.op(
          "node.principled_npr_upgrade_copy", IFACE_("Create V2 Copy"), ICON_DUPLICATE);
      RNA_int_set(&op, "node_identifier", ptr->data_as<bNode>()->identifier);
    }
    const int mapping = RNA_enum_get(ptr, "diffuse_mapping");
    if (mapping == SHD_PRINCIPLED_NPR_DIFFUSE_RAMP) {
      if (!is_v2) {
        template_color_ramp(&layout, ptr, "diffuse_ramp", true);
      }
      else {
        layout.prop(ptr, "ramp_point_count", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
        ui::Layout &buttons = layout.row(true);
        PointerRNA add = buttons.op("node.principled_npr_point_add", "", ICON_ADD);
        RNA_int_set(&add, "node_identifier", ptr->data_as<bNode>()->identifier);
        PointerRNA remove = buttons.op("node.principled_npr_point_remove", "", ICON_REMOVE);
        RNA_int_set(&remove, "node_identifier", ptr->data_as<bNode>()->identifier);
        buttons.prop(
            ptr, "ramp_active_index", ui::ITEM_R_SPLIT_EMPTY_NAME, IFACE_("Point"), ICON_NONE);
        template_custom_color_ramp(&layout, ptr, "ramp_points", ramp_ui_data(*ptr));
        const bNode &node = *ptr->data_as<bNode>();
        for (const UString input :
             {"coordinate_scale"_ustr, "coordinate_offset"_ustr, "mapping_softness"_ustr})
        {
          const bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, input);
          if (socket && socket->link) {
            layout.label(IFACE_("Preview uses socket defaults"), ICON_INFO);
            break;
          }
        }
        PointerRNA point;
        if (RNA_property_collection_lookup_int(ptr,
                                               RNA_struct_find_property(ptr, "ramp_points"),
                                               RNA_int_get(ptr, "ramp_active_index"),
                                               &point))
        {
          layout.prop(&point, "position", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
          layout.prop(&point, "color", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
        }
      }
    }
    else if (mapping == SHD_PRINCIPLED_NPR_DIFFUSE_DRIVEN_RAMP) {
      layout.prop(ptr, "driven_stop_count", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      if (!is_v2) {
        layout.prop(
            ptr, "driven_interpolation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      }
    }
  });
  shading.add_input<decl::Color>("Shadow Color"_ustr, "shadow_color"_ustr)
      .default_value({0.45f, 0.45f, 0.45f, 1.0f})
      .description("Multiplier or replacement color on the shadow side")
      .available(diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_SIMPLE);
  shading.add_input<decl::Color>("Lit Color"_ustr, "lit_color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .description("Multiplier or replacement color on the lit side")
      .available(diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_SIMPLE);
  shading.add_input<decl::Float>("Boundary"_ustr, "boundary"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Lighting coordinate at the center of the simple color transition")
      .available(!v2 && diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_SIMPLE);
  shading.add_input<decl::Float>("Softness"_ustr, "softness"_ustr)
      .default_value(0.05f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Width of the simple color transition")
      .available(!v2 && diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_SIMPLE);
  PanelDeclarationBuilder &mapping_settings =
      v2 ? shading.add_panel("Mapping Settings"_ustr).default_closed(true) : shading;
  if (v2) {
    mapping_settings.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
      layout.prop(ptr, "coordinate_range", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      layout.prop(ptr, "color_application", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      layout.prop(
          ptr, "mapping_interpolation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      if (RNA_enum_get(ptr, "diffuse_mapping") == SHD_PRINCIPLED_NPR_DIFFUSE_RAMP) {
        PointerRNA point;
        if (RNA_property_collection_lookup_int(ptr,
                                               RNA_struct_find_property(ptr, "ramp_points"),
                                               RNA_int_get(ptr, "ramp_active_index"),
                                               &point))
        {
          /* Node layouts do not own a regular Panel. Use this declaration's existing
           * collapsed panel instead of Layout::panel(), which requires Panel state. */
          layout.separator();
          layout.label(IFACE_("Selected Point Transition"), ICON_NONE);
          layout.prop(&point, "offset", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
          layout.prop(&point, "softness", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
        }
      }
    });
  }
  mapping_settings.add_input<decl::Float>("Coordinate Scale"_ustr, "coordinate_scale"_ustr)
      .default_value(1.0f)
      .min(-1000.0f)
      .max(1000.0f)
      .description("Scale the unclamped lighting coordinate; negative values invert it");
  shading
      .add_input<decl::Float>(v2 ? "Overall Offset"_ustr : "Coordinate Offset"_ustr,
                              "coordinate_offset"_ustr)
      .default_value(0.0f)
      .min(-1000.0f)
      .max(1000.0f)
      .description("Offset the unclamped lighting coordinate before its final 0..1 mapping clamp");

  PanelDeclarationBuilder &lighting =
      b.add_panel(v2 ? "Lights and Shadows"_ustr : "Lighting"_ustr).default_closed(true);
  lighting.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
    layout.prop(ptr, "mapping_stage", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    layout.prop(ptr, "light_combine", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    layout.prop(ptr, "shadow_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    const bool is_v2 = RNA_int_get(ptr, "model_version") >= 2;
    if (is_v2) {
      if (RNA_int_get(ptr, "energy_response_version") == 0) {
        layout.label(IFACE_("Legacy Energy Response"), ICON_INFO);
        PointerRNA op = layout.op("node.principled_npr_upgrade_copy",
                                  IFACE_("Create Updated Copy"), ICON_DUPLICATE);
        RNA_int_set(&op, "node_identifier", ptr->data_as<bNode>()->identifier);
      }
      else if (RNA_enum_get(ptr, "mapping_stage") == SHD_PRINCIPLED_NPR_MAPPING_COMBINED) {
        layout.label(IFACE_("Shared energy, per-light shadows"), ICON_INFO);
      }
      layout.prop(ptr, "shadow_quality", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      if (RNA_enum_get(ptr, "shadow_quality") == SHD_SHADER_INFO_SHADOW_SOFT_FILTERED) {
        layout.prop(ptr, "shadow_samples", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      }
      layout.prop(ptr, "use_all_lights", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    }
    if (!is_v2 || !RNA_boolean_get(ptr, "use_all_lights")) {
      layout.prop(ptr, "lightgroup_id", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    }
  });
  lighting.add_input<decl::Float>("Shadow Strength"_ustr, "shadow_strength"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  lighting.add_input<decl::Float>("Direct Strength"_ustr, "direct_strength"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(8.0f);
  lighting.add_input<decl::Float>("Intensity Influence"_ustr, "intensity_influence"_ustr)
      .default_value(v2 ? 0.0f : 1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("How strongly light energy pushes the diffuse mapping boundary")
      .available(!energy_response);
  lighting.add_input<decl::Float>("Light Color Influence"_ustr, "light_color_influence"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);

  PanelDeclarationBuilder &specular =
      b.add_panel(v2 ? "Highlights"_ustr : "Specular"_ustr).default_closed(true);
  specular.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
    if (RNA_int_get(ptr, "model_version") >= 2) {
      layout.prop(ptr, "highlight_light_shape", ui::ITEM_R_SPLIT_EMPTY_NAME,
                  std::nullopt, ICON_NONE);
      if (RNA_enum_get(ptr, "highlight_light_shape") == SHD_PRINCIPLED_NPR_HIGHLIGHT_INTEGRATED) {
        layout.label(IFACE_("Finite integration: higher cost"), ICON_INFO);
      }
      return;
    }
    layout.prop(ptr, "specular_mapping", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    if (RNA_enum_get(ptr, "specular_mapping") == SHD_PRINCIPLED_NPR_SPECULAR_RAMP) {
      template_color_ramp(&layout, ptr, "specular_ramp", true);
    }
  });
  specular.add_input<decl::Color>("Highlight Color"_ustr, "highlight_color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f});
  specular.add_input<decl::Float>("Highlight Strength"_ustr, "highlight_strength"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(8.0f)
      .description("Enable and scale the NPR highlight lobe");
  specular.add_input<decl::Float>("Highlight Roughness"_ustr, "highlight_roughness"_ustr)
      .default_value(0.4f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .available(false)
      .description("Legacy socket retained for file compatibility; use Roughness");
  specular.add_input<decl::Float>("Highlight Size"_ustr, "highlight_size"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("NPR highlight coverage from a point to the full lobe")
      .available(!v2);
  specular.add_input<decl::Float>("Highlight Softness"_ustr, "highlight_softness"_ustr)
      .default_value(0.08f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("NPR highlight edge transition width")
      .available(!v2);
  specular.add_input<decl::Float>("Highlight Offset"_ustr, "highlight_offset"_ustr)
      .default_value(0.0f)
      .min(-1000.0f)
      .max(1000.0f)
      .description("Shift the highlight coordinate before size and softness mapping")
      .available(!v2);
  specular
      .add_input<decl::Float>("Light Color Influence"_ustr, "highlight_light_color_influence"_ustr)
      .default_value(v2 ? 1.0f : 0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  PanelDeclarationBuilder &anisotropy =
      v2 ? specular.add_panel("Anisotropy"_ustr).default_closed(true) : specular;
  anisotropy.add_input<decl::Float>("Anisotropy"_ustr, "anisotropy"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  anisotropy.add_input<decl::Float>("Anisotropy Rotation"_ustr, "anisotropy_rotation"_ustr)
      .default_value(0.0f)
      .min(-1.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  anisotropy.add_input<decl::Vector>("Tangent"_ustr, "tangent"_ustr).hide_value();

  PanelDeclarationBuilder &environment = b.add_panel("Environment"_ustr).default_closed(true);
  environment
      .add_input<decl::Float>(v2 ? "Indirect Specular"_ustr : "Reflection Strength"_ustr,
                              "reflection_strength"_ustr)
      .default_value(v2 ? 1.0f : 0.0f)
      .min(0.0f)
      .max(8.0f)
      .description(
          v2 ? "Scale indirect reflection from the engine's tracing and probe paths" :
               "Reflection probes only; V1 does not add a screen-space reflection closure");
  environment
      .add_input<decl::Float>(v2 ? "Indirect Diffuse"_ustr : "Ambient Strength"_ustr,
                              "ambient_strength"_ustr)
      .default_value(v2 ? 1.0f : 0.0f)
      .min(0.0f)
      .max(8.0f);
  environment.add_input<decl::Float>("Ambient Directionality"_ustr, "ambient_directionality"_ustr)
      .default_value(0.1f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("0 uses equalized ambient light; 1 follows the surface normal")
      .available(!v2);

  PanelDeclarationBuilder &rim = b.add_panel("Rim"_ustr).default_closed(true);
  if (v2) {
    rim.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
      layout.prop(ptr, "rim_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      if (RNA_enum_get(ptr, "rim_mode") == SHD_PRINCIPLED_NPR_RIM_SCREEN_DEPTH) {
        layout.label(IFACE_("Opaque / binary cutout only"), ICON_INFO);
      }
    });
  }
  rim.add_input<decl::Color>("Rim Color"_ustr, "rim_color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f});
  rim.add_input<decl::Float>("Rim Strength"_ustr, "rim_strength"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(8.0f);
  rim.add_input<decl::Float>("Angle"_ustr, "rim_angle"_ustr)
      .default_value(0.0f)
      .min(-360.0f)
      .max(360.0f);
  rim.add_input<decl::Float>("Length"_ustr, "rim_length"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  rim.add_input<decl::Float>("Length Falloff"_ustr, "rim_length_falloff"_ustr)
      .default_value(0.1f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  rim.add_input<decl::Float>("Thickness"_ustr, "rim_thickness"_ustr)
      .default_value(0.1f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .available(!depth_rim);
  rim.add_input<decl::Float>("Thickness Falloff"_ustr, "rim_thickness_falloff"_ustr)
      .default_value(0.05f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .available(!depth_rim);
  rim.add_input<decl::Float>("Light Bias"_ustr, "rim_light_bias"_ustr)
      .default_value(0.0f)
      .min(-1.0f)
      .max(1.0f);
  rim.add_input<decl::Float>("Mask"_ustr, "rim_mask"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);

  PanelDeclarationBuilder &emission = b.add_panel("Emission"_ustr).default_closed(true);
  emission.add_input<decl::Color>("Emission Color"_ustr, "emission_color"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 1.0f});
  emission.add_input<decl::Float>("Emission Strength"_ustr, "emission_strength"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1000000.0f);

  for (int i = 0; i < PRINCIPLED_NPR_MAX_STOPS; i++) {
    char color_id[32];
    char position_id[32];
    char color_name[32];
    char position_name[32];
    SNPRINTF(color_id, "stop_color_%d", i);
    SNPRINTF(position_id, "stop_position_%d", i);
    SNPRINTF(color_name, "Stop %d Color", i + 1);
    SNPRINTF(position_name, "Stop %d Position", i + 1);
    const bool available = diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_DRIVEN_RAMP &&
                           i < stop_count;
    static constexpr float initial_v2_positions[8] = {
        0.0f, 1.0f, 0.5f, 0.25f, 0.75f, 0.125f, 0.375f, 0.625f};
    const float position = v2 ? initial_v2_positions[i] :
                                driven_stop_default_position(i, stop_count);
    const float value = (i == 0) ? 0.45f : 1.0f;
    char panel_name[32];
    SNPRINTF(panel_name, "Control Point %d", i + 1);
    PanelDeclarationBuilder &point_panel =
        v2 ? shading.add_panel(UString(panel_name)).default_closed(i > 1) : shading;
    stop_panels[i] = &point_panel;
    point_panel.add_input<decl::Color>(UString(color_name), UString(color_id))
        .default_value({value, value, value, 1.0f})
        .available(available);
    point_panel.add_input<decl::Float>(UString(position_name), UString(position_id))
        .default_value(position)
        .min(0.0f)
        .max(1.0f)
        .subtype(PROP_FACTOR)
        .available(available);
  }

  if (!v2) {
    return;
  }
  /* Append to the flat input array after the 56 legacy inputs, regardless of panel placement. */
  shading.add_input<decl::Float>("Global Softness"_ustr, "mapping_softness"_ustr)
      .default_value(0.1f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  specular.add_input<decl::Float>("Softness"_ustr, "profile_softness"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("0 creates a flat hard highlight; 1 restores the original GGX response");
  specular.add_input<decl::Float>("Range Offset"_ustr, "profile_offset"_ustr)
      .default_value(0.0f)
      .min(-1.0f)
      .max(1.0f)
      .description("Expand or shrink both highlight axes without changing reference brightness");
  PanelDeclarationBuilder &material = b.add_panel("Material Details"_ustr).default_closed(true);
  material.add_input<decl::Float>("IOR"_ustr, "ior"_ustr)
      .default_value(1.5f)
      .min(1.0f)
      .max(1000.0f);
  material
      .add_input<decl::Float>("Metallic Body Preservation"_ustr, "metallic_body_preservation"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Artistic recovery of mapped diffuse color on metals; does not restore SSS or "
          "transmission");
  PanelDeclarationBuilder &transmission = b.add_panel("Transmission"_ustr).default_closed(true);
  transmission.add_input<decl::Float>("Transmission Weight"_ustr, "transmission_weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Weight");
  PanelDeclarationBuilder &subsurface = b.add_panel("Subsurface"_ustr).default_closed(true);
  subsurface.add_input<decl::Float>("Subsurface Weight"_ustr, "subsurface_weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Weight");
  subsurface.add_input<decl::Vector>("Subsurface Radius"_ustr, "subsurface_radius"_ustr)
      .default_value({1.0f, 0.2f, 0.1f})
      .min(0.0f)
      .max(100.0f)
      .short_label("Radius");
  subsurface.add_input<decl::Float>("Subsurface Scale"_ustr, "subsurface_scale"_ustr)
      .default_value(0.005f)
      .min(0.0f)
      .max(10.0f)
      .subtype(PROP_DISTANCE)
      .short_label("Scale");
  PanelDeclarationBuilder &coat = b.add_panel("Coat"_ustr).default_closed(true);
  coat.add_input<decl::Float>("Coat Weight"_ustr, "coat_weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Weight");
  coat.add_input<decl::Float>("Coat Roughness"_ustr, "coat_roughness"_ustr)
      .default_value(0.03f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Roughness");
  coat.add_input<decl::Float>("Coat IOR"_ustr, "coat_ior"_ustr)
      .default_value(1.5f)
      .min(1.0f)
      .max(1000.0f)
      .short_label("IOR");
  coat.add_input<decl::Color>("Coat Tint"_ustr, "coat_tint"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .short_label("Tint");
  coat.add_input<decl::Vector>("Coat Normal"_ustr, "coat_normal"_ustr)
      .hide_value()
      .short_label("Normal");
  PanelDeclarationBuilder &sheen = b.add_panel("Sheen"_ustr).default_closed(true);
  sheen.add_input<decl::Float>("Sheen Weight"_ustr, "sheen_weight"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Weight");
  sheen.add_input<decl::Float>("Sheen Roughness"_ustr, "sheen_roughness"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .short_label("Roughness");
  sheen.add_input<decl::Color>("Sheen Tint"_ustr, "sheen_tint"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .short_label("Tint");
  material.add_input<decl::Float>("Thin Wall"_ustr, "thin_wall"_ustr)
      .default_value(0.0f)
      .min(0.0f)
      .max(1.0f)
      .description(
          "Treat the surface as a thin sheet; thickness remains a Material Output setting");
  for (int i = 0; i < PRINCIPLED_NPR_MAX_STOPS; i++) {
    char offset_id[32], softness_id[32];
    SNPRINTF(offset_id, "stop_offset_%d", i);
    SNPRINTF(softness_id, "stop_softness_%d", i);
    const bool available = diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_DRIVEN_RAMP &&
                           i < stop_count;
    stop_panels[i]
        ->add_input<decl::Float>("Boundary Offset"_ustr, UString(offset_id))
        .default_value(0.0f)
        .min(-0.5f)
        .max(0.5f)
        .available(available);
    stop_panels[i]
        ->add_input<decl::Float>("Softness Multiplier"_ustr, UString(softness_id))
        .default_value(1.0f)
        .min(0.0f)
        .max(10.0f)
        .available(available);
  }
  /* Do not reinterpret the old intensity socket: its index, links and animation stay intact. */
  lighting.add_input<decl::Float>("Energy Influence"_ustr, "energy_influence"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .available(energy_response)
      .description(
          "How strongly received diffuse light energy moves the color bands: 0 keeps their "
          "shape fixed, 1 follows energy fully. Coordinate Scale calibrates the fixed reference; "
          "Base Color and camera exposure do not drive the bands");
  rim.add_input<decl::Float>("Width (Pixels)"_ustr, "rim_pixel_width"_ustr)
      .default_value(4.0f)
      .min(0.0f)
      .max(128.0f)
      .description("Rim width in actual render pixels, independent of camera distance")
      .available(depth_rim);
  rim.add_input<decl::Float>("Depth Threshold"_ustr, "rim_depth_threshold"_ustr)
      .default_value(0.01f)
      .min(0.0f)
      .max(1000.0f)
      .subtype(PROP_DISTANCE)
      .description("Minimum linear depth discontinuity after geometric plane compensation")
      .available(depth_rim);
  rim.add_input<decl::Float>("Softness"_ustr, "rim_depth_softness"_ustr)
      .default_value(0.1f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Transition as a fraction of pixel width; hard edges retain coverage AA")
      .available(depth_rim);
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderPrincipledNPR *storage = MEM_new<NodeShaderPrincipledNPR>(__func__);
  storage->model_version = 2;
  storage->energy_response_version = 1;
  storage->highlight_light_shape = SHD_PRINCIPLED_NPR_HIGHLIGHT_DIRECTION;
  storage->coordinate_range = SHD_PRINCIPLED_NPR_RANGE_FULL;
  storage->light_combine = SHD_PRINCIPLED_NPR_LIGHT_ADD;
  storage->driven_initialized_count = PRINCIPLED_NPR_MAX_STOPS;
  principled_npr::initialize_ramp(*storage);
  BKE_colorband_init(&storage->diffuse_ramp, true);
  storage->diffuse_ramp.data[0].pos = 0.45f;
  storage->diffuse_ramp.data[0].r = 0.45f;
  storage->diffuse_ramp.data[0].g = 0.45f;
  storage->diffuse_ramp.data[0].b = 0.45f;
  storage->diffuse_ramp.data[0].a = 1.0f;
  storage->diffuse_ramp.data[1].pos = 0.55f;
  storage->diffuse_ramp.data[1].r = 1.0f;
  storage->diffuse_ramp.data[1].g = 1.0f;
  storage->diffuse_ramp.data[1].b = 1.0f;
  storage->diffuse_ramp.data[1].a = 1.0f;

  BKE_colorband_init(&storage->specular_ramp, true);
  storage->specular_ramp.data[0].pos = 0.0f;
  storage->specular_ramp.data[0].r = 0.0f;
  storage->specular_ramp.data[0].g = 0.0f;
  storage->specular_ramp.data[0].b = 0.0f;
  storage->specular_ramp.data[0].a = 0.0f;
  storage->specular_ramp.data[1].pos = 1.0f;
  storage->specular_ramp.data[1].r = 1.0f;
  storage->specular_ramp.data[1].g = 1.0f;
  storage->specular_ramp.data[1].b = 1.0f;
  storage->specular_ramp.data[1].a = 1.0f;
  node->storage = storage;

  if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, "highlight_strength"_ustr)) {
    socket->default_value_typed<bNodeSocketValueFloat>()->value = 1.0f;
  }
  if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, "highlight_roughness"_ustr)) {
    socket->default_value_typed<bNodeSocketValueFloat>()->value = 0.4f;
  }
  if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, "highlight_size"_ustr)) {
    socket->default_value_typed<bNodeSocketValueFloat>()->value = 0.5f;
  }
  if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, "highlight_softness"_ustr)) {
    socket->default_value_typed<bNodeSocketValueFloat>()->value = 0.08f;
  }
  if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, "intensity_influence"_ustr)) {
    socket->default_value_typed<bNodeSocketValueFloat>()->value = 0.0f;
  }
  for (const UString identifier : {"reflection_strength"_ustr, "ambient_strength"_ustr}) {
    if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, identifier)) {
      socket->default_value_typed<bNodeSocketValueFloat>()->value = 1.0f;
    }
  }
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader & /*reader*/)
{
  if (node.storage != nullptr && node_storage(node).model_version == 0) {
    node_storage(node).model_version = 1;
  }
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  if (node->storage == nullptr) {
    return;
  }
  const NodeShaderPrincipledNPR &storage = node_storage(*node);
  const bool v2 = storage.model_version >= 2;
  const bool energy_response = v2 && storage.energy_response_version >= 1;
  const bool depth_rim = v2 && storage.rim_mode == SHD_PRINCIPLED_NPR_RIM_SCREEN_DEPTH;
  for (const UString id : {"rim_pixel_width"_ustr, "rim_depth_threshold"_ustr,
                           "rim_depth_softness"_ustr, "rim_thickness"_ustr,
                           "rim_thickness_falloff"_ustr})
  {
    if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, id)) {
      const bool fresnel_input = ELEM(id, "rim_thickness"_ustr, "rim_thickness_falloff"_ustr);
      bke::node_set_socket_availability(*ntree, *socket, fresnel_input ? !depth_rim : depth_rim);
    }
  }
  if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, "intensity_influence"_ustr)) {
    bke::node_set_socket_availability(*ntree, *socket, !energy_response);
  }
  if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, "energy_influence"_ustr)) {
    bke::node_set_socket_availability(*ntree, *socket, energy_response);
  }
  const bool simple = storage.diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_SIMPLE;
  const bool driven = storage.diffuse_mapping == SHD_PRINCIPLED_NPR_DIFFUSE_DRIVEN_RAMP;
  for (const UString identifier :
       {"shadow_color"_ustr, "lit_color"_ustr, "boundary"_ustr, "softness"_ustr})
  {
    if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, identifier)) {
      const bool legacy_control = ELEM(identifier, "boundary"_ustr, "softness"_ustr);
      bke::node_set_socket_availability(*ntree, *socket, simple && !(v2 && legacy_control));
    }
  }

  for (const UString identifier : {"highlight_size"_ustr, "highlight_softness"_ustr}) {
    if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, identifier)) {
      bke::node_set_socket_availability(*ntree, *socket, !v2);
    }
  }

  const int stop_count = driven_stop_count(node);
  for (int i = 0; i < PRINCIPLED_NPR_MAX_STOPS; i++) {
    char color_id[32];
    char position_id[32];
    SNPRINTF(color_id, "stop_color_%d", i);
    SNPRINTF(position_id, "stop_position_%d", i);
    const bool available = driven && i < stop_count;
    if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, UString(color_id))) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
    if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, UString(position_id))) {
      bke::node_set_socket_availability(*ntree, *socket, available);
    }
    if (v2) {
      for (const char *prefix : {"stop_offset_", "stop_softness_"}) {
        char identifier[32];
        SNPRINTF(identifier, "%s%d", prefix, i);
        if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, UString(identifier))) {
          bke::node_set_socket_availability(*ntree, *socket, available);
        }
      }
    }
  }
  update_driven_stop_default_positions(*node);
}

struct PrincipledNPRActions {
  static constexpr StringRefNull node_idname = "ShaderNodePrincipledNPR";
};

static wmOperatorStatus point_add_exec(bContext *C, wmOperator *op)
{
  PointerRNA ptr = socket_items::ops::get_active_node_to_operate_on(
      C, op, PrincipledNPRActions::node_idname);
  if (!ptr.data) {
    return OPERATOR_CANCELLED;
  }
  NodeShaderPrincipledNPR &storage = node_storage(*ptr.data_as<bNode>());
  if (storage.model_version < 2) {
    return OPERATOR_CANCELLED;
  }
  const float position = principled_npr::largest_gap_midpoint(storage);
  float color[4];
  principled_npr::evaluate_ramp(
      storage,
      position,
      1.0f,
      0.0f,
      ramp_preview_input(*ptr.data_as<bNode>(), "mapping_softness"_ustr, 0.1f),
      color);
  NodePrincipledNPRRampPoint *point = principled_npr::add_point(storage, position);
  if (point == nullptr) {
    BKE_report(op->reports,
               RPT_WARNING,
               "No free NPR point slots (maximum 32, including suspended points)");
    return OPERATOR_CANCELLED;
  }
  std::copy_n(color, 4, point->color);
  socket_items::ops::update_after_node_change(C, ptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus point_remove_exec(bContext *C, wmOperator *op)
{
  PointerRNA ptr = socket_items::ops::get_active_node_to_operate_on(
      C, op, PrincipledNPRActions::node_idname);
  if (!ptr.data) {
    return OPERATOR_CANCELLED;
  }
  NodeShaderPrincipledNPR &storage = node_storage(*ptr.data_as<bNode>());
  if (storage.model_version < 2 ||
      !principled_npr::remove_point(storage, storage.active_point_identifier))
  {
    BKE_report(op->reports, RPT_WARNING, "At least two NPR mapping points must remain enabled");
    return OPERATOR_CANCELLED;
  }
  socket_items::ops::update_after_node_change(C, ptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus upgrade_copy_exec(bContext *C, wmOperator *op)
{
  PointerRNA ptr = socket_items::ops::get_active_node_to_operate_on(
      C, op, PrincipledNPRActions::node_idname);
  if (!ptr.data) {
    return OPERATOR_CANCELLED;
  }
  bNode &source = *ptr.data_as<bNode>();
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr.owner_id);
  const bool legacy_v1 = node_storage(source).model_version < 2;
  if (!legacy_v1 && node_storage(source).energy_response_version >= 1) {
    BKE_report(op->reports, RPT_INFO, "This node already uses the current NPR V2 model");
    return OPERATOR_CANCELLED;
  }

  Map<const bNodeSocket *, bNodeSocket *> socket_map;
  bNode *copy = bke::node_copy_with_mapping(
      &tree, source, LIB_ID_COPY_DEFAULT, std::nullopt, std::nullopt, socket_map);
  NodeShaderPrincipledNPR &storage = node_storage(*copy);
  storage.model_version = 2;
  storage.energy_response_version = 1;
  if (legacy_v1) {
    storage.shadow_quality = SHD_SHADER_INFO_SHADOW_TEMPORAL;
    storage.shadow_samples = 8;
    storage.use_all_lights = 0; /* The V1 input light group was always a filter. */
    storage.mapping_interpolation = SHD_PRINCIPLED_NPR_INTERP_EASE;
    storage.driven_initialized_count = PRINCIPLED_NPR_MAX_STOPS;
    principled_npr::initialize_ramp(storage);
    const int count = clamp_i(storage.diffuse_ramp.tot, 2, SHD_PRINCIPLED_NPR_MAX_RAMP_POINTS);
    principled_npr::set_point_count(storage, count);
    for (int i = 0; i < count; i++) {
      NodePrincipledNPRRampPoint &point = storage.ramp_points[i];
      const CBData &legacy = storage.diffuse_ramp.data[i];
      point.position = legacy.pos;
      point.color[0] = legacy.r;
      point.color[1] = legacy.g;
      point.color[2] = legacy.b;
      point.color[3] = legacy.a;
    }
  }
  copy->location[0] += source.width + 40.0f;
  STRNCPY(copy->label, "V2 upgrade (review)");
  nodes::update_node_declaration_and_sockets(tree, *copy, CTX_data_main(C));
  node_update(&tree, copy);

  Vector<bNodeLink *> incoming_links;
  for (bNodeLink &link : tree.links) {
    if (link.tonode == &source && link.fromnode && link.fromsock) {
      incoming_links.append(&link);
    }
  }
  for (const bNodeLink *link : incoming_links) {
    if (bNodeSocket *input = bke::node_find_socket(
            *copy, SOCK_IN, UString(link->tosock->identifier)))
    {
      bke::node_add_link(tree, *link->fromnode, *link->fromsock, *copy, *input);
    }
  }
  bke::node_set_active(tree, *copy);
  PointerRNA copy_ptr = RNA_pointer_create_discrete(&tree.id, RNA_Node, copy);
  socket_items::ops::update_after_node_change(C, copy_ptr);
  BKE_report(
      op->reports,
      RPT_WARNING,
      "Created an unconnected V2 copy. Legacy boundary, highlight, color ramp and light energy "
      "are not appearance-equivalent. Animation and drivers remain on the unchanged source node; "
      "review and migrate them explicitly before connecting the V2 output");
  return OPERATOR_FINISHED;
}

static void node_operators()
{
  WM_operatortype_append([](wmOperatorType *ot) {
    ot->name = "Add NPR Mapping Point";
    ot->idname = "NODE_OT_principled_npr_point_add";
    ot->description =
        "Add a new point with a new animation identity; does not restore deleted points";
    ot->poll = socket_items::ops::editable_node_active_poll<PrincipledNPRActions>;
    ot->exec = point_add_exec;
    ot->flag = OPTYPE_UNDO;
    socket_items::ops::add_node_identifier_property(ot);
  });
  WM_operatortype_append([](wmOperatorType *ot) {
    ot->name = "Delete NPR Mapping Point";
    ot->idname = "NODE_OT_principled_npr_point_remove";
    ot->description =
        "Delete the selected point; new points never inherit its animation. Use Point Count to "
        "suspend points instead";
    ot->poll = socket_items::ops::editable_node_active_poll<PrincipledNPRActions>;
    ot->exec = point_remove_exec;
    ot->flag = OPTYPE_UNDO;
    socket_items::ops::add_node_identifier_property(ot);
  });
  WM_operatortype_append([](wmOperatorType *ot) {
    ot->name = "Create NPR V2 Copy";
    ot->idname = "NODE_OT_principled_npr_upgrade_copy";
    ot->description =
        "Create an unconnected current V2 copy; legacy appearance, animation and drivers are not "
        "automatically converted";
    ot->poll = socket_items::ops::editable_node_active_poll<PrincipledNPRActions>;
    ot->exec = upgrade_copy_exec;
    ot->invoke = [](bContext *C, wmOperator *op, const wmEvent * /*event*/) {
      return WM_operator_confirm_ex(C,
                                    op,
                                    IFACE_("Create V2 Copy?"),
                                    IFACE_("Appearance is not equivalent. Animation and drivers "
                                           "remain on the original node."),
                                    IFACE_("Create Copy"),
                                    ui::AlertIcon::Warning,
                                    true);
    };
    ot->flag = OPTYPE_UNDO;
    socket_items::ops::add_node_identifier_property(ot);
  });
}

static void node_extra_info(NodeExtraInfoParams &params)
{
  const Scene *scene = CTX_data_scene(&params.C);
  if (scene != nullptr && StringRef(scene->r.engine) == RE_engine_id_BLENDER_EEVEE) {
    return;
  }
  NodeExtraInfoRow row;
  row.text = RPT_("Eevee Only");
  row.tooltip = TIP_("Principled NPR is evaluated only by the Eevee render engine");
  row.icon = ICON_ERROR;
  params.rows.append(std::move(row));
}

static int node_gpu(GPUMaterial *mat,
                    bNode *node,
                    bNodeExecData * /*execdata*/,
                    GPUNodeStack *in,
                    GPUNodeStack *out)
{
  if (node->storage == nullptr) {
    return 0;
  }
  const NodeShaderPrincipledNPR &storage = node_storage(*node);
  if (storage.model_version >= 2) {
    return node_gpu_v2(mat, node, in, out);
  }
  if (!in[SOCK_NORMAL].link) {
    GPU_link(mat, "world_normals_get", &in[SOCK_NORMAL].link);
  }
  GPU_material_flag_set(mat, GPU_MATFLAG_GLSL_LIGHT_ACCESS);

  const bool any_output = out[0].hasoutput || out[1].hasoutput || out[2].hasoutput;
  if (storage.shadow_mode == SHD_PRINCIPLED_NPR_SHADOW_CAST_ONLY && any_output) {
    GPU_material_shader_info_shadow_classification_set(mat);
  }
  if (any_output &&
      (in[SOCK_REFLECTION_STRENGTH].link ||
       in[SOCK_REFLECTION_STRENGTH].socket_not_zero() || in[SOCK_AMBIENT_STRENGTH].link ||
       in[SOCK_AMBIENT_STRENGTH].socket_not_zero()))
  {
    GPU_material_flag_set(mat, GPU_MATFLAG_LIGHTPROBE_ACCESS);
  }
  if (out[0].hasoutput) {
    GPU_material_flag_set(mat, GPU_MATFLAG_EMISSION);
    if (in[SOCK_ALPHA].link || in[SOCK_ALPHA].socket_not_one()) {
      GPU_material_flag_set(mat, GPU_MATFLAG_TRANSPARENT);
    }
  }

  float *diffuse_data;
  float diffuse_layer;
  int diffuse_size;
  BKE_colorband_evaluate_table_rgba(
      &storage.diffuse_ramp, &diffuse_data, &diffuse_size);
  GPUNodeLink *diffuse_ramp = GPU_color_band(
      mat, diffuse_size, diffuse_data, &diffuse_layer);

  float *specular_data;
  float specular_layer;
  int specular_size;
  BKE_colorband_evaluate_table_rgba(
      &storage.specular_ramp, &specular_data, &specular_size);
  GPUNodeLink *specular_ramp = GPU_color_band(
      mat, specular_size, specular_data, &specular_layer);

  const float mode0 = float(storage.diffuse_mapping + storage.specular_mapping * 10 +
                            storage.coordinate_range * 100 + storage.color_application * 1000 +
                            storage.mapping_stage * 10000 + storage.light_combine * 100000 +
                            storage.shadow_mode * 1000000);
  const float mode1 = float(storage.driven_interpolation + driven_stop_count(node) * 10 +
                            clamp_i(storage.lightgroup_id, 0, SHD_PRINCIPLED_NPR_LIGHTGROUP_MAX) *
                                100);
  const float unused_control = 0.0f;

  GPUNodeLink *diffuse_controls = nullptr;
  GPU_link(mat,
           "principled_npr_pack4",
           npr_socket(*node, in, "boundary"),
           npr_socket(*node, in, "softness"),
           npr_socket(*node, in, "coordinate_scale"),
           npr_socket(*node, in, "coordinate_offset"),
           &diffuse_controls);
  GPUNodeLink *lighting_controls = nullptr;
  GPU_link(mat,
           "principled_npr_pack4",
           npr_socket(*node, in, "shadow_strength"),
           npr_socket(*node, in, "direct_strength"),
           npr_socket(*node, in, "intensity_influence"),
           npr_socket(*node, in, "light_color_influence"),
           &lighting_controls);
  GPUNodeLink *highlight_controls = nullptr;
  GPU_link(mat,
           "principled_npr_pack4",
           npr_socket(*node, in, "highlight_strength"),
           npr_socket(*node, in, "highlight_size"),
           npr_socket(*node, in, "highlight_softness"),
           GPU_constant(&unused_control),
           &highlight_controls);
  GPUNodeLink *highlight_details = nullptr;
  GPU_link(mat,
           "principled_npr_pack4",
           npr_socket(*node, in, "highlight_offset"),
           npr_socket(*node, in, "highlight_light_color_influence"),
           npr_socket(*node, in, "anisotropy"),
           npr_socket(*node, in, "anisotropy_rotation"),
           &highlight_details);
  GPUNodeLink *environment_controls = nullptr;
  GPU_link(mat,
           "principled_npr_pack4",
           npr_socket(*node, in, "reflection_strength"),
           npr_socket(*node, in, "ambient_strength"),
           npr_socket(*node, in, "ambient_directionality"),
           GPU_constant(&unused_control),
           &environment_controls);
  GPUNodeLink *rim_controls = nullptr;
  GPU_link(mat,
           "principled_npr_pack4",
           npr_socket(*node, in, "rim_strength"),
           npr_socket(*node, in, "rim_angle"),
           npr_socket(*node, in, "rim_length"),
           npr_socket(*node, in, "rim_length_falloff"),
           &rim_controls);
  GPUNodeLink *rim_details = nullptr;
  GPU_link(mat,
           "principled_npr_pack4",
           npr_socket(*node, in, "rim_thickness"),
           npr_socket(*node, in, "rim_thickness_falloff"),
           npr_socket(*node, in, "rim_light_bias"),
           npr_socket(*node, in, "rim_mask"),
           &rim_details);
  GPUNodeLink *stop_positions_0_3 = nullptr;
  GPU_link(mat,
           "principled_npr_pack4",
           npr_socket(*node, in, "stop_position_0"),
           npr_socket(*node, in, "stop_position_1"),
           npr_socket(*node, in, "stop_position_2"),
           npr_socket(*node, in, "stop_position_3"),
           &stop_positions_0_3);
  GPUNodeLink *stop_positions_4_7 = nullptr;
  GPU_link(mat,
           "principled_npr_pack4",
           npr_socket(*node, in, "stop_position_4"),
           npr_socket(*node, in, "stop_position_5"),
           npr_socket(*node, in, "stop_position_6"),
           npr_socket(*node, in, "stop_position_7"),
           &stop_positions_4_7);
  GPUNodeLink *mode_controls = nullptr;
  GPU_link(mat,
           "principled_npr_pack4",
           GPU_constant(&diffuse_layer),
           GPU_constant(&specular_layer),
           GPU_constant(&mode0),
           GPU_constant(&mode1),
           &mode_controls);

  GPUNodeLink *shader_link = nullptr;
  GPUNodeLink *color_link = nullptr;
  GPUNodeLink *alpha_link = nullptr;
  const bool ok = GPU_link(mat,
                           "node_principled_npr_v1",
                           npr_socket(*node, in, "base_color"),
                           npr_socket(*node, in, "metallic"),
                           npr_socket(*node, in, "roughness"),
                           npr_socket(*node, in, "alpha"),
                           npr_socket(*node, in, "normal"),
                           npr_socket(*node, in, "Weight"),
                           npr_socket(*node, in, "shadow_color"),
                           npr_socket(*node, in, "lit_color"),
                           diffuse_controls,
                           lighting_controls,
                           npr_socket(*node, in, "highlight_color"),
                           highlight_controls,
                           highlight_details,
                           npr_socket(*node, in, "tangent"),
                           environment_controls,
                           npr_socket(*node, in, "rim_color"),
                           rim_controls,
                           rim_details,
                           npr_socket(*node, in, "emission_color"),
                           npr_socket(*node, in, "emission_strength"),
                           npr_socket(*node, in, "stop_color_0"),
                           npr_socket(*node, in, "stop_color_1"),
                           npr_socket(*node, in, "stop_color_2"),
                           npr_socket(*node, in, "stop_color_3"),
                           npr_socket(*node, in, "stop_color_4"),
                           npr_socket(*node, in, "stop_color_5"),
                           npr_socket(*node, in, "stop_color_6"),
                           npr_socket(*node, in, "stop_color_7"),
                           stop_positions_0_3,
                           stop_positions_4_7,
                           diffuse_ramp,
                           specular_ramp,
                           mode_controls,
                           &shader_link,
                           &color_link,
                           &alpha_link);
  out[0].link = shader_link;
  out[1].link = color_link;
  out[2].link = alpha_link;
  return int(ok);
}

}  // namespace nodes::node_shader_principled_npr_cc

void register_node_type_sh_principled_npr()
{
  namespace file_ns = nodes::node_shader_principled_npr_cc;

  static bke::bNodeType ntype;
  sh_node_type_base(&ntype, "ShaderNodePrincipledNPR"_ustr, SH_NODE_PRINCIPLED_NPR);
  ntype.ui_name = "Principled NPR";
  ntype.ui_description =
      "Eevee-native stylized material with programmable diffuse mapping, artistic highlights, "
      "probe lighting, and rim control";
  ntype.enum_name_legacy = "PRINCIPLED_NPR";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  ntype.blend_data_read_storage_content = file_ns::node_blend_read;
  ntype.register_operators = file_ns::node_operators;
  ntype.gpu_fn = file_ns::node_gpu;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.gather_link_search_ops = search_link_ops_for_shader_bsdf_node;
  ntype.get_extra_info = file_ns::node_extra_info;
  ntype.default_width = bke::NodeWidth::_240;
  bke::node_type_storage(
      ntype, "NodeShaderPrincipledNPR", node_free_standard_storage, node_copy_standard_storage);
  bke::node_register_type(ntype);
}

}  // namespace blender
