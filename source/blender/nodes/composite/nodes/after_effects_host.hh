#pragma once

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <string>
#include <vector>

#include "BLI_math_vector_types.hh"
#include "BLI_string_ref.hh"

#include "NOD_menu_value.hh"

#include "COM_context.hh"
#include "COM_domain.hh"
#include "COM_result.hh"

namespace blender::nodes::after_effects {

enum class ParamType : uint8_t {
  MainInput,
  Layer,
  Int,
  Float,
  Bool,
  Color,
  Vector2,
  Vector3,
  Menu,
  GroupStart,
  GroupEnd,
  Unsupported,
};

struct PopupItem {
  int value = 0;
  std::string name;
  std::string identifier;
};

struct PluginParameter {
  int index = 0;
  int disk_id = 0;
  int ae_param_type = 0;
  ParamType type = ParamType::Unsupported;
  std::string name;
  std::string identifier;
  bool expose_as_socket = false;
  bool use_host_default_if_unchanged = false;
  bool layer_default_myself = false;
  float min_value = 0.0f;
  float max_value = 0.0f;
  float default_value = 0.0f;
  float4 vector_default = float4(0.0f);
  std::vector<PopupItem> popup_items;
};

struct PluginMetadata {
  bool host_available = false;
  bool success = false;
  bool has_unsupported_parameters = false;
  bool supports_deep_color = false;
  bool supports_smart_render = false;
  bool supports_float_color = false;
  bool expands_buffer = false;
  int signature_hash = 0;
  std::string error;
  std::string absolute_path;
  std::string effect_name;
  std::string match_name;
  std::string category;
  std::string entry_point;
  std::vector<PluginParameter> parameters;
};

struct RenderValue {
  std::string identifier;
  const compositor::Result *image = nullptr;
  bool is_linked = false;
  float scalar = 0.0f;
  int integer = 0;
  bool boolean = false;
  float4 vector = float4(0.0f);
  nodes::MenuValue menu_value;
};

bool host_is_available();

PluginMetadata parse_plugin_metadata(StringRef filepath);

bool render_plugin(compositor::Context &context,
                   StringRef filepath,
                   const std::vector<RenderValue> &values,
                   compositor::Result &r_output,
                   compositor::Domain &r_output_domain,
                   std::string &r_error);

}  // namespace blender::nodes::after_effects
