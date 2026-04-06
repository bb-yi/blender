/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include <cctype>
#include <cstdlib>
#include <sstream>

#include "node_exec.hh"
#include "node_shader_util.hh"

#include "BKE_image.hh"
#include "BKE_node_runtime.hh"
#include "BKE_text.h"

#include "BLI_fileops.h"
#include "BLI_ghash.h"
#include "BLI_map.hh"
#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_set.hh"
#include "BLI_string.h"

#include "RNA_access.hh"

#include "NOD_sync_sockets.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_glsl_function_cc {

NODE_STORAGE_FUNCS(NodeShaderGLSLFunction)

enum class GLSLBoundaryType {
  Unsupported = 0,
  Float,
  Vec2,
  Vec3,
  Vec4,
  Sampler2D,
  Sample2D,
  Void,
};

struct GLSLToken {
  enum class Kind {
    Identifier,
    Punctuation,
  };

  Kind kind;
  std::string text;
  char punctuation = '\0';
  int64_t source_start = 0;
  int64_t source_end = 0;
};

struct GLSLFunctionParam {
  enum class Qualifier {
    In,
    Out,
    InOut,
  };

  struct Meta {
    bool has_default_value = false;
    float4 default_value = float4(0.0f);
    bool has_min = false;
    float min_value = 0.0f;
    bool has_max = false;
    float max_value = 0.0f;
    bool hide_value = false;
    std::optional<PropertySubType> subtype;

    bool has_any() const
    {
      return has_default_value || has_min || has_max || hide_value || subtype.has_value();
    }
  };

  GLSLBoundaryType type = GLSLBoundaryType::Unsupported;
  Qualifier qualifier = Qualifier::In;
  std::string type_name;
  std::string name;
  std::string identifier;
  int dimensions = 0;
  int64_t type_source_start = -1;
  int64_t type_source_end = -1;
  Meta meta;
};

struct GLSLFunctionDefinition {
  std::string name;
  std::string return_type_name;
  GLSLBoundaryType return_type = GLSLBoundaryType::Unsupported;
  Vector<GLSLFunctionParam> params;
  int body_token_start = -1;
  int body_token_end = -1;
};

struct GLSLParseResult {
  bool ok = false;
  bool keep_existing_sockets = false;
  bool used_first_function = false;

  std::string error;
  std::string source;
  std::string source_hash_hex;
  std::string source_filename;
  std::string source_prefix;
  std::string wrapper_filename;
  std::string wrapper_name;
  std::string resolved_function_name;
  std::string library_source;
  std::string wrapper_source;

  Vector<std::string> function_names;
  GLSLFunctionDefinition function;
  int signature_hash = 0;
  int meta_hash = 0;
};

struct GLSLSample2DUsage {
  int texture_calls = 0;
  bool uses_texture_lod = false;
  bool uses_texture_grad = false;
  bool uses_texture_size = false;
  bool uses_texel_fetch = false;
  bool uses_texture_gather = false;
};

enum class GLSLSample2DSourceKind : uint8_t {
  None = 0,
  ImageToClosure,
  ClosureOutput,
  Unsupported,
};

struct GLSLRawParamMeta {
  std::optional<std::string> default_value;
  std::optional<std::string> min_value;
  std::optional<std::string> max_value;
  std::optional<std::string> hide_value;
  std::optional<std::string> subtype;

  bool has_any() const
  {
    return default_value.has_value() || min_value.has_value() || max_value.has_value() ||
           hide_value.has_value() || subtype.has_value();
  }
};

struct GLSLClosureSampleHelper {
  std::string param_name;
  std::string helper_name;
  std::string uv_global_name;
  std::string sub_function_name;
};

static constexpr const char *result_socket_identifier = "Result";

static Vector<GLSLToken> tokenize_glsl_source(const StringRef source);
static const bNodeSocket *find_node_input_socket_by_identifier(const bNode &node,
                                                               const StringRef identifier);
static bNodeSocket *find_node_input_socket_by_identifier(bNode &node, const StringRef identifier);
static const bNodeSocket *find_closure_output_socket_by_name(const bNode &node, const StringRef name);
static bool closure_output_has_required_sample2d_signature(const bNode &closure_output_node,
                                                           std::string &r_error);
static bool glsl_param_has_input_socket(const GLSLFunctionParam &param);

static bool is_identifier_start(const char c)
{
  return std::isalpha(uchar(c)) || c == '_';
}

static bool is_identifier_continue(const char c)
{
  return std::isalnum(uchar(c)) || c == '_';
}

static GLSLBoundaryType glsl_boundary_type_from_name(const StringRef type_name)
{
  if (type_name == "float") {
    return GLSLBoundaryType::Float;
  }
  if (type_name == "vec2") {
    return GLSLBoundaryType::Vec2;
  }
  if (type_name == "vec3") {
    return GLSLBoundaryType::Vec3;
  }
  if (type_name == "vec4") {
    return GLSLBoundaryType::Vec4;
  }
  if (type_name == "sampler2D") {
    return GLSLBoundaryType::Sampler2D;
  }
  if (type_name == "sample2D") {
    return GLSLBoundaryType::Sample2D;
  }
  if (type_name == "void") {
    return GLSLBoundaryType::Void;
  }
  return GLSLBoundaryType::Unsupported;
}

static int glsl_boundary_dimensions(const GLSLBoundaryType type)
{
  switch (type) {
    case GLSLBoundaryType::Vec2:
      return 2;
    case GLSLBoundaryType::Vec3:
      return 3;
    case GLSLBoundaryType::Vec4:
      return 4;
    default:
      return 0;
  }
}

static bool glsl_boundary_type_is_sampler(const GLSLBoundaryType type)
{
  return ELEM(type, GLSLBoundaryType::Sampler2D, GLSLBoundaryType::Sample2D);
}

static bool glsl_boundary_type_is_image_sampler(const GLSLBoundaryType type)
{
  return type == GLSLBoundaryType::Sampler2D;
}

