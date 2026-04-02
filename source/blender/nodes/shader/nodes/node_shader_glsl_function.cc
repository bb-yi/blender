/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include <cctype>
#include <sstream>

#include "node_shader_util.hh"

#include "BKE_image.hh"
#include "BKE_node_runtime.hh"
#include "BKE_text.h"

#include "BLI_fileops.h"
#include "BLI_ghash.h"
#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "RNA_access.hh"

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
};

struct GLSLFunctionParam {
  enum class Qualifier {
    In,
    Out,
    InOut,
  };

  GLSLBoundaryType type = GLSLBoundaryType::Unsupported;
  Qualifier qualifier = Qualifier::In;
  std::string type_name;
  std::string name;
  std::string identifier;
  int dimensions = 0;
};

struct GLSLFunctionDefinition {
  std::string name;
  std::string return_type_name;
  GLSLBoundaryType return_type = GLSLBoundaryType::Unsupported;
  Vector<GLSLFunctionParam> params;
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
};

static constexpr const char *result_socket_identifier = "Result";

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
  return type == GLSLBoundaryType::Sampler2D;
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
                                    const GLSLFunctionDefinition &function,
                                    std::string &r_error)
{
  for (const GLSLFunctionParam &param : function.params) {
    if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_sampler(param.type)) {
      continue;
    }

    const bNodeSocket *socket = find_node_input_socket_by_identifier(node, param.identifier);
    if (socket == nullptr || socket->type != SOCK_IMAGE || socket->default_value == nullptr) {
      continue;
    }

    if (socket->is_directly_linked()) {
      r_error = "sampler2D parameter '" + param.name +
                "' does not support links yet; choose an image on the node";
      return false;
    }

    Image *image = socket->default_value_typed<bNodeSocketValueImage>()->value;
    if (image == nullptr) {
      r_error = "Choose an image for sampler2D parameter '" + param.name + "'";
      return false;
    }
    if (image->source == IMA_SRC_TILED) {
      r_error = "sampler2D parameter '" + param.name +
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
    if (!glsl_param_has_input_socket(param) || !glsl_boundary_type_is_sampler(param.type)) {
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
      tokens.append({GLSLToken::Kind::Identifier, std::string(source.substr(start, i - start)), '\0'});
      continue;
    }

    if (strchr("(){}[],;=", c) != nullptr) {
      tokens.append({GLSLToken::Kind::Punctuation, std::string(1, c), c});
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
  const GLSLBoundaryType boundary_type = glsl_boundary_type_from_name(type_name);
  if (!ELEM(boundary_type,
            GLSLBoundaryType::Float,
            GLSLBoundaryType::Vec2,
            GLSLBoundaryType::Vec3,
            GLSLBoundaryType::Vec4,
            GLSLBoundaryType::Sampler2D))
  {
    r_error = "Supported parameter types are float, vec2, vec3, vec4, and sampler2D";
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

static std::string build_namespaced_glsl_source(const StringRef prefix,
                                                const Span<std::string> function_names,
                                                const StringRef source)
{
  std::stringstream ss;
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
  return ss.str();
}

static std::string build_wrapper_glsl_source(const GLSLParseResult &parse_result)
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
    ss << param.type_name << " " << make_wrapper_argument_name("in", param.name);
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
  if (!validate_sampler_inputs(node, result.function, result.error)) {
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
      result.source_prefix, result.function_names, result.source);
  result.wrapper_source = build_wrapper_glsl_source(result);
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
  if (param.type == GLSLBoundaryType::Sampler2D) {
    BLI_assert(!is_output);
    b.add_input<decl::Image>(socket_name, socket_identifier);
    return;
  }

  if (param.type == GLSLBoundaryType::Float) {
    if (is_output) {
      b.add_output<decl::Float>(socket_name, socket_identifier);
    }
    else {
      b.add_input<decl::Float>(socket_name, socket_identifier).min(-10000.0f).max(10000.0f);
    }
    return;
  }

  auto configure_vector_decl = [&](auto &decl) {
    decl.dimensions(param.dimensions).min(-10000.0f).max(10000.0f);
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
  }

  layout.prop(ptr, "function_name", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

  bNode &node = *static_cast<bNode *>(ptr->data);
  const NodeShaderGLSLFunction &storage = node_storage(node);
  const GLSLParseResult parse_result = parse_glsl_for_node(node);
  cache_parse_status(node, parse_result);
  sync_sampler_socket_visibility(node, parse_result);

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

static void node_layout_ex(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  draw_node_layout_content(layout, ptr);

  bNode &node = *static_cast<bNode *>(ptr->data);
  if (!node_has_sampler_input_sockets(node)) {
    return;
  }

  const std::string panel_id = "glsl_function_sampler_settings_" + std::to_string(node.identifier);
  if (ui::Layout *panel = layout.panel(C, panel_id.c_str(), true, IFACE_("Sampler Settings"))) {
    panel->use_property_split_set(true);
    panel->use_property_decorate_set(false);
    panel->prop(
        ptr, "sampler_interpolation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    panel->prop(ptr, "sampler_extension", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
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
  if (in == nullptr || !glsl_function_has_sampler_inputs(function)) {
    return true;
  }

  const NodeShaderGLSLFunction &storage = node_storage(node);
  const GPUSamplerState sampler_state = glsl_function_sampler_state(storage);

  int input_index = 0;
  for (const GLSLFunctionParam &param : function.params) {
    if (!glsl_param_has_input_socket(param)) {
      continue;
    }

    if (glsl_boundary_type_is_sampler(param.type)) {
      const Span<const bNodeSocket *> input_sockets = node.input_sockets();
      const bNodeSocket *socket = input_index < input_sockets.size() ? input_sockets[input_index] :
                                                                     nullptr;
      if (socket == nullptr || socket->type != SOCK_IMAGE || socket->default_value == nullptr ||
          socket->is_directly_linked())
      {
        return false;
      }

      Image *image = socket->default_value_typed<bNodeSocketValueImage>()->value;
      if (image == nullptr || image->source == IMA_SRC_TILED) {
        return false;
      }

      in[input_index].type = GPU_TEX2D;
      in[input_index].link = GPU_image(mat, image, nullptr, sampler_state);
    }

    input_index++;
  }

  return true;
}

static void node_update(bNodeTree * /*ntree*/, bNode *node)
{
  const GLSLParseResult parse_result = parse_glsl_for_node(*node);
  cache_parse_status(*node, parse_result);
  sync_sampler_socket_visibility(*node, parse_result);
}

static int gpu_shader_glsl_function(GPUMaterial *mat,
                                    bNode *node,
                                    bNodeExecData * /*execdata*/,
                                    GPUNodeStack *in,
                                    GPUNodeStack *out)
{
  const GLSLParseResult parse_result = parse_glsl_for_node(*node);
  cache_parse_status(*node, parse_result);

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

  GPU_material_generated_source_add(
      mat, parse_result.source_filename.c_str(), {}, parse_result.library_source.c_str());

  Vector<StringRefNull> dependencies;
  dependencies.append(parse_result.source_filename.c_str());
  GPU_material_generated_source_add(
      mat,
      parse_result.wrapper_filename.c_str(),
      dependencies,
      parse_result.wrapper_source.c_str());

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
  ntype.gpu_fn = file_ns::gpu_shader_glsl_function;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  bke::node_type_storage(
      ntype, "NodeShaderGLSLFunction", node_free_standard_storage, node_copy_standard_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
