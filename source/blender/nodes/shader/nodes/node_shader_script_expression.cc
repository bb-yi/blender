/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include <cctype>
#include <cstdint>
#include <sstream>

#include "node_shader_util.hh"

#include "BLI_hash_mm2a.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"

#include "BLO_read_write.hh"

#include "DNA_node_types.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"

#include "NOD_sh_script_expression.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

namespace blender {

namespace nodes::node_shader_script_expression_cc {

NODE_STORAGE_FUNCS(NodeShaderScriptExpression)

static const char *script_expr_output_identifier = "Result";

static bool socket_type_supported(const eNodeSocketDatatype socket_type)
{
  return ELEM(socket_type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA);
}

static eNodeSocketDatatype safe_socket_type(const short socket_type)
{
  const eNodeSocketDatatype type = eNodeSocketDatatype(socket_type);
  return socket_type_supported(type) ? type : SOCK_FLOAT;
}

static const char *glsl_type_for_socket(const eNodeSocketDatatype socket_type)
{
  switch (socket_type) {
    case SOCK_VECTOR:
      return "float3";
    case SOCK_RGBA:
      return "float4";
    case SOCK_FLOAT:
    default:
      return "float";
  }
}

static GPUType gpu_type_for_socket(const eNodeSocketDatatype socket_type)
{
  switch (socket_type) {
    case SOCK_VECTOR:
      return GPU_VEC3;
    case SOCK_RGBA:
      return GPU_VEC4;
    case SOCK_FLOAT:
    default:
      return GPU_FLOAT;
  }
}

static bool is_identifier_start(const char c)
{
  return (c == '_') || std::isalpha(uint8_t(c));
}

static bool is_identifier_continue(const char c)
{
  return (c == '_') || std::isalnum(uint8_t(c));
}

static bool is_glsl_reserved_identifier(const StringRef name)
{
  if (name.startswith("gl_")) {
    return true;
  }
  static const Set<StringRefNull> reserved = {
      "attribute",
      "bool",
      "break",
      "case",
      "centroid",
      "const",
      "continue",
      "default",
      "discard",
      "do",
      "else",
      "false",
      "flat",
      "float",
      "for",
      "highp",
      "if",
      "in",
      "inout",
      "int",
      "invariant",
      "layout",
      "lowp",
      "mat2",
      "mat3",
      "mat4",
      "mediump",
      "out",
      "precision",
      "return",
      "sampler1D",
      "sampler2D",
      "sampler3D",
      "samplerCube",
      "smooth",
      "struct",
      "switch",
      "true",
      "uniform",
      "varying",
      "vec2",
      "vec3",
      "vec4",
      "void",
      "while",
      "float2",
      "float3",
      "float4",
      "TextureHandle",
      "Closure",
      "result",
  };
  return reserved.contains_as(name);
}

struct ExpressionValidation {
  bool ok = false;
  std::string error;
};

static bool is_blocked_identifier(const StringRef identifier)
{
  if (identifier.startswith("gl_")) {
    return true;
  }
  static const Set<StringRefNull> blocked = {
      "return",
      "if",
      "else",
      "for",
      "while",
      "do",
      "switch",
      "case",
      "break",
      "continue",
      "discard",
      "sampler1D",
      "sampler2D",
      "sampler3D",
      "samplerCube",
      "texture",
      "textureLod",
      "textureGrad",
      "textureSize",
      "texelFetch",
      "imageLoad",
      "imageStore",
      "glsl_position",
      "glsl_normal",
      "glsl_true_normal",
      "glsl_incoming",
      "glsl_ambient_lighting",
      "glsl_light_count",
      "glsl_light_get",
      "glsl_light_shadow",
      "GLSLLight",
  };
  return blocked.contains_as(identifier);
}

static bool is_type_identifier(const StringRef identifier)
{
  static const Set<StringRefNull> type_names = {
      "bool", "int", "float", "vec2", "vec3", "vec4", "float2", "float3", "float4"};
  return type_names.contains_as(identifier);
}

static ExpressionValidation validate_expression(const StringRef expression)
{
  const StringRef trimmed_expression = expression.trim();
  if (trimmed_expression.is_empty()) {
    return {false, "Expression is empty"};
  }

  int paren_depth = 0;
  int bracket_depth = 0;
  bool previous_was_identifier = false;
  for (int i = 0; i < expression.size(); i++) {
    const char c = expression[i];
    if (c == ';' || c == '{' || c == '}' || c == '#') {
      return {false, "Only a single GLSL expression is allowed"};
    }
    if (ELEM(c, '"', '\'', '\\')) {
      return {false, "Only numeric GLSL expressions are allowed"};
    }
    if ((c == '/' && i + 1 < expression.size() && ELEM(expression[i + 1], '/', '*')) ||
        (c == '*' && i + 1 < expression.size() && expression[i + 1] == '/'))
    {
      return {false, "Comments are not allowed"};
    }

    if (c == '(') {
      paren_depth++;
    }
    else if (c == ')') {
      paren_depth--;
      if (paren_depth < 0) {
        return {false, "Parentheses are not balanced"};
      }
    }
    else if (c == '[') {
      bracket_depth++;
    }
    else if (c == ']') {
      bracket_depth--;
      if (bracket_depth < 0) {
        return {false, "Brackets are not balanced"};
      }
    }

    if (c == '=') {
      const char prev = (i > 0) ? expression[i - 1] : '\0';
      const char next = (i + 1 < expression.size()) ? expression[i + 1] : '\0';
      if (next == '=') {
        if (ELEM(prev, '<', '>', '!', '=')) {
          return {false, "Assignment is not allowed"};
        }
        i++;
        continue;
      }
      if (!ELEM(prev, '<', '>', '!') || (i > 1 && expression[i - 2] == prev)) {
        return {false, "Assignment is not allowed"};
      }
    }
    if (ELEM(c, '+', '-', '*', '/', '%') && i + 1 < expression.size() &&
        expression[i + 1] == '=')
    {
      return {false, "Assignment is not allowed"};
    }
    if (ELEM(c, '+', '-') && i + 1 < expression.size() && expression[i + 1] == c) {
      return {false, "Increment and decrement are not allowed"};
    }

    if (is_identifier_start(c)) {
      const int start = i;
      i++;
      while (i < expression.size() && is_identifier_continue(expression[i])) {
        i++;
      }
      const StringRef identifier = expression.substr(start, i - start);
      if (is_blocked_identifier(identifier)) {
        return {false, "This expression uses unsupported GLSL features"};
      }
      if (previous_was_identifier) {
        return {false, "Declarations are not allowed"};
      }
      int next_non_space = i;
      while (next_non_space < expression.size() && std::isspace(uint8_t(expression[next_non_space])))
      {
        next_non_space++;
      }
      if (is_type_identifier(identifier) &&
          (next_non_space >= expression.size() || expression[next_non_space] != '('))
      {
        return {false, "Declarations are not allowed"};
      }
      previous_was_identifier = true;
      i--;
      continue;
    }
    if (std::isspace(uint8_t(c))) {
      continue;
    }
    previous_was_identifier = false;
  }

  if (paren_depth != 0 || bracket_depth != 0) {
    return {false, "Parentheses or brackets are not balanced"};
  }
  return {true, ""};
}

static ExpressionValidation validate_expression_for_node(const bNode &node)
{
  const NodeShaderScriptExpression *storage = static_cast<const NodeShaderScriptExpression *>(
      node.storage);
  if (storage == nullptr) {
    return {false, "Node storage is missing"};
  }
  return validate_expression(storage->expression);
}

static std::string source_hash_suffix(const bNode &node)
{
  const NodeShaderScriptExpression &storage = node_storage(node);
  BLI_HashMurmur2A mm2;
  BLI_hash_mm2a_init(&mm2, 0);
  BLI_hash_mm2a_add_int(&mm2, node.identifier);
  BLI_hash_mm2a_add_int(&mm2, int(storage.output_socket_type));
  BLI_hash_mm2a_add(&mm2,
                    reinterpret_cast<const unsigned char *>(storage.expression),
                    strlen(storage.expression));
  for (const NodeShaderScriptExpressionVariable &item : storage.variables_span()) {
    BLI_hash_mm2a_add_int(&mm2, item.identifier);
    BLI_hash_mm2a_add_int(&mm2, int(item.socket_type));
    if (item.name != nullptr) {
      BLI_hash_mm2a_add(
          &mm2, reinterpret_cast<const unsigned char *>(item.name), strlen(item.name));
    }
  }
  return std::to_string(BLI_hash_mm2a_end(&mm2));
}

static std::string build_wrapper_source(const bNode &node, const StringRefNull function_name)
{
  const NodeShaderScriptExpression &storage = node_storage(node);
  std::stringstream source;
  source << "void " << function_name << "(";
  bool first_argument = true;
  for (const NodeShaderScriptExpressionVariable &item : storage.variables_span()) {
    if (!first_argument) {
      source << ", ";
    }
    first_argument = false;
    source << glsl_type_for_socket(safe_socket_type(item.socket_type)) << " "
           << (item.name ? item.name : "var");
  }
  if (!first_argument) {
    source << ", ";
  }
  source << "out " << glsl_type_for_socket(safe_socket_type(storage.output_socket_type))
         << " result)\n";
  source << "{\n";
  source << "  result = " << StringRef(storage.expression).trim() << ";\n";
  source << "}\n";
  return source.str();
}

static void set_zero_output(GPUNodeStack *out)
{
  static float zero_value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (out != nullptr && out[0].type != GPU_NONE) {
    out[0].link = GPU_constant(zero_value);
  }
}

static void draw_expression_settings(ui::Layout &layout, PointerRNA *ptr);
static void draw_variables_settings(ui::Layout &layout, bContext *C, PointerRNA *ptr);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  auto add_expression_panel = [&]() -> PanelDeclarationBuilder & {
    PanelDeclarationBuilder &panel = b.add_panel("Expression", 0);
    panel.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
      draw_expression_settings(layout, ptr);
    });
    return panel;
  };

  auto add_variables_panel = [&]() -> PanelDeclarationBuilder & {
    PanelDeclarationBuilder &panel = b.add_panel("Variables", 1).default_closed(true);
    panel.add_layout([](ui::Layout &layout, bContext *C, PointerRNA *ptr) {
      draw_variables_settings(layout, C, ptr);
    });
    return panel;
  };

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  if (!node || !tree || !node->storage) {
    b.add_output<decl::Float>("Result", script_expr_output_identifier);
    add_expression_panel();
    add_variables_panel();
    return;
  }

  const NodeShaderScriptExpression &storage = node_storage(*node);
  b.add_output(safe_socket_type(storage.output_socket_type),
               "Result",
               script_expr_output_identifier);
  add_expression_panel();
  PanelDeclarationBuilder &variables_panel = add_variables_panel();

  for (const NodeShaderScriptExpressionVariable &item : storage.variables_span()) {
    const eNodeSocketDatatype socket_type = safe_socket_type(item.socket_type);
    const StringRefNull name = item.name ? item.name : "";
    const std::string identifier = ShScriptExpressionVariablesAccessor::socket_identifier_for_item(
        item);
    auto &input_decl = variables_panel.add_input(socket_type, name, identifier)
                           .socket_name_ptr(&tree->id,
                                            *ShScriptExpressionVariablesAccessor::item_srna,
                                            &item,
                                            "name");
    input_decl.structure_type(StructureType::Dynamic);
  }
}