static bool glsl_boundary_type_is_sample2d(const GLSLBoundaryType type)
{
  return type == GLSLBoundaryType::Sample2D;
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

static GLSLSample2DSourceKind resolve_sample2d_source_kind(const bNode &node,
                                                           const GLSLFunctionParam &param,
                                                           const bNodeLink *&r_link)
{
  const bNodeSocket *sample_socket = find_node_input_socket_by_identifier(node, param.identifier);
  if ((sample_socket == nullptr || !sample_socket->is_directly_linked()) && node.runtime->original) {
    sample_socket = find_node_input_socket_by_identifier(*node.runtime->original, param.identifier);
  }
  if (sample_socket == nullptr) {
    r_link = nullptr;
    return GLSLSample2DSourceKind::None;
  }

  r_link = find_used_direct_link(*sample_socket);
  if (r_link == nullptr || r_link->fromnode == nullptr) {
    return GLSLSample2DSourceKind::None;
  }
  if (r_link->fromnode->is_type("ShaderNodeImageToClosure")) {
    return GLSLSample2DSourceKind::ImageToClosure;
  }
  if (r_link->fromnode->is_type("NodeClosureOutput")) {
    return GLSLSample2DSourceKind::ClosureOutput;
  }
  return GLSLSample2DSourceKind::Unsupported;
}

static Image *resolve_image_to_closure_image(const bNodeLink &link)
{
  if (link.fromnode == nullptr || !link.fromnode->is_type("ShaderNodeImageToClosure")) {
    return nullptr;
  }
  const bNodeSocket &image_socket = link.fromnode->input_socket(0);
  if (image_socket.type != SOCK_IMAGE || image_socket.default_value == nullptr) {
    return nullptr;
  }
  return image_socket.default_value_typed<bNodeSocketValueImage>()->value;
}

static GPUSamplerState sampler_state_from_image_to_closure_node(const bNode &node)
{
  GPUSamplerState sampler_state = GPUSamplerState::default_sampler();

  switch (node.custom2) {
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

  if (node.custom1 != SHD_INTERP_CLOSEST) {
    sampler_state.filtering = GPU_SAMPLER_FILTERING_ANISOTROPIC | GPU_SAMPLER_FILTERING_LINEAR |
                              GPU_SAMPLER_FILTERING_MIPMAP;
  }

  return sampler_state;
}

static Map<std::string, GLSLSample2DUsage> analyze_sample2d_usages(
    const Vector<GLSLToken> &tokens, const GLSLFunctionDefinition &function)
{
  Map<std::string, GLSLSample2DUsage> usage_by_name;
  for (const GLSLFunctionParam &param : function.params) {
    if (glsl_boundary_type_is_sample2d(param.type) && glsl_param_has_input_socket(param)) {
      usage_by_name.add(param.name, {});
    }
  }
  if (usage_by_name.is_empty() || function.body_token_start < 0 || function.body_token_end < 0) {
    return usage_by_name;
  }

  auto mark_usage = [&](const StringRef param_name, const StringRef call_name) {
    GLSLSample2DUsage &usage = usage_by_name.lookup(param_name);
    if (call_name == "texture") {
      usage.texture_calls++;
    }
    else if (call_name == "textureLod") {
      usage.uses_texture_lod = true;
    }
    else if (call_name == "textureGrad") {
      usage.uses_texture_grad = true;
    }
    else if (call_name == "textureSize") {
      usage.uses_texture_size = true;
    }
    else if (call_name == "texelFetch") {
      usage.uses_texel_fetch = true;
    }
    else if (call_name == "textureGather") {
      usage.uses_texture_gather = true;
    }
  };

  for (int i = function.body_token_start; i <= function.body_token_end; i++) {
    const GLSLToken &token = tokens[i];
    if (token.kind != GLSLToken::Kind::Identifier) {
      continue;
    }
    if (!ELEM(token.text.c_str(),
              "texture",
              "textureLod",
              "textureGrad",
              "textureSize",
              "texelFetch",
              "textureGather"))
    {
      continue;
    }
    if ((i + 1) > function.body_token_end || tokens[i + 1].kind != GLSLToken::Kind::Punctuation ||
        tokens[i + 1].punctuation != '(')
    {
      continue;
    }

    int paren_depth = 1;
    std::string first_identifier;
    for (int j = i + 2; j <= function.body_token_end; j++) {
      const GLSLToken &arg_token = tokens[j];
      if (arg_token.kind == GLSLToken::Kind::Punctuation) {
        if (arg_token.punctuation == '(') {
          paren_depth++;
        }
        else if (arg_token.punctuation == ')') {
          paren_depth--;
          if (paren_depth == 0) {
            break;
          }
        }
        else if (arg_token.punctuation == ',' && paren_depth == 1) {
          break;
        }
        continue;
      }
      if (paren_depth == 1 && arg_token.kind == GLSLToken::Kind::Identifier &&
          first_identifier.empty())
      {
        first_identifier = arg_token.text;
      }
    }

    if (!first_identifier.empty() && usage_by_name.contains(first_identifier)) {
      mark_usage(first_identifier, token.text);
    }
  }

  return usage_by_name;
}

static Map<std::string, GLSLSample2DSourceKind> resolve_sample2d_source_kinds(
    const bNode &node, const GLSLFunctionDefinition &function)
{
  Map<std::string, GLSLSample2DSourceKind> source_kinds;
  for (const GLSLFunctionParam &param : function.params) {
    if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_sample2d(param.type)) {
      continue;
    }
    const bNodeLink *used_link = nullptr;
    source_kinds.add_new(param.name, resolve_sample2d_source_kind(node, param, used_link));
  }
  return source_kinds;
}

static bool sample2d_usage_requires_image_source(const GLSLSample2DUsage &usage)
{
  return usage.uses_texture_size || usage.uses_texel_fetch || usage.uses_texture_gather;
}

static bool sample2d_usage_supported_for_closure_output(const GLSLSample2DUsage &usage)
{
  return !usage.uses_texture_lod && !usage.uses_texture_grad &&
         !sample2d_usage_requires_image_source(usage);
}

static bool validate_sample2d_direct_texture_usage(const Vector<GLSLToken> &tokens,
                                                   const GLSLFunctionDefinition &function,
                                                   const StringRef param_name)
{
  Set<int> allowed_token_indices;
  for (int i = function.body_token_start; i <= function.body_token_end; i++) {
    const GLSLToken &token = tokens[i];
    if (token.kind != GLSLToken::Kind::Identifier || token.text != "texture") {
      continue;
    }
    if ((i + 1) > function.body_token_end || tokens[i + 1].kind != GLSLToken::Kind::Punctuation ||
        tokens[i + 1].punctuation != '(')
    {
      continue;
    }
    int paren_depth = 1;
    int comma_index = -1;
    int closing_paren_index = -1;
    for (int j = i + 2; j <= function.body_token_end; j++) {
      const GLSLToken &arg_token = tokens[j];
      if (arg_token.kind != GLSLToken::Kind::Punctuation) {
        continue;
      }
      if (arg_token.punctuation == '(') {
        paren_depth++;
      }
      else if (arg_token.punctuation == ')') {
        paren_depth--;
        if (paren_depth == 0) {
          closing_paren_index = j;
          break;
        }
      }
      else if (arg_token.punctuation == ',' && paren_depth == 1) {
        comma_index = j;
      }
    }
    if (comma_index == -1 || closing_paren_index == -1 || (comma_index - (i + 2)) != 1) {
      continue;
    }
    if (tokens[i + 2].kind == GLSLToken::Kind::Identifier && tokens[i + 2].text == param_name) {
      allowed_token_indices.add(i + 2);
    }
  }

  for (int i = function.body_token_start; i <= function.body_token_end; i++) {
    const GLSLToken &token = tokens[i];
    if (token.kind == GLSLToken::Kind::Identifier && token.text == param_name &&
        !allowed_token_indices.contains(i))
    {
      return false;
    }
  }
  return true;
}

static std::string make_socket_identifier(const StringRef prefix, const StringRef name)
{
  std::string identifier;
  identifier.reserve(prefix.size() + name.size() + 8);
  identifier.append(prefix);
  identifier.push_back('_');

  if (name.is_empty()) {
    identifier.append("value");
  }
  else {
    for (const char c : name) {
      identifier.push_back(is_identifier_continue(c) ? c : '_');
    }
  }

  if (!identifier.empty() && std::isdigit(uchar(identifier.back()))) {
    identifier.push_back('_');
  }
  return identifier;
}

static std::string make_wrapper_argument_name(const StringRef prefix, const StringRef name)
{
  std::string identifier = make_socket_identifier(prefix, name);
  return identifier;
}

static bool glsl_param_has_input_socket(const GLSLFunctionParam &param)
{
  return ELEM(param.qualifier,
              GLSLFunctionParam::Qualifier::In,
              GLSLFunctionParam::Qualifier::InOut);
}

static bool glsl_param_has_output_socket(const GLSLFunctionParam &param)
{
  return ELEM(param.qualifier,
              GLSLFunctionParam::Qualifier::Out,
              GLSLFunctionParam::Qualifier::InOut);
}

static int glsl_function_output_count(const GLSLFunctionDefinition &function)
{
  int count = function.return_type == GLSLBoundaryType::Void ? 0 : 1;
  for (const GLSLFunctionParam &param : function.params) {
    if (glsl_param_has_output_socket(param)) {
      count++;
    }
  }
  return count;
}

static bool glsl_function_has_sampler_inputs(const GLSLFunctionDefinition &function)
{
  for (const GLSLFunctionParam &param : function.params) {
    if (glsl_param_has_input_socket(param) && glsl_boundary_type_is_sampler(param.type)) {
      return true;
    }
  }
  return false;
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

static bNodeSocket *find_node_input_socket_by_identifier(bNode &node, const StringRef identifier)
{
  for (bNodeSocket *socket : node.input_sockets()) {
    if (identifier == socket->identifier) {
      return socket;
    }
  }
  return nullptr;
}

static bool validate_sampler_inputs(const bNode &node,
                                    const Vector<GLSLToken> &tokens,
                                    const GLSLFunctionDefinition &function,
                                    std::string &r_error)
{
  const Map<std::string, GLSLSample2DUsage> usage_by_name = analyze_sample2d_usages(tokens,
                                                                                    function);

  for (const GLSLFunctionParam &param : function.params) {
    if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_sampler(param.type)) {
      continue;
    }

    const bNodeSocket *socket = find_node_input_socket_by_identifier(node, param.identifier);
    if ((socket == nullptr || !socket->is_directly_linked()) && node.runtime->original) {
      socket = find_node_input_socket_by_identifier(*node.runtime->original, param.identifier);
    }
    if (socket == nullptr) {
      continue;
    }

    if (glsl_boundary_type_is_image_sampler(param.type)) {
      if (socket->type != SOCK_IMAGE || socket->default_value == nullptr) {
        continue;
      }
      if (socket->is_directly_linked()) {
        r_error = param.type_name + " parameter '" + param.name +
                  "' does not support links yet; choose an image on the node";
        return false;
      }

      Image *image = socket->default_value_typed<bNodeSocketValueImage>()->value;
      if (image == nullptr) {
        r_error = "Choose an image for " + param.type_name + " parameter '" + param.name + "'";
        return false;
      }
      if (image->source == IMA_SRC_TILED) {
        r_error = param.type_name + " parameter '" + param.name +
                  "' does not support UDIM tiled images yet";
        return false;
      }
      continue;
    }

    if (!socket->is_directly_linked()) {
      r_error = "Connect an Image to Closure node to sample2D parameter '" + param.name + "'";
      return false;
    }

    const bNodeLink *used_link = nullptr;
    const GLSLSample2DSourceKind source_kind = resolve_sample2d_source_kind(node, param, used_link);
    if (source_kind == GLSLSample2DSourceKind::ClosureOutput) {
      const GLSLSample2DUsage usage = usage_by_name.lookup_default(param.name, {});
      if (sample2d_usage_requires_image_source(usage)) {
        r_error = "sample2D parameter '" + param.name +
                  "' uses image-only sampling functions that require Image to Closure";
        return false;
      }
      if (!sample2d_usage_supported_for_closure_output(usage)) {
        r_error = "sample2D parameter '" + param.name +
                  "' only supports texture(tex, uv) when driven by Closure Output";
        return false;
      }
      if (!validate_sample2d_direct_texture_usage(tokens, function, param.name)) {
        r_error = "sample2D parameter '" + param.name +
                  "' only supports direct texture(tex, uv) calls when driven by Closure Output";
        return false;
      }
      std::string closure_error;
      if (!closure_output_has_required_sample2d_signature(*used_link->fromnode, closure_error)) {
        r_error = "Closure Output connected to sample2D parameter '" + param.name +
                  "' is missing required closure items: " + closure_error;
        return false;
      }
      continue;
    }
    if (source_kind != GLSLSample2DSourceKind::ImageToClosure || used_link == nullptr) {
      r_error = "sample2D parameter '" + param.name +
                "' currently only supports Image to Closure";
      return false;
    }

    Image *image = resolve_image_to_closure_image(*used_link);
    if (image == nullptr) {
      r_error = "Choose an image on the Image to Closure node connected to '" + param.name + "'";
      return false;
    }
    if (image->source == IMA_SRC_TILED) {
      r_error = "sample2D parameter '" + param.name +
                "' does not support UDIM tiled images yet";
      return false;
    }
  }
  return true;
}

static bool node_has_sampler_input_sockets(const bNode &node)
{
  for (const bNodeSocket *socket : node.input_sockets()) {
    if (socket->type == SOCK_IMAGE) {
      return true;
    }
  }
  return false;
}

static void sync_sampler_socket_visibility(bNode &node, const GLSLParseResult &parse_result)
{
  if (parse_result.function.name.empty()) {
    return;
  }

  for (bNodeSocket *socket : node.input_sockets()) {
    if (socket->type == SOCK_IMAGE) {
      socket->flag &= ~SOCK_HIDDEN;
    }
  }

  for (const GLSLFunctionParam &param : parse_result.function.params) {
    if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_image_sampler(param.type)) {
      continue;
    }
    if (bNodeSocket *socket = find_node_input_socket_by_identifier(node, param.identifier)) {
      socket->flag |= SOCK_HIDDEN;
    }
  }

}

static void draw_sampler_input_properties(ui::Layout &layout, PointerRNA *node_ptr, bNode &node)
{
  if (!node_has_sampler_input_sockets(node)) {
    return;
  }

  for (bNodeSocket *socket : node.input_sockets()) {
    if (socket->type != SOCK_IMAGE) {
      continue;
    }

    PointerRNA socket_ptr = RNA_pointer_create_discrete(node_ptr->owner_id, RNA_NodeSocket, socket);
    layout.prop(&socket_ptr, "default_value", ui::ITEM_R_SPLIT_EMPTY_NAME, socket->name, ICON_NONE);
  }
}

static std::string trim_copy(const StringRef text)
{
  int64_t start = 0;
  int64_t end = text.size();
  while (start < end && std::isspace(uchar(text[start]))) {
    start++;
  }
  while (end > start && std::isspace(uchar(text[end - 1]))) {
    end--;
  }
  return text.substr(start, end - start);
}

static std::string strip_glsl_comments(StringRef source)
{
  std::string stripped;
  stripped.reserve(source.size());

  for (int64_t i = 0; i < source.size();) {
    if ((i + 1) < source.size() && source[i] == '/' && source[i + 1] == '/') {
      i += 2;
      while (i < source.size() && source[i] != '\n') {
        i++;
      }
      continue;
    }
    if ((i + 1) < source.size() && source[i] == '/' && source[i + 1] == '*') {
      i += 2;
      while ((i + 1) < source.size() && !(source[i] == '*' && source[i + 1] == '/')) {
        i++;
      }
      i = std::min<int64_t>(i + 2, source.size());
      continue;
    }
    stripped.push_back(source[i]);
    i++;
  }

  return stripped;
}

static std::string make_glsl_meta_key(const StringRef function_name, const StringRef param_name)
{
  std::string key;
  key.reserve(function_name.size() + param_name.size() + 1);
  key.append(function_name);
  key.push_back('\x1f');
  key.append(param_name);
  return key;
}

static void split_glsl_meta_key(const StringRef key,
                                StringRef &r_function_name,
                                StringRef &r_param_name)
{
  const int64_t separator = key.find('\x1f');
  if (separator == StringRef::not_found) {
    r_function_name = "";
    r_param_name = key;
    return;
  }
  r_function_name = key.substr(0, separator);
  r_param_name = key.substr(separator + 1);
}

static std::string lowercase_copy(StringRef text)
{
  std::string result(text);
  for (char &c : result) {
    c = std::tolower(uchar(c));
  }
  return result;
}

static bool parse_glsl_meta_bool_literal(const StringRef text, bool &r_value, std::string &r_error)
{
  const std::string normalized = lowercase_copy(trim_copy(text));
  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
    r_value = true;
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
    r_value = false;
    return true;
  }
  r_error = "Expected a GLSL meta boolean literal";
  return false;
}

static bool parse_glsl_meta_assignment_list(const StringRef text,
                                            Map<std::string, std::string> &r_assignments,
                                            std::string &r_error)
{
  for (int64_t i = 0; i < text.size();) {
    while (i < text.size() && std::isspace(uchar(text[i]))) {
      i++;
    }
    if (i >= text.size()) {
      break;
    }
    if (!is_identifier_start(text[i])) {
      r_error = "Malformed GLSL meta attribute list";
      return false;
    }

    const int64_t key_start = i;
    i++;
    while (i < text.size() && is_identifier_continue(text[i])) {
      i++;
    }
    const std::string key = std::string(text.substr(key_start, i - key_start));

    while (i < text.size() && std::isspace(uchar(text[i]))) {
      i++;
    }
    if (i >= text.size() || text[i] != '=') {
      r_error = "GLSL meta attributes must use key=value syntax";
      return false;
    }
    i++;

    while (i < text.size() && std::isspace(uchar(text[i]))) {
      i++;
    }
    if (i >= text.size()) {
      r_error = "GLSL meta attribute is missing a value";
      return false;
    }

    const int64_t value_start = i;
    int paren_depth = 0;
    while (i < text.size()) {
      const char c = text[i];
      if (c == '(') {
        paren_depth++;
      }
      else if (c == ')') {
        paren_depth = std::max(paren_depth - 1, 0);
      }
      else if (paren_depth == 0 && std::isspace(uchar(c))) {
        break;
      }
      i++;
    }

    const std::string value = trim_copy(text.substr(value_start, i - value_start));
    if (value.empty()) {
      r_error = "GLSL meta attribute is missing a value";
      return false;
    }
    if (r_assignments.contains(key)) {
      r_error = "Duplicate GLSL meta attribute '" + key + "'";
      return false;
    }
    r_assignments.add(key, value);
  }

  return true;
}

static bool merge_glsl_raw_param_meta(GLSLRawParamMeta &r_meta,
                                      const Map<std::string, std::string> &assignments,
                                      std::string &r_error)
{
  auto assign_once = [&](std::optional<std::string> &slot,
                         const StringRef key,
                         const StringRef value) -> bool {
    if (slot.has_value()) {
      r_error = "Duplicate GLSL meta attribute '" + std::string(key) + "'";
      return false;
    }
    slot = std::string(value);
    return true;
  };

  for (const auto &item : assignments.items()) {
    const StringRef key = item.key;
    const StringRef value = item.value;
    if (key == "default") {
      if (!assign_once(r_meta.default_value, key, value)) {
        return false;
      }
    }
    else if (key == "min") {
      if (!assign_once(r_meta.min_value, key, value)) {
        return false;
      }
    }
    else if (key == "max") {
      if (!assign_once(r_meta.max_value, key, value)) {
        return false;
      }
    }
    else if (key == "hide_value") {
      if (!assign_once(r_meta.hide_value, key, value)) {
        return false;
      }
    }
    else if (key == "subtype") {
      if (!assign_once(r_meta.subtype, key, value)) {
        return false;
      }
    }
    else {
      r_error = "Unsupported GLSL meta attribute '" + std::string(key) + "'";
      return false;
    }
  }

  return true;
}

static bool parse_glsl_meta_block(const StringRef comment,
                                  Map<std::string, GLSLRawParamMeta> &r_param_meta_by_name,
                                  bool &r_is_meta_block,
                                  std::string &r_error)
{
  r_is_meta_block = false;
  std::stringstream stream{std::string(comment)};
  std::string line;
  bool header_seen = false;

  while (std::getline(stream, line)) {
    std::string normalized = trim_copy(line);
    if (!normalized.empty() && normalized[0] == '*') {
      normalized = trim_copy(StringRef(normalized).drop_prefix(1));
    }
    if (normalized.empty()) {
      continue;
    }

    if (!header_seen) {
      if (!StringRef(normalized).startswith("@glsl_meta")) {
        return true;
      }
      header_seen = true;
      r_is_meta_block = true;
      continue;
    }

    if (StringRef(normalized).startswith("function ")) {
      r_error =
          "GLSL meta blocks now belong to the function directly below them; remove the "
          "'function ...' line";
      return false;
    }

    const int64_t separator = StringRef(normalized).find(':');
    if (separator == StringRef::not_found) {
      r_error = "GLSL meta lines must use 'name: key=value' syntax";
      return false;
    }

    std::string target = trim_copy(StringRef(normalized).substr(0, separator));
    const std::string attributes_text = trim_copy(StringRef(normalized).substr(separator + 1));
    if (target.empty() || attributes_text.empty()) {
      r_error = "GLSL meta lines must define a target and at least one attribute";
      return false;
    }

    const int64_t dot_index = StringRef(target).find('.');
    if (dot_index != StringRef::not_found) {
      r_error =
          "GLSL meta blocks now belong to the function directly below them; use plain parameter "
          "names inside the block";
      return false;
    }
    const std::string param_name = target;
    if (param_name.empty()) {
      r_error = "GLSL meta parameter target cannot be empty";
      return false;
    }

    Map<std::string, std::string> assignments;
    if (!parse_glsl_meta_assignment_list(attributes_text, assignments, r_error)) {
      return false;
    }

    GLSLRawParamMeta meta;
    if (const GLSLRawParamMeta *existing = r_param_meta_by_name.lookup_ptr(param_name))
    {
      meta = *existing;
    }

    if (!merge_glsl_raw_param_meta(meta, assignments, r_error)) {
      return false;
    }

    r_param_meta_by_name.add_overwrite(param_name, meta);
  }

  return true;
}

static bool find_glsl_meta_target_function_name(const StringRef source_after_comment,
                                                std::string &r_function_name,
                                                std::string &r_error)
{
  const std::string stripped_source = strip_glsl_comments(source_after_comment);
  const Vector<GLSLToken> tokens = tokenize_glsl_source(stripped_source);
  if (tokens.is_empty()) {
    r_error = "GLSL meta block must be placed directly above a function definition";
    return false;
  }

  int brace_depth = 0;
  for (int i = 0; i < tokens.size(); i++) {
    const GLSLToken &token = tokens[i];
    if (token.kind != GLSLToken::Kind::Punctuation) {
      continue;
    }

    if (token.punctuation == '{') {
      if (brace_depth == 0) {
        r_error = "GLSL meta block must be placed directly above a function definition";
        return false;
      }
      brace_depth++;
      continue;
    }
    if (token.punctuation == '}') {
      brace_depth = max_ii(0, brace_depth - 1);
      continue;
    }
    if (brace_depth != 0) {
      continue;
    }
    if (token.punctuation == ';') {
      r_error = "GLSL meta block must be placed directly above a function definition";
      return false;
    }
    if (token.punctuation != '(' || i < 2 || tokens[i - 1].kind != GLSLToken::Kind::Identifier ||
        tokens[i - 2].kind != GLSLToken::Kind::Identifier)
    {
      continue;
    }

    int paren_depth = 1;
    int closing_paren_index = -1;
    for (int j = i + 1; j < tokens.size(); j++) {
      if (tokens[j].kind != GLSLToken::Kind::Punctuation) {
        continue;
      }
      if (tokens[j].punctuation == '(') {
        paren_depth++;
      }
      else if (tokens[j].punctuation == ')') {
        paren_depth--;
        if (paren_depth == 0) {
          closing_paren_index = j;
          break;
        }
      }
    }

    if (closing_paren_index != -1 && (closing_paren_index + 1) < tokens.size() &&
        tokens[closing_paren_index + 1].kind == GLSLToken::Kind::Punctuation &&
        tokens[closing_paren_index + 1].punctuation == '{')
    {
      r_function_name = tokens[i - 1].text;
      return true;
    }

    r_error = "GLSL meta block must be placed directly above a function definition";
    return false;
  }

  r_error = "GLSL meta block must be placed directly above a function definition";
  return false;
}

static bool extract_glsl_meta(const StringRef source,
                              Map<std::string, GLSLRawParamMeta> &r_meta_by_key,
                              std::string &r_error)
{
  Set<std::string> functions_with_meta;
  for (int64_t i = 0; (i + 1) < source.size();) {
    if (source[i] == '/' && source[i + 1] == '*') {
      const int64_t body_start = i + 2;
      int64_t body_end = source.size();
      bool found_end = false;
      for (int64_t j = body_start; (j + 1) < source.size(); j++) {
        if (source[j] == '*' && source[j + 1] == '/') {
          body_end = j;
          i = j + 2;
          found_end = true;
          break;
        }
      }
      if (!found_end) {
        r_error = "Unterminated GLSL block comment";
        return false;
      }

      Map<std::string, GLSLRawParamMeta> param_meta_by_name;
      bool is_meta_block = false;
      if (!parse_glsl_meta_block(source.substr(body_start, body_end - body_start),
                                 param_meta_by_name,
                                 is_meta_block,
                                 r_error))
      {
        return false;
      }
      if (!is_meta_block) {
        continue;
      }

      std::string function_name;
      if (!find_glsl_meta_target_function_name(source.substr(i), function_name, r_error)) {
        return false;
      }
      if (functions_with_meta.contains(function_name)) {
        r_error = "Only one GLSL meta block is supported per function";
        return false;
      }
      functions_with_meta.add(function_name);

      for (const auto &item : param_meta_by_name.items()) {
        r_meta_by_key.add(make_glsl_meta_key(function_name, item.key), item.value);
      }
      continue;
    }
    i++;
  }

  return true;
}

static bool parse_glsl_meta_float_literal(const StringRef text,
                                          float &r_value,
                                          std::string &r_error)
{
  const std::string trimmed = trim_copy(text);
  if (trimmed.empty()) {
    r_error = "GLSL meta float value cannot be empty";
    return false;
  }

  char *end = nullptr;
  const float value = std::strtof(trimmed.c_str(), &end);
  if (end == trimmed.c_str() || trim_copy(end).size() != 0) {
    r_error = "Could not parse GLSL meta float value '" + trimmed + "'";
    return false;
  }
  r_value = value;
  return true;
}