static void draw_expression_settings(ui::Layout &layout, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "output_type", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "expression", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

  const bNode &node = *ptr->data_as<bNode>();
  const ExpressionValidation validation = validate_expression_for_node(node);
  if (!validation.ok) {
    layout.label(validation.error.c_str(), ICON_ERROR);
  }
}

static void draw_variables_settings(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *ptr->data_as<bNode>();

  socket_items::ui::draw_items_list_with_operators<ShScriptExpressionVariablesAccessor>(
      C, &layout, ntree, node);
  socket_items::ui::draw_active_item_props<ShScriptExpressionVariablesAccessor>(
      ntree, node, [&](PointerRNA *item_ptr) {
        layout.use_property_split_set(true);
        layout.use_property_decorate_set(false);
        layout.prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      });
}

static void node_layout(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  draw_expression_settings(layout, ptr);
  draw_variables_settings(layout, C, ptr);
}

static void node_layout_ex(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  draw_expression_settings(layout, ptr);

  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *ptr->data_as<bNode>();
  if (ui::Layout *panel = layout.panel(C, "script_expression_variables", false, IFACE_("Variables")))
  {
    socket_items::ui::draw_items_list_with_operators<ShScriptExpressionVariablesAccessor>(
        C, panel, ntree, node);
    socket_items::ui::draw_active_item_props<ShScriptExpressionVariablesAccessor>(
        ntree, node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderScriptExpression *storage = MEM_new<NodeShaderScriptExpression>(__func__);
  STRNCPY(storage->expression, "0.0");
  storage->output_socket_type = SOCK_FLOAT;
  storage->next_identifier = 0;
  node->storage = storage;
  node->flag |= NODE_OPTIONS;
}

static void node_free_storage(bNode *node)
{
  if (node->storage == nullptr) {
    return;
  }
  socket_items::destruct_array<ShScriptExpressionVariablesAccessor>(*node);
  MEM_delete(static_cast<NodeShaderScriptExpression *>(node->storage));
  node->storage = nullptr;
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeShaderScriptExpression &src_storage = node_storage(*src_node);
  NodeShaderScriptExpression *dst_storage = MEM_new<NodeShaderScriptExpression>(
      __func__, dna::shallow_copy(src_storage));
  dst_node->storage = dst_storage;
  socket_items::copy_array<ShScriptExpressionVariablesAccessor>(*src_node, *dst_node);
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  if (node.storage != nullptr) {
    socket_items::blend_write<ShScriptExpressionVariablesAccessor>(&writer, node);
  }
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  if (node.storage != nullptr) {
    socket_items::blend_read_data<ShScriptExpressionVariablesAccessor>(&reader, node);
  }
}

static void node_update(bNodeTree * /*ntree*/, bNode *node)
{
  if (node->storage == nullptr) {
    return;
  }
  NodeShaderScriptExpression &storage = node_storage(*node);
  storage.output_socket_type = safe_socket_type(storage.output_socket_type);
  for (NodeShaderScriptExpressionVariable &item : storage.variables_span()) {
    item.socket_type = safe_socket_type(item.socket_type);
  }
}

static int node_shader_gpu_script_expression(GPUMaterial *mat,
                                             bNode *node,
                                             bNodeExecData * /*execdata*/,
                                             GPUNodeStack *in,
                                             GPUNodeStack *out)
{
  if (node->storage == nullptr) {
    set_zero_output(out);
    return true;
  }
  const ExpressionValidation validation = validate_expression_for_node(*node);
  if (!validation.ok) {
    set_zero_output(out);
    return true;
  }

  const NodeShaderScriptExpression &storage = node_storage(*node);
  if (out != nullptr) {
    out[0].type = gpu_type_for_socket(safe_socket_type(storage.output_socket_type));
  }
  for (const int index : storage.variables_span().index_range()) {
    in[index].type = gpu_type_for_socket(safe_socket_type(storage.variables[index].socket_type));
  }

  const std::string hash = source_hash_suffix(*node);
  const std::string function_name = "script_expr_" + std::to_string(node->identifier) + "_" +
                                    hash;
  const std::string filename = function_name + ".glsl";
  const std::string source = build_wrapper_source(*node, function_name.c_str());
  GPU_material_generated_source_add(mat, filename.c_str(), {}, source.c_str());
  return GPU_stack_link_custom(mat,
                               node,
                               function_name.c_str(),
                               filename.c_str(),
                               GPU_CUSTOM_NODE_DEPENDENCY_NONE,
                               in,
                               out);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ShScriptExpressionVariablesAccessor>();
}

}  // namespace nodes::node_shader_script_expression_cc

namespace nodes {

StructRNA **ShScriptExpressionVariablesAccessor::item_srna =
    &RNA_ShaderScriptExpressionVariable;

socket_items::SocketItemsRef<NodeShaderScriptExpressionVariable>
ShScriptExpressionVariablesAccessor::get_items_from_node(bNode &node)
{
  NodeShaderScriptExpression *storage = static_cast<NodeShaderScriptExpression *>(node.storage);
  return {&storage->variables, &storage->variables_num, &storage->active_variable_index};
}

void ShScriptExpressionVariablesAccessor::copy_item(const ItemT &src, ItemT &dst)
{
  dst = src;
  dst.name = BLI_strdup_null(src.name);
}

void ShScriptExpressionVariablesAccessor::destruct_item(ItemT *item)
{
  MEM_SAFE_DELETE(item->name);
}

eNodeSocketDatatype ShScriptExpressionVariablesAccessor::get_socket_type(const ItemT &item)
{
  return node_shader_script_expression_cc::safe_socket_type(item.socket_type);
}

char **ShScriptExpressionVariablesAccessor::get_name(ItemT &item)
{
  return &item.name;
}

bool ShScriptExpressionVariablesAccessor::supports_socket_type(
    const eNodeSocketDatatype socket_type, const int ntree_type)
{
  if (!node_shader_script_expression_cc::socket_type_supported(socket_type)) {
    return false;
  }
  return bke::node_tree_type_supports_socket_type_static(ntree_type, socket_type);
}

std::string ShScriptExpressionVariablesAccessor::validate_name(const StringRef name)
{
  std::string result;
  result.reserve(name.size() + 4);
  for (const char c : name) {
    if (result.empty()) {
      if (node_shader_script_expression_cc::is_identifier_start(c)) {
        result.push_back(c);
      }
      else if (node_shader_script_expression_cc::is_identifier_continue(c)) {
        result.push_back('_');
        result.push_back(c);
      }
      else {
        result.push_back('_');
      }
    }
    else {
      result.push_back(node_shader_script_expression_cc::is_identifier_continue(c) ? c : '_');
    }
  }
  if (result.empty() || !node_shader_script_expression_cc::is_identifier_start(result[0])) {
    result = "var";
  }
  if (StringRef(result).startswith("gl_")) {
    result = "var_" + result;
  }
  else if (node_shader_script_expression_cc::is_glsl_reserved_identifier(result) ||
           node_shader_script_expression_cc::is_blocked_identifier(result))
  {
    result += "_var";
  }
  return result;
}

void ShScriptExpressionVariablesAccessor::init_with_socket_type_and_name(
    bNode &node, ItemT &item, const eNodeSocketDatatype socket_type, const char *name)
{
  NodeShaderScriptExpression &storage =
      node_shader_script_expression_cc::node_storage(node);
  item.socket_type = node_shader_script_expression_cc::safe_socket_type(socket_type);
  item.identifier = storage.next_identifier++;
  socket_items::set_item_name_and_make_unique<ShScriptExpressionVariablesAccessor>(
      node, item, name);
}

std::string ShScriptExpressionVariablesAccessor::socket_identifier_for_item(const ItemT &item)
{
  return "Var_" + std::to_string(item.identifier);
}

void ShScriptExpressionVariablesAccessor::blend_write_item(BlendWriter *writer,
                                                           const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ShScriptExpressionVariablesAccessor::blend_read_data_item(BlendDataReader *reader,
                                                               ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace nodes

Span<NodeShaderScriptExpressionVariable> NodeShaderScriptExpression::variables_span() const
{
  return Span<NodeShaderScriptExpressionVariable>(variables, variables_num);
}

MutableSpan<NodeShaderScriptExpressionVariable> NodeShaderScriptExpression::variables_span()
{
  return MutableSpan<NodeShaderScriptExpressionVariable>(variables, variables_num);
}

void register_node_type_sh_script_expression()
{
  namespace file_ns = nodes::node_shader_script_expression_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeScriptExpression", SH_NODE_SCRIPT_EXPRESSION);
  ntype.ui_name = "GLSL Script Expression";
  ntype.ui_description = "Evaluate a single GLSL expression with manually defined inputs";
  ntype.enum_name_legacy = "SCRIPT_EXPRESSION";
  ntype.nclass = NODE_CLASS_SCRIPT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.draw_buttons_ex = file_ns::node_layout_ex;
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  ntype.gpu_fn = file_ns::node_shader_gpu_script_expression;
  ntype.add_ui_poll = object_filter_or_npr_eevee_shader_nodes_poll;
  ntype.register_operators = file_ns::node_operators;
  ntype.blend_write_storage_content = file_ns::node_blend_write;
  ntype.blend_data_read_storage_content = file_ns::node_blend_read;
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Large);
  bke::node_type_storage(
      ntype, "NodeShaderScriptExpression", file_ns::node_free_storage, file_ns::node_copy_storage);

  bke::node_register_type(ntype);
}

}  // namespace blender