static bool parse_glsl_meta_vector_default(const StringRef text,
                                           const int dimensions,
                                           float4 &r_value,
                                           std::string &r_error)
{
  const std::string trimmed = trim_copy(text);
  const std::string prefix = "vec" + std::to_string(dimensions);
  if (!StringRef(trimmed).startswith(prefix) || !StringRef(trimmed).endswith(")")) {
    r_error = "GLSL meta vector defaults must use " + prefix + "(...)";
    return false;
  }

  const StringRef args_text = StringRef(trimmed).substr(prefix.size());
  if (args_text.size() < 2 || args_text[0] != '(' || args_text[args_text.size() - 1] != ')') {
    r_error = "Malformed GLSL meta vector constructor";
    return false;
  }

  Vector<std::string> args;
  int paren_depth = 0;
  int64_t arg_start = 1;
  for (int64_t i = 1; i < args_text.size() - 1; i++) {
    const char c = args_text[i];
    if (c == '(') {
      paren_depth++;
    }
    else if (c == ')') {
      paren_depth = std::max(paren_depth - 1, 0);
    }
    else if (c == ',' && paren_depth == 0) {
      args.append(trim_copy(args_text.substr(arg_start, i - arg_start)));
      arg_start = i + 1;
    }
  }
  args.append(trim_copy(args_text.substr(arg_start, args_text.size() - 1 - arg_start)));

  if (!(args.size() == 1 || args.size() == dimensions)) {
    r_error = "GLSL meta vector defaults must provide either one scalar or " +
              std::to_string(dimensions) + " scalars";
    return false;
  }

  r_value = float4(0.0f);
  if (args.size() == 1) {
    float scalar = 0.0f;
    if (!parse_glsl_meta_float_literal(args[0], scalar, r_error)) {
      return false;
    }
    for (const int i : IndexRange(dimensions)) {
      r_value[i] = scalar;
    }
    return true;
  }

  for (const int i : IndexRange(dimensions)) {
    if (!parse_glsl_meta_float_literal(args[i], r_value[i], r_error)) {
      return false;
    }
  }
  return true;
}

static bool parse_glsl_meta_subtype(const StringRef text,
                                    const GLSLBoundaryType type,
                                    PropertySubType &r_subtype,
                                    std::string &r_error)
{
  std::string name = lowercase_copy(trim_copy(text));
  if (StringRef(name).startswith("prop_")) {
    name = name.substr(5);
  }

  if (type == GLSLBoundaryType::Float) {
    if (name == "none") {
      r_subtype = PROP_NONE;
    }
    else if (name == "unsigned") {
      r_subtype = PROP_UNSIGNED;
    }
    else if (name == "percentage") {
      r_subtype = PROP_PERCENTAGE;
    }
    else if (name == "factor") {
      r_subtype = PROP_FACTOR;
    }
    else if (name == "mass") {
      r_subtype = PROP_MASS;
    }
    else if (name == "angle") {
      r_subtype = PROP_ANGLE;
    }
    else if (name == "time") {
      r_subtype = PROP_TIME;
    }
    else if (name == "time_absolute") {
      r_subtype = PROP_TIME_ABSOLUTE;
    }
    else if (name == "distance") {
      r_subtype = PROP_DISTANCE;
    }
    else if (name == "wavelength") {
      r_subtype = PROP_WAVELENGTH;
    }
    else {
      r_error = "Unsupported GLSL meta float subtype '" + std::string(text) + "'";
      return false;
    }
    return true;
  }

  if (!ELEM(type, GLSLBoundaryType::Vec2, GLSLBoundaryType::Vec3, GLSLBoundaryType::Vec4)) {
    r_error = "GLSL meta subtype is only supported for float and vec* inputs";
    return false;
  }

  if (name == "none") {
    r_subtype = PROP_NONE;
  }
  else if (name == "factor") {
    r_subtype = PROP_FACTOR;
  }
  else if (name == "percentage") {
    r_subtype = PROP_PERCENTAGE;
  }
  else if (name == "translation") {
    r_subtype = PROP_TRANSLATION;
  }
  else if (name == "direction") {
    r_subtype = PROP_DIRECTION;
  }
  else if (name == "velocity") {
    r_subtype = PROP_VELOCITY;
  }
  else if (name == "acceleration") {
    r_subtype = PROP_ACCELERATION;
  }
  else if (name == "euler") {
    r_subtype = PROP_EULER;
  }
  else if (name == "xyz") {
    r_subtype = PROP_XYZ;
  }
  else {
    r_error = "Unsupported GLSL meta vector subtype '" + std::string(text) + "'";
    return false;
  }
  return true;
}

static bool apply_glsl_meta_to_param(const GLSLRawParamMeta &raw_meta,
                                     GLSLFunctionParam &r_param,
                                     std::string &r_error)
{
  if (!raw_meta.has_any()) {
    return true;
  }
  if (!glsl_param_has_input_socket(r_param)) {
    r_error = "GLSL meta only supports input parameters";
    return false;
  }
  if (glsl_boundary_type_is_sampler(r_param.type)) {
    r_error = "GLSL meta does not support sampler2D parameters yet";
    return false;
  }

  if (raw_meta.default_value.has_value()) {
    if (r_param.type == GLSLBoundaryType::Float) {
      if (!parse_glsl_meta_float_literal(*raw_meta.default_value,
                                         r_param.meta.default_value.x,
                                         r_error))
      {
        return false;
      }
    }
    else {
      if (!parse_glsl_meta_vector_default(
              *raw_meta.default_value, r_param.dimensions, r_param.meta.default_value, r_error))
      {
        return false;
      }
    }
    r_param.meta.has_default_value = true;
  }

  if (raw_meta.min_value.has_value()) {
    if (!parse_glsl_meta_float_literal(*raw_meta.min_value, r_param.meta.min_value, r_error)) {
      return false;
    }
    r_param.meta.has_min = true;
  }

  if (raw_meta.max_value.has_value()) {
    if (!parse_glsl_meta_float_literal(*raw_meta.max_value, r_param.meta.max_value, r_error)) {
      return false;
    }
    r_param.meta.has_max = true;
  }

  if (raw_meta.hide_value.has_value()) {
    if (!parse_glsl_meta_bool_literal(*raw_meta.hide_value, r_param.meta.hide_value, r_error)) {
      return false;
    }
  }

  if (r_param.meta.has_min && r_param.meta.has_max &&
      r_param.meta.min_value > r_param.meta.max_value)
  {
    r_error = "GLSL meta min cannot be greater than max";
    return false;
  }

  if (raw_meta.subtype.has_value()) {
    PropertySubType subtype = PROP_NONE;
    if (!parse_glsl_meta_subtype(*raw_meta.subtype, r_param.type, subtype, r_error)) {
      return false;
    }
    r_param.meta.subtype = subtype;
  }

  return true;
}

static std::string build_glsl_meta_signature_key(const GLSLFunctionDefinition &function)
{
  std::stringstream ss;
  for (const GLSLFunctionParam &param : function.params) {
    if (!param.meta.has_any()) {
      continue;
    }
    ss << param.name << '{';
    if (param.meta.has_default_value) {
      ss << "default=";
      for (const int i : IndexRange(std::max(param.dimensions, 1))) {
        if (i != 0) {
          ss << ',';
        }
        ss << param.meta.default_value[i];
      }
      ss << ';';
    }
    if (param.meta.has_min) {
      ss << "min=" << param.meta.min_value << ';';
    }
    if (param.meta.has_max) {
      ss << "max=" << param.meta.max_value << ';';
    }
    if (param.meta.hide_value) {
      ss << "hide_value=1;";
    }
    if (param.meta.subtype.has_value()) {
      ss << "subtype=" << int(*param.meta.subtype) << ';';
    }
    ss << '}';
  }
  return ss.str();
}

static bool apply_glsl_meta_to_function(const Map<std::string, GLSLRawParamMeta> &meta_by_key,
                                        GLSLFunctionDefinition &r_function,
                                        int &r_meta_hash,
                                        std::string &r_error)
{
  Set<std::string> param_names;
  for (const GLSLFunctionParam &param : r_function.params) {
    param_names.add(param.name);
  }

  for (const auto &item : meta_by_key.items()) {
    StringRef target_function;
    StringRef target_param;
    split_glsl_meta_key(item.key, target_function, target_param);
    if (!target_function.is_empty() && target_function != r_function.name) {
      continue;
    }
    if (!param_names.contains(std::string(target_param))) {
      r_error = "GLSL meta parameter '" + std::string(target_param) +
                "' was not found in function '" + r_function.name + "'";
      return false;
    }
  }

  for (GLSLFunctionParam &param : r_function.params) {
    if (const GLSLRawParamMeta *function_meta = meta_by_key.lookup_ptr(
            make_glsl_meta_key(r_function.name, param.name)))
    {
      if (!apply_glsl_meta_to_param(*function_meta, param, r_error)) {
        if (!r_error.empty()) {
          r_error = "For parameter '" + param.name + "': " + r_error;
        }
        return false;
      }
    }
  }

  const std::string meta_signature = build_glsl_meta_signature_key(r_function);
  r_meta_hash = meta_signature.empty() ? 0 : int(BLI_ghashutil_strhash_p(meta_signature.c_str()));
  return true;
}

static Vector<GLSLToken> tokenize_glsl_source(const StringRef source)
{
  Vector<GLSLToken> tokens;
  bool beginning_of_line = true;

  for (int64_t i = 0; i < source.size();) {
    const char c = source[i];

    if (c == '\n') {
      beginning_of_line = true;
      i++;
      continue;
    }
    if (std::isspace(uchar(c))) {
      i++;
      continue;
    }
    if (beginning_of_line && c == '#') {
      while (i < source.size() && source[i] != '\n') {
        i++;
      }
      continue;
    }

    beginning_of_line = false;

    if (is_identifier_start(c)) {
      const int64_t start = i;
      i++;
      while (i < source.size() && is_identifier_continue(source[i])) {
        i++;
      }
      tokens.append(
          {GLSLToken::Kind::Identifier, std::string(source.substr(start, i - start)), '\0', start, i});
      continue;
    }

    if (strchr("(){}[],;=", c) != nullptr) {
      tokens.append({GLSLToken::Kind::Punctuation, std::string(1, c), c, i, i + 1});
    }
    i++;
  }

  return tokens;
}

static bool parse_glsl_parameter_tokens(const Span<GLSLToken> tokens,
                                        GLSLFunctionParam &r_param,
                                        std::string &r_error)
{
  if (tokens.is_empty()) {
    r_error = "Empty parameter declaration";
    return false;
  }

  Vector<StringRef> identifiers;
  bool has_out_qualifier = false;
  bool has_inout_qualifier = false;
  bool has_unsupported_punctuation = false;

  for (const GLSLToken &token : tokens) {
    if (token.kind == GLSLToken::Kind::Identifier) {
      identifiers.append(token.text);
      has_out_qualifier |= token.text == "out";
      has_inout_qualifier |= token.text == "inout";
    }
    else if (!ELEM(token.punctuation, '[', ']')) {
      has_unsupported_punctuation = true;
    }
  }

  if (has_out_qualifier && has_inout_qualifier) {
    r_error = "A parameter cannot be both 'out' and 'inout'";
    return false;
  }
  if (has_unsupported_punctuation) {
    r_error = "Unsupported GLSL parameter syntax";
    return false;
  }
  if (identifiers.size() == 1 && identifiers[0] == "void") {
    r_param = {};
    return true;
  }
  if (identifiers.size() < 2) {
    r_error = "Each parameter needs a type and a name";
    return false;
  }

  const StringRef type_name = identifiers[identifiers.size() - 2];
  const StringRef param_name = identifiers.last();
  const GLSLToken &type_token = tokens[tokens.size() - 2];
  const GLSLBoundaryType boundary_type = glsl_boundary_type_from_name(type_name);
  if (!ELEM(boundary_type,
            GLSLBoundaryType::Float,
            GLSLBoundaryType::Vec2,
            GLSLBoundaryType::Vec3,
            GLSLBoundaryType::Vec4,
            GLSLBoundaryType::Sampler2D,
            GLSLBoundaryType::Sample2D))
  {
    r_error = "Supported parameter types are float, vec2, vec3, vec4, sampler2D, and sample2D";
    return false;
  }

  r_param.type = boundary_type;
  if (has_inout_qualifier) {
    r_error = "The 'inout' qualifier is not supported yet";
    return false;
  }
  if (has_out_qualifier && glsl_boundary_type_is_sampler(boundary_type)) {
    r_error = "sampler2D parameters only support input qualifiers";
    return false;
  }
  r_param.qualifier = has_out_qualifier ? GLSLFunctionParam::Qualifier::Out :
                                          GLSLFunctionParam::Qualifier::In;
  r_param.type_name = type_name;
  r_param.name = std::string(param_name);
  r_param.identifier = make_socket_identifier(has_out_qualifier ? "Out" : "In", param_name);
  r_param.dimensions = glsl_boundary_dimensions(boundary_type);
  r_param.type_source_start = type_token.source_start;
  r_param.type_source_end = type_token.source_end;
  return true;
}

static bool parse_glsl_function_definition(const Vector<GLSLToken> &tokens,
                                           const int paren_index,
                                           const int closing_paren_index,
                                           GLSLFunctionDefinition &r_function,
                                           std::string &r_error)
{
  if (paren_index < 2 || tokens[paren_index].punctuation != '(' ||
      tokens[closing_paren_index].punctuation != ')')
  {
    r_error = "Malformed GLSL function declaration";
    return false;
  }

  const GLSLToken &name_token = tokens[paren_index - 1];
  const GLSLToken &type_token = tokens[paren_index - 2];
  if (name_token.kind != GLSLToken::Kind::Identifier ||
      type_token.kind != GLSLToken::Kind::Identifier)
  {
    r_error = "Could not resolve function name and return type";
    return false;
  }

  r_function.name = name_token.text;
  r_function.return_type_name = type_token.text;
  r_function.return_type = glsl_boundary_type_from_name(type_token.text);
  if (!ELEM(r_function.return_type,
            GLSLBoundaryType::Void,
            GLSLBoundaryType::Float,
            GLSLBoundaryType::Vec2,
            GLSLBoundaryType::Vec3,
            GLSLBoundaryType::Vec4))
  {
    r_error = "Supported return types are void, float, vec2, vec3, and vec4";
    return false;
  }

  Vector<GLSLToken> parameter_tokens;
  int parameter_depth = 0;
  for (int i = paren_index + 1; i < closing_paren_index; i++) {
    const GLSLToken &token = tokens[i];
    if (token.kind == GLSLToken::Kind::Punctuation && token.punctuation == ',' &&
        parameter_depth == 0)
    {
      GLSLFunctionParam parameter;
      if (!parse_glsl_parameter_tokens(parameter_tokens, parameter, r_error)) {
        return false;
      }
      if (parameter.type != GLSLBoundaryType::Unsupported) {
        r_function.params.append(parameter);
      }
      parameter_tokens.clear();
      continue;
    }
    if (token.kind == GLSLToken::Kind::Punctuation) {
      if (token.punctuation == '(') {
        parameter_depth++;
      }
      else if (token.punctuation == ')') {
        parameter_depth--;
      }
    }
    parameter_tokens.append(token);
  }

  if (!parameter_tokens.is_empty()) {
    GLSLFunctionParam parameter;
    if (!parse_glsl_parameter_tokens(parameter_tokens, parameter, r_error)) {
      return false;
    }
    if (parameter.type != GLSLBoundaryType::Unsupported) {
      r_function.params.append(parameter);
    }
  }

  if (r_function.return_type == GLSLBoundaryType::Void && glsl_function_output_count(r_function) == 0) {
    r_error = "The selected function does not expose any node outputs";
    return false;
  }

  const int opening_brace_index = closing_paren_index + 1;
  if (opening_brace_index >= tokens.size() || tokens[opening_brace_index].kind != GLSLToken::Kind::Punctuation ||
      tokens[opening_brace_index].punctuation != '{')
  {
    r_error = "Could not resolve the GLSL function body";
    return false;
  }

  int brace_depth = 1;
  int closing_brace_index = -1;
  for (int i = opening_brace_index + 1; i < tokens.size(); i++) {
    const GLSLToken &token = tokens[i];
    if (token.kind != GLSLToken::Kind::Punctuation) {
      continue;
    }
    if (token.punctuation == '{') {
      brace_depth++;
    }
    else if (token.punctuation == '}') {
      brace_depth--;
      if (brace_depth == 0) {
        closing_brace_index = i;
        break;
      }
    }
  }
  if (closing_brace_index == -1) {
    r_error = "Could not resolve the GLSL function body";
    return false;
  }
  r_function.body_token_start = opening_brace_index + 1;
  r_function.body_token_end = closing_brace_index - 1;

  return true;
}

static Vector<std::string> find_top_level_glsl_function_names(const Vector<GLSLToken> &tokens)
{
  Vector<std::string> names;
  int brace_depth = 0;

  brace_depth = 0;
  for (int i = 0; i < tokens.size(); i++) {
    const GLSLToken &token = tokens[i];
    if (token.kind == GLSLToken::Kind::Punctuation) {
      if (token.punctuation == '{') {
        brace_depth++;
      }
      else if (token.punctuation == '}') {
        brace_depth = max_ii(0, brace_depth - 1);
      }
      else if (token.punctuation == '(' && brace_depth == 0 && i >= 2 &&
               tokens[i - 1].kind == GLSLToken::Kind::Identifier &&
               tokens[i - 2].kind == GLSLToken::Kind::Identifier)
      {
        int paren_depth = 1;
        int closing_paren_index = -1;
        for (int j = i + 1; j < tokens.size(); j++) {
          if (tokens[j].kind == GLSLToken::Kind::Punctuation) {
            if (tokens[j].punctuation == '(') {
              paren_depth++;
            }
            else if (tokens[j].punctuation == ')') {
              paren_depth--;
              if (paren_depth == 0) {
                closing_paren_index = j;
                break;
              }
            }
          }
        }
        if (closing_paren_index == -1) {
          continue;
        }
        const int next_index = closing_paren_index + 1;
        if (next_index < tokens.size() && tokens[next_index].kind == GLSLToken::Kind::Punctuation &&
            tokens[next_index].punctuation == '{')
        {
          names.append(tokens[i - 1].text);
        }
      }
    }
  }
  return names;
}

static bool find_glsl_function_definition(const Vector<GLSLToken> &tokens,
                                          const StringRef function_name,
                                          GLSLFunctionDefinition &r_function,
                                          std::string &r_error)
{
  int brace_depth = 0;
  bool found_first_function = false;

  for (int i = 0; i < tokens.size(); i++) {
    const GLSLToken &token = tokens[i];

    if (token.kind == GLSLToken::Kind::Punctuation && token.punctuation == '{') {
      brace_depth++;
      continue;
    }
    if (token.kind == GLSLToken::Kind::Punctuation && token.punctuation == '}') {
      brace_depth = max_ii(0, brace_depth - 1);
      continue;
    }

    if (brace_depth != 0 || token.kind != GLSLToken::Kind::Punctuation || token.punctuation != '(' ||
        i < 2 || tokens[i - 1].kind != GLSLToken::Kind::Identifier ||
        tokens[i - 2].kind != GLSLToken::Kind::Identifier)
    {
      continue;
    }

    int paren_depth = 1;
    int closing_paren_index = -1;
    for (int j = i + 1; j < tokens.size(); j++) {
      if (tokens[j].kind == GLSLToken::Kind::Punctuation) {
        if (tokens[j].punctuation == '(') {
          paren_depth++;
        }
        else if (tokens[j].punctuation == ')') {
          paren_depth--;
          if (paren_depth == 0) {
            closing_paren_index = j;
            break;
          }
        }
      }
    }

    if (closing_paren_index == -1 || (closing_paren_index + 1) >= tokens.size() ||
        tokens[closing_paren_index + 1].kind != GLSLToken::Kind::Punctuation ||
        tokens[closing_paren_index + 1].punctuation != '{')
    {
      continue;
    }

    const StringRef candidate_name = tokens[i - 1].text;
    if (!function_name.is_empty() && candidate_name != function_name) {
      found_first_function = true;
      continue;
    }

    if (!parse_glsl_function_definition(tokens, i, closing_paren_index, r_function, r_error)) {
      return false;
    }
    return true;
  }

  if (function_name.is_empty()) {
    r_error = found_first_function ? "Could not parse the first GLSL function definition" :
                                     "No GLSL function definition was found";
  }
  else {
    r_error = "The selected function was not found in the source";
  }
  return false;
}

static std::string build_function_signature_key(const GLSLFunctionDefinition &function)
{
  std::stringstream ss;
  ss << function.return_type_name << ' ' << function.name << '(';
  for (const int index : function.params.index_range()) {
    const GLSLFunctionParam &param = function.params[index];
    if (index != 0) {
      ss << ", ";
    }
    switch (param.qualifier) {
      case GLSLFunctionParam::Qualifier::In:
        ss << "in ";
        break;
      case GLSLFunctionParam::Qualifier::Out:
        ss << "out ";
        break;
      case GLSLFunctionParam::Qualifier::InOut:
        ss << "inout ";
        break;
    }
    ss << param.type_name << ' ' << param.name;
  }
  ss << ')';
  return ss.str();
}

static bool load_glsl_source(const bNode &node, std::string &r_source, std::string &r_error)
{
  const NodeShaderGLSLFunction &storage = node_storage(node);

  if (storage.source_mode == SHD_GLSL_FUNCTION_SOURCE_INTERNAL) {
    Text *text = reinterpret_cast<Text *>(node.id);
    if (text == nullptr) {
      r_error = "Choose a Text datablock to use as the GLSL source";
      return false;
    }
    size_t buffer_len = 0;
    char *buffer = txt_to_buf(text, &buffer_len);
    BLI_SCOPED_DEFER([&]() { MEM_delete(buffer); });
    r_source.assign(buffer, buffer_len);
    return true;
  }

  if (storage.filepath[0] == '\0') {
    r_error = "Choose a GLSL file path";
    return false;
  }

  char absolute_path[FILE_MAX];
  STRNCPY(absolute_path, storage.filepath);
  if (!BLI_path_is_abs_from_cwd(absolute_path) && node.runtime->owner_tree != nullptr) {
    BLI_path_abs(absolute_path, ID_BLEND_PATH_FROM_GLOBAL(&node.runtime->owner_tree->id));
  }

  size_t buffer_len = 0;
  char *buffer = BLI_file_read_text_as_mem(absolute_path, 0, &buffer_len);
  if (buffer == nullptr) {
    r_error = "Could not open the GLSL file";
    return false;
  }
  BLI_SCOPED_DEFER([&]() { MEM_delete(buffer); });
  r_source.assign(buffer, buffer_len);
  return true;
}

static const bNodeSocket *find_closure_output_socket_by_name(const bNode &node, const StringRef name)
{
  if (!node.is_type("NodeClosureOutput")) {
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

static const bNode *find_localized_copy_of_original_node(const bNodeTree &tree, const bNode &original_node)
{
  for (const bNode *node : tree.nodes_by_type("NodeClosureOutput")) {
    if (node->runtime->original == &original_node) {
      return node;
    }
  }
  return nullptr;
}

static bool closure_output_has_required_sample2d_signature(const bNode &closure_output_node,
                                                           std::string &r_error)
{
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
    r_error = "Closure Output must expose a Vector input item named 'UV' for sample2D";
    return false;
  }

  const bNodeSocket *color_socket = find_closure_output_socket_by_name(closure_output_node, "Color");
  if (color_socket == nullptr || color_socket->type != SOCK_RGBA) {
    r_error = "Closure Output must expose an RGBA output item named 'Color' for sample2D";
    return false;
  }
  return true;
}

static void mark_node_upstream_for_closure_helper(const bNode &node, Set<const bNode *> &r_visited_nodes);

static void mark_socket_upstream_for_closure_helper(const bNodeSocket &socket,
                                                    Set<const bNode *> &r_visited_nodes)
{
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (!link->is_used() || link->fromnode == nullptr) {
      continue;
    }
    mark_node_upstream_for_closure_helper(*link->fromnode, r_visited_nodes);
  }
}

static void mark_node_upstream_for_closure_helper(const bNode &node, Set<const bNode *> &r_visited_nodes)
{
  if (!r_visited_nodes.add(&node)) {
    return;
  }
  const_cast<bNode &>(node).runtime->need_exec = 1;
  for (const bNodeSocket *input_socket : node.input_sockets()) {
    mark_socket_upstream_for_closure_helper(*input_socket, r_visited_nodes);
  }
}

static bool build_closure_sample_helper(GPUMaterial *mat,
                                        const bNode &node,
                                        const GLSLFunctionParam &param,
                                        const StringRef helper_name,
                                        const StringRef uv_global_name,
                                        GLSLClosureSampleHelper &r_helper,
                                        std::string &r_error)
{
  const bNodeLink *used_link = nullptr;
  if (resolve_sample2d_source_kind(node, param, used_link) != GLSLSample2DSourceKind::ClosureOutput ||
      used_link == nullptr || used_link->fromnode == nullptr)
  {
    r_error = "sample2D parameter '" + param.name + "' is missing a Closure Output source";
    return false;
  }

  const bNode *closure_output_node = used_link->fromnode;
  if (&closure_output_node->owner_tree() != &node.owner_tree()) {
    if (const bNode *localized_node = find_localized_copy_of_original_node(node.owner_tree(),
                                                                           *closure_output_node))
    {
      closure_output_node = localized_node;
    }
  }

  std::string closure_error;
  if (!closure_output_has_required_sample2d_signature(*closure_output_node, closure_error)) {
    r_error = "Closure Output connected to '" + param.name + "' is missing required closure items: " +
              closure_error;
    return false;
  }
  const bNodeSocket *color_socket = find_closure_output_socket_by_name(*closure_output_node, "Color");

  bNodeExecContext context = {};
  bNodeTree *helper_tree = const_cast<bNodeTree *>(&closure_output_node->owner_tree());
  bNodeTreeExec *exec = ntreeShaderBeginExecTree_internal(&context, helper_tree, bke::NODE_INSTANCE_KEY_BASE);
  if (exec == nullptr) {
    r_error = "Could not build a helper shader tree for sample2D parameter '" + param.name + "'";
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
  mark_socket_upstream_for_closure_helper(*color_socket, visited_nodes);

  GPU_material_closure_uv_source_push(mat, StringRefNull(uv_global_name));
  BLI_SCOPED_DEFER([&]() { GPU_material_closure_uv_source_pop(mat); });

  ntreeExecGPUNodes(exec, mat, nullptr);

  bNodeStack *color_stack = node_get_socket_stack(exec->stack, const_cast<bNodeSocket *>(color_socket));
  GPUNodeLink *color_link = (color_stack != nullptr) ? static_cast<GPUNodeLink *>(color_stack->data) :
                                                       nullptr;
  if (color_link == nullptr && color_stack != nullptr) {
    color_link = GPU_constant(color_stack->vec);
  }
  if (color_link == nullptr) {
    r_error = "Could not evaluate Closure Output Color for sample2D parameter '" + param.name + "'";
    return false;
  }

  const char *sub_function_name = GPU_material_split_sub_function(mat, GPU_VEC4, &color_link);
  r_helper.param_name = param.name;
  r_helper.helper_name = helper_name;
  r_helper.uv_global_name = uv_global_name;
  r_helper.sub_function_name = sub_function_name;
  return true;
}

static std::string trim_source_range(const StringRef source, const int64_t start, const int64_t end)
{
  if (end <= start) {
    return "";
  }
  return trim_copy(source.substr(start, end - start));
}

static const GLSLClosureSampleHelper *find_closure_sample_helper(
    const Span<GLSLClosureSampleHelper> helpers, const StringRef param_name)
{
  for (const GLSLClosureSampleHelper &helper : helpers) {
    if (param_name == helper.param_name) {
      return &helper;
    }
  }
  return nullptr;
}

static bool rewrite_glsl_source_for_sample2d(const StringRef source,
                                             const Vector<GLSLToken> &tokens,
                                             const GLSLFunctionDefinition &function,
                                             const Map<std::string, GLSLSample2DSourceKind> &source_kinds,
                                             const Span<GLSLClosureSampleHelper> closure_helpers,
                                             std::string &r_rewritten_source,
                                             std::string &r_error)
{
  struct Replacement {
    int64_t start = 0;
    int64_t end = 0;
    std::string text;
  };

  Vector<Replacement> replacements;

  for (const GLSLFunctionParam &param : function.params) {
    if (!glsl_boundary_type_is_sample2d(param.type) || param.type_source_start < 0 ||
        param.type_source_end <= param.type_source_start)
    {
      continue;
    }
    const GLSLSample2DSourceKind *source_kind = source_kinds.lookup_ptr(param.name);
    if (source_kind != nullptr && *source_kind == GLSLSample2DSourceKind::ClosureOutput) {
      replacements.append({param.type_source_start, param.type_source_end, "float"});
    }
  }

  for (int i = function.body_token_start; i <= function.body_token_end; i++) {
    const GLSLToken &token = tokens[i];
    if (token.kind != GLSLToken::Kind::Identifier || token.text != "texture") {
      continue;
    }
    if ((i + 1) > function.body_token_end || tokens[i + 1].kind != GLSLToken::Kind::Punctuation ||
        tokens[i + 1].punctuation != '(')
    {
      continue;
    }

    int paren_depth = 1;
    int closing_paren_index = -1;
    Vector<int> comma_indices;
    for (int j = i + 2; j <= function.body_token_end; j++) {
      const GLSLToken &arg_token = tokens[j];
      if (arg_token.kind != GLSLToken::Kind::Punctuation) {
        continue;
      }
      if (arg_token.punctuation == '(') {
        paren_depth++;
      }
      else if (arg_token.punctuation == ')') {
        paren_depth--;
        if (paren_depth == 0) {
          closing_paren_index = j;
          break;
        }
      }
      else if (arg_token.punctuation == ',' && paren_depth == 1) {
        comma_indices.append(j);
      }
    }

    if (closing_paren_index == -1 || comma_indices.is_empty()) {
      continue;
    }

    const std::string first_argument = trim_source_range(
        source, tokens[i + 1].source_end, tokens[comma_indices[0]].source_start);
    const GLSLClosureSampleHelper *helper = find_closure_sample_helper(closure_helpers, first_argument);
    if (helper == nullptr) {
      continue;
    }
    if (comma_indices.size() != 1) {
      r_error = "sample2D parameter '" + helper->param_name +
                "' only supports texture(tex, uv) when driven by Closure Output";
      return false;
    }

    const std::string second_argument = trim_source_range(
        source, tokens[comma_indices[0]].source_end, tokens[closing_paren_index].source_start);
    replacements.append({token.source_start,
                         tokens[closing_paren_index].source_end,
                         helper->helper_name + "(" + second_argument + ")"});
  }

  r_rewritten_source = std::string(source);
  for (int i = replacements.size() - 1; i >= 0; i--) {
    const Replacement &replacement = replacements[i];
    r_rewritten_source.replace(
        replacement.start, replacement.end - replacement.start, replacement.text);
  }
  return true;
}

static std::string build_sample2d_closure_helper_block(
    const Span<GLSLClosureSampleHelper> closure_helpers)
{
  if (closure_helpers.is_empty()) {
    return "";
  }

  std::stringstream ss;
  for (const GLSLClosureSampleHelper &helper : closure_helpers) {
    ss << "vec4 " << helper.sub_function_name << "();\n";
  }
  ss << "\n";
  for (const GLSLClosureSampleHelper &helper : closure_helpers) {
    ss << "vec2 " << helper.uv_global_name << " = vec2(0.0);\n";
  }
  ss << "\n";
  for (const GLSLClosureSampleHelper &helper : closure_helpers) {
    ss << "vec4 " << helper.helper_name << "(vec2 uv)\n{\n";
    ss << "  " << helper.uv_global_name << " = uv;\n";
    ss << "  return " << helper.sub_function_name << "();\n";
    ss << "}\n\n";
  }
  return ss.str();
}

static std::string build_namespaced_glsl_source(const StringRef prefix,
                                                const Span<std::string> function_names,
                                                const StringRef source,
                                                const Span<GLSLClosureSampleHelper> closure_helpers)
{
  std::stringstream ss;
  ss << build_sample2d_closure_helper_block(closure_helpers);
  ss << "#define sample2D sampler2D\n";
  for (const std::string &name : function_names) {
    ss << "#define " << name << " " << prefix << name << "\n";
  }
  ss << "\n" << source;
  if (!source.is_empty() && source[source.size() - 1] != '\n') {
    ss << "\n";
  }
  for (const std::string &name : function_names) {
    ss << "#undef " << name << "\n";
  }
  ss << "#undef sample2D\n";
  return ss.str();
}

static StringRefNull emitted_type_name(const GLSLFunctionParam &param,
                                       const GLSLSample2DSourceKind sample2d_source_kind)
{
  if (param.type != GLSLBoundaryType::Sample2D) {
    return param.type_name;
  }
  return (sample2d_source_kind == GLSLSample2DSourceKind::ClosureOutput) ? StringRefNull("float") :
                                                                            StringRefNull("sampler2D");
}

static std::string build_wrapper_glsl_source(
    const GLSLParseResult &parse_result,
    const Map<std::string, GLSLSample2DSourceKind> &sample2d_source_kinds)
{
  const GLSLFunctionDefinition &function = parse_result.function;

  std::stringstream ss;
  ss << "void " << parse_result.wrapper_name << "(";
  bool need_comma = false;
  for (const GLSLFunctionParam &param : function.params) {
    if (!glsl_param_has_input_socket(param)) {
      continue;
    }
    if (need_comma) {
      ss << ", ";
    }
    const GLSLSample2DSourceKind source_kind = sample2d_source_kinds.lookup_ptr(param.name) ?
                                                   *sample2d_source_kinds.lookup_ptr(param.name) :
                                                   GLSLSample2DSourceKind::None;
    ss << emitted_type_name(param, source_kind) << " " << make_wrapper_argument_name("in", param.name);
    need_comma = true;
  }
  if (function.return_type != GLSLBoundaryType::Void) {
    if (need_comma) {
      ss << ", ";
    }
    ss << "out " << function.return_type_name << " out_result";
    need_comma = true;
  }
  for (const GLSLFunctionParam &param : function.params) {
    if (!glsl_param_has_output_socket(param)) {
      continue;
    }
    if (need_comma) {
      ss << ", ";
    }
    ss << "out " << param.type_name << " " << make_wrapper_argument_name("out", param.name);
    need_comma = true;
  }
  ss << ")\n{\n";
  ss << "  ";
  if (function.return_type != GLSLBoundaryType::Void) {
    ss << "out_result = ";
  }
  ss << parse_result.source_prefix << function.name << "(";
  need_comma = false;
  for (const GLSLFunctionParam &param : function.params) {
    if (need_comma) {
      ss << ", ";
    }
    if (glsl_param_has_output_socket(param)) {
      ss << make_wrapper_argument_name("out", param.name);
    }
    else {
      ss << make_wrapper_argument_name("in", param.name);
    }
    need_comma = true;
  }
  ss << ");\n}\n";
  return ss.str();
}

static bool build_specialized_glsl_sources(GPUMaterial *mat,
                                           const bNode &node,
                                           const GLSLParseResult &parse_result,
                                           std::string &r_library_source,
                                           std::string &r_wrapper_source,
                                           std::string &r_error)
{
  const std::string stripped_source = strip_glsl_comments(parse_result.source);
  const Vector<GLSLToken> tokens = tokenize_glsl_source(stripped_source);
  const Map<std::string, GLSLSample2DSourceKind> source_kinds = resolve_sample2d_source_kinds(
      node, parse_result.function);

  Vector<GLSLClosureSampleHelper> closure_helpers;
  for (const GLSLFunctionParam &param : parse_result.function.params) {
    const GLSLSample2DSourceKind *source_kind = source_kinds.lookup_ptr(param.name);
    if (source_kind == nullptr || *source_kind != GLSLSample2DSourceKind::ClosureOutput) {
      continue;
    }

    GLSLClosureSampleHelper helper;
    const std::string helper_name = "glsl_sample2d_" + std::to_string(node.identifier) + "_" +
                                    param.identifier;
    const std::string uv_global_name = helper_name + "_uv";
    if (!build_closure_sample_helper(
            mat, node, param, helper_name, uv_global_name, helper, r_error))
    {
      return false;
    }
    closure_helpers.append(helper);
  }

  std::string rewritten_source;
  if (!rewrite_glsl_source_for_sample2d(
          stripped_source, tokens, parse_result.function, source_kinds, closure_helpers, rewritten_source, r_error))
  {
    return false;
  }

  r_library_source = build_namespaced_glsl_source(
      parse_result.source_prefix, parse_result.function_names, rewritten_source, closure_helpers);
  r_wrapper_source = build_wrapper_glsl_source(parse_result, source_kinds);
  return true;
}

static GLSLParseResult parse_glsl_for_node(const bNode &node)
{
  GLSLParseResult result;
  result.keep_existing_sockets = true;

  std::string source;
  if (!load_glsl_source(node, source, result.error)) {
    return result;
  }
  if (trim_copy(source).empty()) {
    result.error = "The GLSL source is empty";
    return result;
  }

  Map<std::string, GLSLRawParamMeta> meta_by_key;
  if (!extract_glsl_meta(source, meta_by_key, result.error)) {
    return result;
  }

  const std::string stripped_source = strip_glsl_comments(source);
  const Vector<GLSLToken> tokens = tokenize_glsl_source(stripped_source);
  result.function_names = find_top_level_glsl_function_names(tokens);
  if (result.function_names.is_empty()) {
    result.error = "No top-level GLSL function definition was found";
    return result;
  }

  const NodeShaderGLSLFunction &storage = node_storage(node);
  const StringRef requested_function_name = storage.function_name;
  if (requested_function_name.is_empty()) {
    return result;
  }
  if (!find_glsl_function_definition(tokens, requested_function_name, result.function, result.error)) {
    return result;
  }
  if (!apply_glsl_meta_to_function(meta_by_key, result.function, result.meta_hash, result.error)) {
    return result;
  }
  if (!validate_sampler_inputs(node, tokens, result.function, result.error)) {
    return result;
  }

  result.ok = true;
  result.source = source;
  result.resolved_function_name = result.function.name;
  result.used_first_function = false;

  const std::string signature_key = build_function_signature_key(result.function);
  const uint signature_hash = BLI_ghashutil_strhash_p(signature_key.c_str());
  result.signature_hash = int(signature_hash);

  const uint source_hash = BLI_ghashutil_strhash_p(source.c_str());
  char source_hash_hex[16];
  char signature_hash_hex[16];
  SNPRINTF(source_hash_hex, "%08x", source_hash);
  SNPRINTF(signature_hash_hex, "%08x", signature_hash);
  result.source_hash_hex = source_hash_hex;
  result.source_prefix = "glsl_src_" + result.source_hash_hex + "_";
  result.source_filename = "glsl_function_source_" + result.source_hash_hex + ".glsl";
  result.wrapper_name = "glsl_fn_" + std::to_string(node.identifier) + "_" + signature_hash_hex;
  result.wrapper_filename = result.wrapper_name + ".glsl";
  result.library_source = build_namespaced_glsl_source(
      result.source_prefix, result.function_names, result.source, Span<GLSLClosureSampleHelper>());
  result.wrapper_source = build_wrapper_glsl_source(
      result, Map<std::string, GLSLSample2DSourceKind>());
  return result;
}

static void cache_parse_status(bNode &node, const GLSLParseResult &parse_result)
{
  NodeShaderGLSLFunction &storage = node_storage(node);
  if (parse_result.ok) {
    storage.parse_status = SHD_GLSL_FUNCTION_PARSE_READY;
    storage.signature_hash = parse_result.signature_hash;
  }
  else if (parse_result.error.empty()) {
    storage.parse_status = SHD_GLSL_FUNCTION_PARSE_DIRTY;
    storage.signature_hash = 0;
  }
  else {
    storage.parse_status = SHD_GLSL_FUNCTION_PARSE_ERROR;
    storage.signature_hash = 0;
  }
}

static void add_glsl_socket_declaration(NodeDeclarationBuilder &b,
                                        const GLSLFunctionParam &param,
                                        const bool is_output,
                                        const StringRef socket_name,
                                        const StringRef socket_identifier)
{
  if (glsl_boundary_type_is_image_sampler(param.type)) {
    BLI_assert(!is_output);
    b.add_input<decl::Image>(socket_name, socket_identifier);
    return;
  }

  if (glsl_boundary_type_is_sample2d(param.type)) {
    BLI_assert(!is_output);
    b.add_input<decl::Closure>(socket_name, socket_identifier);
    return;
  }

  if (param.type == GLSLBoundaryType::Float) {
    if (is_output) {
      b.add_output<decl::Float>(socket_name, socket_identifier);
    }
    else {
      auto &decl = b.add_input<decl::Float>(socket_name, socket_identifier)
                       .min(param.meta.has_min ? param.meta.min_value : -10000.0f)
                       .max(param.meta.has_max ? param.meta.max_value : 10000.0f);
      if (param.meta.has_default_value) {
        decl.default_value(param.meta.default_value.x);
      }
      if (param.meta.hide_value) {
        decl.hide_value();
      }
      if (param.meta.subtype.has_value()) {
        decl.subtype(*param.meta.subtype);
      }
    }
    return;
  }

  auto configure_vector_decl = [&](auto &decl) {
    decl.dimensions(param.dimensions)
        .min(param.meta.has_min ? param.meta.min_value : -10000.0f)
        .max(param.meta.has_max ? param.meta.max_value : 10000.0f);
    if (param.meta.has_default_value) {
      switch (param.dimensions) {
        case 2:
          decl.default_value(float2(param.meta.default_value.x, param.meta.default_value.y));
          break;
        case 3:
          decl.default_value(
              float3(param.meta.default_value.x, param.meta.default_value.y, param.meta.default_value.z));
          break;
        case 4:
          decl.default_value(param.meta.default_value);
          break;
        default:
          break;
      }
    }
    if (param.meta.subtype.has_value()) {
      decl.subtype(*param.meta.subtype);
    }
    if (param.meta.hide_value) {
      decl.hide_value();
    }
  };

  if (is_output) {
    auto &decl = b.add_output<decl::Vector>(socket_name, socket_identifier);
    configure_vector_decl(decl);
  }
  else {
    auto &decl = b.add_input<decl::Vector>(socket_name, socket_identifier);
    configure_vector_decl(decl);
  }
}

static void sync_glsl_meta_defaults(bNode &node, const GLSLParseResult &parse_result)
{
  NodeShaderGLSLFunction &storage = node_storage(node);
  if (!parse_result.ok) {
    storage.meta_hash = 0;
    return;
  }
  if (storage.meta_hash == parse_result.meta_hash) {
    return;
  }

  for (const GLSLFunctionParam &param : parse_result.function.params) {
    if (!glsl_param_has_input_socket(param) || !param.meta.has_default_value) {
      continue;
    }

    bNodeSocket *socket = find_node_input_socket_by_identifier(node, param.identifier);
    if (socket == nullptr || socket->default_value == nullptr) {
      continue;
    }

    if (param.type == GLSLBoundaryType::Float && socket->type == SOCK_FLOAT) {
      socket->default_value_typed<bNodeSocketValueFloat>()->value = param.meta.default_value.x;
    }
    else if (ELEM(param.type, GLSLBoundaryType::Vec2, GLSLBoundaryType::Vec3, GLSLBoundaryType::Vec4) &&
             socket->type == SOCK_VECTOR)
    {
      std::copy_n(
          &param.meta.default_value[0], param.dimensions, socket->default_value_typed<bNodeSocketValueVector>()->value);
    }
  }

  storage.meta_hash = parse_result.meta_hash;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (node == nullptr) {
    return;
  }

  const GLSLParseResult parse_result = parse_glsl_for_node(*node);
  if (!parse_result.ok) {
    b.declaration().skip_updating_sockets = parse_result.keep_existing_sockets;
    return;
  }

  for (const GLSLFunctionParam &param : parse_result.function.params) {
    if (glsl_param_has_input_socket(param)) {
      add_glsl_socket_declaration(b, param, false, param.name, param.identifier);
    }
  }

  if (parse_result.function.return_type != GLSLBoundaryType::Void) {
    GLSLFunctionParam output_param;
    output_param.type = parse_result.function.return_type;
    output_param.type_name = parse_result.function.return_type_name;
    output_param.dimensions = glsl_boundary_dimensions(parse_result.function.return_type);
    add_glsl_socket_declaration(
        b, output_param, true, "Result", result_socket_identifier);
  }

  for (const GLSLFunctionParam &param : parse_result.function.params) {
    if (glsl_param_has_output_socket(param)) {
      add_glsl_socket_declaration(
          b, param, true, param.name, make_socket_identifier("Out", param.name));
    }
  }
}

static void draw_node_layout_content(ui::Layout &layout, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  {
    ui::Layout &row = layout.row(false);
    row.prop(
        ptr, "source_mode", ui::ITEM_R_SPLIT_EMPTY_NAME | ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  }

  {
    ui::Layout &row = layout.row(true);
    if (RNA_enum_get(ptr, "source_mode") == SHD_GLSL_FUNCTION_SOURCE_INTERNAL) {
      row.prop(ptr, "script", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    }
    else {
      row.prop(ptr, "filepath", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    }
    row.op("node.glsl_function_refresh", "", ICON_FILE_REFRESH);
  }

  layout.prop(ptr, "function_name", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

  bNode &node = *static_cast<bNode *>(ptr->data);
  const NodeShaderGLSLFunction &storage = node_storage(node);
  const GLSLParseResult parse_result = parse_glsl_for_node(node);
  cache_parse_status(node, parse_result);
  sync_sampler_socket_visibility(node, parse_result);
  sync_glsl_meta_defaults(node, parse_result);

  draw_sampler_input_properties(layout, ptr, node);

  if (parse_result.ok) {
    std::string label = "Using function: " + parse_result.resolved_function_name;
    layout.label(label.c_str(), ICON_NONE);
  }
  else if (storage.function_name[0] == '\0' && !parse_result.function_names.is_empty()) {
    layout.label(IFACE_("Choose a function"), ICON_NONE);
  }
  else if (!parse_result.error.empty()) {
    layout.label(parse_result.error.c_str(), ICON_ERROR);
  }
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  draw_node_layout_content(layout, ptr);
}

static void node_layout_ex(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  draw_node_layout_content(layout, ptr);

  bNode &node = *static_cast<bNode *>(ptr->data);
  if (!node_has_sampler_input_sockets(node)) {
    return;
  }

  ui::Layout &box = layout.box();
  box.use_property_split_set(true);
  box.use_property_decorate_set(false);
  box.label(IFACE_("Sampler Settings"), ICON_NONE);
  box.prop(ptr, "sampler_interpolation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  box.prop(ptr, "sampler_extension", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderGLSLFunction *data = MEM_new<NodeShaderGLSLFunction>(__func__);
  data->sampler_interpolation = SHD_INTERP_LINEAR;
  data->sampler_extension = SHD_IMAGE_EXTENSION_REPEAT;
  node->storage = data;
}

static GPUSamplerState glsl_function_sampler_state(const NodeShaderGLSLFunction &storage)
{
  GPUSamplerState sampler_state = GPUSamplerState::default_sampler();

  switch (storage.sampler_extension) {
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

  if (storage.sampler_interpolation != SHD_INTERP_CLOSEST) {
    sampler_state.filtering = GPU_SAMPLER_FILTERING_ANISOTROPIC | GPU_SAMPLER_FILTERING_LINEAR |
                              GPU_SAMPLER_FILTERING_MIPMAP;
  }

  return sampler_state;
}

static bool prepare_sampler_input_bindings(GPUMaterial *mat,
                                           const bNode &node,
                                           const GLSLFunctionDefinition &function,
                                           GPUNodeStack *in)
{
  static float zero_value[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  if (in == nullptr || !glsl_function_has_sampler_inputs(function)) {
    return true;
  }

  const NodeShaderGLSLFunction &storage = node_storage(node);
  const GPUSamplerState sampler_state = glsl_function_sampler_state(storage);

  auto find_input_stack_by_identifier = [&](const StringRef identifier) -> GPUNodeStack * {
    int input_index = 0;
    for (const bNodeSocket *socket : node.input_sockets()) {
      if (identifier == socket->identifier) {
        return &in[input_index];
      }
      input_index++;
    }
    return nullptr;
  };

  auto resolve_sample2d_image = [&](const GLSLFunctionParam &param) -> Image * {
    const bNodeLink *used_link = nullptr;
    const GLSLSample2DSourceKind source_kind = resolve_sample2d_source_kind(node, param, used_link);
    if (source_kind != GLSLSample2DSourceKind::ImageToClosure || used_link == nullptr) {
      return nullptr;
    }
    return resolve_image_to_closure_image(*used_link);
  };

  auto resolve_sample2d_sampler_state = [&](const GLSLFunctionParam &param) -> GPUSamplerState {
    const bNodeLink *used_link = nullptr;
    const GLSLSample2DSourceKind source_kind = resolve_sample2d_source_kind(node, param, used_link);
    if (source_kind == GLSLSample2DSourceKind::ImageToClosure && used_link != nullptr &&
        used_link->fromnode != nullptr)
    {
      return sampler_state_from_image_to_closure_node(*used_link->fromnode);
    }
    return sampler_state;
  };

  for (const GLSLFunctionParam &param : function.params) {
    if (!glsl_param_has_input_socket(param)) {
      continue;
    }

    if (glsl_boundary_type_is_sampler(param.type)) {
      const std::string socket_identifier = param.identifier;
      const bNodeSocket *socket = find_node_input_socket_by_identifier(node, socket_identifier);
      GPUNodeStack *stack = find_input_stack_by_identifier(socket_identifier);
      if (stack == nullptr) {
        return false;
      }

      Image *image = nullptr;
      if (glsl_boundary_type_is_sample2d(param.type)) {
        const bNodeLink *used_link = nullptr;
        const GLSLSample2DSourceKind source_kind = resolve_sample2d_source_kind(node, param, used_link);
        if (source_kind == GLSLSample2DSourceKind::ClosureOutput) {
          stack->type = GPU_FLOAT;
          stack->link = GPU_constant(zero_value);
          continue;
        }
        image = resolve_sample2d_image(param);
      }
      else {
        if (socket == nullptr || socket->type != SOCK_IMAGE || socket->default_value == nullptr ||
            socket->is_directly_linked())
        {
          return false;
        }
        image = socket->default_value_typed<bNodeSocketValueImage>()->value;
      }

      if (image == nullptr || image->source == IMA_SRC_TILED) {
        return false;
      }

      stack->type = GPU_TEX2D;
      const GPUSamplerState active_sampler_state = glsl_boundary_type_is_sample2d(param.type) ?
                                                      resolve_sample2d_sampler_state(param) :
                                                      sampler_state;
      stack->link = GPU_image(mat, image, nullptr, active_sampler_state);
    }
  }

  return true;
}

static void node_update(bNodeTree * /*ntree*/, bNode *node)
{
  const GLSLParseResult parse_result = parse_glsl_for_node(*node);
  cache_parse_status(*node, parse_result);
  sync_sampler_socket_visibility(*node, parse_result);
  sync_glsl_meta_defaults(*node, parse_result);
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

static int gpu_shader_glsl_function(GPUMaterial *mat,
                                    bNode *node,
                                    bNodeExecData * /*execdata*/,
                                    GPUNodeStack *in,
                                    GPUNodeStack *out)
{
  const GLSLParseResult parse_result = parse_glsl_for_node(*node);
  cache_parse_status(*node, parse_result);
  sync_glsl_meta_defaults(*node, parse_result);

  if (!parse_result.ok) {
    static float zero_value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (out != nullptr) {
      for (int i = 0; !out[i].end; i++) {
        if (out[i].type != GPU_NONE) {
          out[i].link = GPU_constant(zero_value);
        }
      }
    }
    return 1;
  }
  if (!prepare_sampler_input_bindings(mat, *node, parse_result.function, in)) {
    static float zero_value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (out != nullptr) {
      for (int i = 0; !out[i].end; i++) {
        if (out[i].type != GPU_NONE) {
          out[i].link = GPU_constant(zero_value);
        }
      }
    }
    return 1;
  }

  std::string library_source = parse_result.library_source;
  std::string wrapper_source = parse_result.wrapper_source;
  std::string specialized_error;
  if (!build_specialized_glsl_sources(
          mat, *node, parse_result, library_source, wrapper_source, specialized_error))
  {
    static float zero_value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (out != nullptr) {
      for (int i = 0; !out[i].end; i++) {
        if (out[i].type != GPU_NONE) {
          out[i].link = GPU_constant(zero_value);
        }
      }
    }
    return 1;
  }

  GPU_material_generated_source_add(
      mat, parse_result.source_filename.c_str(), {}, library_source.c_str());

  Vector<StringRefNull> dependencies;
  dependencies.append(parse_result.source_filename.c_str());
  GPU_material_generated_source_add(
      mat,
      parse_result.wrapper_filename.c_str(),
      dependencies,
      wrapper_source.c_str());

  return GPU_stack_link_custom(mat,
                               node,
                               parse_result.wrapper_name.c_str(),
                               parse_result.wrapper_filename.c_str(),
                               in,
                               out);
}

}  // namespace nodes::node_shader_glsl_function_cc

void register_node_type_sh_glsl_function()
{
  namespace file_ns = nodes::node_shader_glsl_function_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeGLSLFunction", SH_NODE_GLSL_FUNCTION);
  ntype.ui_name = "GLSL Function";
  ntype.ui_description =
      "Call a user-authored GLSL function from a Text datablock or external GLSL file";
  ntype.enum_name_legacy = "GLSL_FUNCTION";
  ntype.nclass = NODE_CLASS_SCRIPT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.draw_buttons_ex = file_ns::node_layout_ex;
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  ntype.insert_link = file_ns::node_insert_link;
  ntype.gpu_fn = file_ns::gpu_shader_glsl_function;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  bke::node_type_storage(
      ntype, "NodeShaderGLSLFunction", node_free_standard_storage, node_copy_standard_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
