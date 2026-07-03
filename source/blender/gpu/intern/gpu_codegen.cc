/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Convert material node-trees to GLSL.
 */

#include "MEM_guardedalloc.h"

#include "DNA_material_types.h"

#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_cryptomatte.hh"

#include "IMB_colormanagement.hh"

#include "GPU_capabilities.hh"
#include "GPU_shader.hh"
#include "GPU_uniform_buffer.hh"
#include "GPU_vertex_format.hh"

#include "gpu_codegen.hh"
#include "gpu_material_library.hh"
#include "gpu_shader_dependency_private.hh"

#include <cstdarg>
#include <cstring>

namespace blender {

using namespace blender::gpu::shader;

/* -------------------------------------------------------------------- */
/** \name Type > string conversion
 * \{ */

static std::ostream &operator<<(std::ostream &stream, const GPUInput *input)
{
  switch (input->source) {
    case GPU_SOURCE_FUNCTION_CALL:
    case GPU_SOURCE_OUTPUT:
      return stream << (input->is_zone_io ? "zone" : "tmp") << input->id;
    case GPU_SOURCE_CONSTANT:
      return stream << (input->is_zone_io ? "zone" : "cons") << input->id;
    case GPU_SOURCE_UNIFORM:
      return stream << "node_tree.u" << input->id << (input->is_duplicate ? "b" : "");
    case GPU_SOURCE_ATTR:
      return stream << "var_attrs.v" << input->attr->id;
    case GPU_SOURCE_UNIFORM_ATTR:
      return stream << "UNI_ATTR(unf_attrs[resource_id].attr" << input->uniform_attr->id << ")";
    case GPU_SOURCE_LAYER_ATTR:
      return stream << "attr_load_layer(" << input->layer_attr->hash_code << ")";
    case GPU_SOURCE_STRUCT:
      return stream << "strct" << input->id;
    case GPU_SOURCE_TEX:
      if (input->texture == nullptr || input->texture->sampler_name[0] == '\0') {
        BLI_assert_msg(0, "Invalid material texture input");
        return stream << "gpu_invalid_sampler";
      }
      return stream << input->texture->sampler_name;
    case GPU_SOURCE_TEX_TILED_MAPPING:
      if (input->texture == nullptr || input->texture->tiled_mapping_name[0] == '\0') {
        BLI_assert_msg(0, "Invalid material tiled texture input");
        return stream << "gpu_invalid_tiled_sampler";
      }
      return stream << input->texture->tiled_mapping_name;
    default:
      BLI_assert(0);
      return stream;
  }
}

static std::ostream &operator<<(std::ostream &stream, const GPUOutput *output)
{
  return stream << (output->is_zone_io ? "zone" : "tmp") << output->id;
}

/* Print data constructor (i.e: vec2(1.0f, 1.0f)). */
static std::ostream &operator<<(std::ostream &stream, const Span<float> &span)
{
  stream << GPUType(span.size()) << "(";
  /* Use uint representation to allow exact same bit pattern even if NaN. This is
   * because we can pass UINTs as floats for constants. */
  const Span<uint32_t> uint_span = span.cast<uint32_t>();
  for (const uint32_t &element : uint_span) {
    char formatted_float[32];
    SNPRINTF(formatted_float, "uintBitsToFloat(%uu)", element);
    stream << formatted_float;
    if (&element != &uint_span.last()) {
      stream << ", ";
    }
  }
  stream << ")";
  return stream;
}

/* Trick type to change overload and keep a somewhat nice syntax. */
struct GPUConstant : public GPUInput {};

static std::ostream &operator<<(std::ostream &stream, const GPUConstant *input)
{
  stream << Span<float>(input->vec, input->type);
  return stream;
}

namespace gpu::shader {
/* Needed to use the << operators from nested namespaces. :(
 * https://stackoverflow.com/questions/5195512/namespaces-and-operator-resolution */
using blender::operator<<;
}  // namespace gpu::shader

/** \} */

/* -------------------------------------------------------------------- */
/** \name GLSL code generation
 * \{ */

static std::string gpu_function_call_expand(const char *function_call, const std::string &output)
{
  std::string call = function_call;
  size_t placeholder_pos = call.find("$OUT");
  if (placeholder_pos == std::string::npos) {
    call += output;
    call += ")";
    return call;
  }
  while (placeholder_pos != std::string::npos) {
    call.replace(placeholder_pos, 4, output);
    placeholder_pos = call.find("$OUT", placeholder_pos + output.size());
  }
  return call;
}

const char *GPUCodegenCreateInfo::NameBuffer::append_sampler_name(const char name[32])
{
  auto index = sampler_names.size();
  sampler_names.append(std::make_unique<NameEntry>());
  char *name_buffer = sampler_names[index]->data();
  memcpy(name_buffer, name, 32);
  return name_buffer;
}

GPUCodegen::GPUCodegen(GPUMaterial *mat_, GPUNodeGraph *graph_, const char *debug_name)
    : mat(*mat_), graph(*graph_)
{
  BLI_hash_mm2a_init(&hm2a_, GPU_material_uuid_get(&mat));
  BLI_hash_mm2a_add_int(&hm2a_, GPU_material_flag(&mat));
  create_info = MEM_new<GPUCodegenCreateInfo>(__func__, debug_name);
  output.create_info = reinterpret_cast<GPUShaderCreateInfo *>(
      static_cast<ShaderCreateInfo *>(create_info));
}

GPUCodegen::~GPUCodegen()
{
  MEM_SAFE_DELETE(cryptomatte_input_);
  MEM_delete(create_info);
  ubo_inputs_.free_no_destruct();
};

bool GPUCodegen::should_optimize_heuristic() const
{
  /* If each of the maximal attributes are exceeded, we can optimize, but we should also ensure
   * the baseline is met. */
  bool do_optimize = (nodes_total_ >= 60 || textures_total_ >= 4 || uniforms_total_ >= 64) &&
                     (textures_total_ >= 1 && uniforms_total_ >= 8 && nodes_total_ >= 4);
  return do_optimize;
}

void GPUCodegen::generate_attribs()
{
  if (graph.attributes.is_empty()) {
    output.attr_load.clear();
    return;
  }

  GPUCodegenCreateInfo &info = *create_info;

  info.interface_generated = MEM_new<StageInterfaceInfo>(__func__, "codegen_iface", "var_attrs");
  StageInterfaceInfo &iface = *info.interface_generated;
  info.vertex_out(iface);

  /* Input declaration, loading / assignment to interface and geometry shader passthrough. */
  std::stringstream load_ss;

  /* Index of the attribute as ordered in graph.attributes. */
  int attr_n = 0;
  int slot = 15;
  for (GPUMaterialAttribute &attr : graph.attributes) {
    if (slot == -1) {
      BLI_assert_msg(0, "Too many attributes");
      break;
    }
    STRNCPY(info.name_buffer.attr_names[slot], attr.input_name);
    SNPRINTF(info.name_buffer.var_names[slot], "v%d", attr.id);

    StringRefNull attr_name = info.name_buffer.attr_names[slot];
    StringRefNull var_name = info.name_buffer.var_names[slot];

    GPUType input_type, iface_type;

    load_ss << "var_attrs." << var_name;
    if (attr.is_hair_length || attr.is_hair_intercept) {
      iface_type = input_type = GPU_FLOAT;
      load_ss << " = attr_load_" << input_type << "(domain, " << attr_name << ", " << attr_n
              << ");\n";
    }
    else {
      switch (attr.type) {
        case CD_ORCO:
          /* Need vec4 to detect usage of default attribute. */
          input_type = GPU_VEC4;
          iface_type = GPU_VEC3;
          load_ss << " = attr_load_orco(domain, " << attr_name << ", " << attr_n << ");\n";
          break;
        case CD_TANGENT:
          iface_type = input_type = GPU_VEC4;
          load_ss << " = attr_load_tangent(domain, " << attr_name << ", " << attr_n << ");\n";
          break;
        default:
          iface_type = input_type = GPU_VEC4;
          load_ss << " = attr_load_" << input_type << "(domain, " << attr_name << ", " << attr_n
                  << ");\n";
          break;
      }
    }
    attr_n++;

    info.vertex_in(slot--, to_type(input_type), attr_name);
    iface.smooth(to_type(iface_type), var_name);
  }

  output.attr_load = load_ss.str();
}

void GPUCodegen::generate_resources()
{
  GPUCodegenCreateInfo &info = *create_info;

  std::stringstream ss;

  /* Textures. */
  int slot = 0;
  for (GPUMaterialTexture &tex : graph.textures) {
    const int data_source_count = (tex.ima != nullptr) + (tex.colorband != nullptr) +
                                  (tex.sky != nullptr);
    const bool valid_3d_lut_state = !tex.use_3d_lut_strip ||
                                    (tex.ima != nullptr && tex.colorband == nullptr &&
                                     tex.sky == nullptr && tex.tiled_mapping_name[0] == '\0' &&
                                     tex.lut_3d_width > 0 && tex.lut_3d_height > 0 &&
                                     tex.lut_3d_depth > 0);
    if (data_source_count != 1 || tex.sampler_name[0] == '\0' || !valid_3d_lut_state) {
      BLI_assert_msg(0, "Invalid material texture resource");
      continue;
    }
    if (tex.colorband) {
      const char *name = info.name_buffer.append_sampler_name(tex.sampler_name);
      info.sampler(slot++, ImageType::Float1DArray, name, Frequency::BATCH);
    }
    else if (tex.sky) {
      const char *name = info.name_buffer.append_sampler_name(tex.sampler_name);
      info.sampler(0, ImageType::Float2DArray, name, Frequency::BATCH);
    }
    else if (tex.tiled_mapping_name[0] != '\0') {
      const char *name = info.name_buffer.append_sampler_name(tex.sampler_name);
      info.sampler(slot++, ImageType::Float2DArray, name, Frequency::BATCH);

      const char *name_mapping = info.name_buffer.append_sampler_name(tex.tiled_mapping_name);
      info.sampler(slot++, ImageType::Float1DArray, name_mapping, Frequency::BATCH);
    }
    else {
      const char *name = info.name_buffer.append_sampler_name(tex.sampler_name);
      BLI_assert(tex.ima != nullptr);
      info.sampler(slot++,
                   tex.use_3d_lut_strip ? ImageType::Float3D : ImageType::Float2D,
                   name,
                   Frequency::BATCH);
    }
  }

  /* Increment heuristic. */
  textures_total_ = slot;

  if (!ubo_inputs_.is_empty()) {
    const char *linted_struct_suffix = "_host_shared_";
    /* NOTE: generate_uniform_buffer() should have sorted the inputs before this. */
    ss << "struct NodeTree {\n";
    for (LinkData &link : ubo_inputs_) {
      GPUInput *input = static_cast<GPUInput *>(link.data);
      if (input->source == GPU_SOURCE_CRYPTOMATTE) {
        ss << input->type << " crypto_hash;\n";
      }
      else {
        ss << input->type << " u" << input->id << (input->is_duplicate ? "b" : "") << ";\n";
      }
    }
    ss << "};\n";
    ss << "#define NodeTree" << linted_struct_suffix << " NodeTree\n";
    ss << "#define NodeTree" << linted_struct_suffix << "uniform_ NodeTree\n";
    ss << "\n";

    info.uniform_buf(GPU_NODE_TREE_UBO_SLOT, "NodeTree", GPU_UBO_BLOCK_NAME, Frequency::BATCH);
  }

  if (!graph.uniform_attrs.list.is_empty()) {
    ss << "struct UniformAttrs {\n";
    for (GPUUniformAttr &attr : graph.uniform_attrs.list) {
      ss << "vec4 attr" << attr.id << ";\n";
    }
    ss << "};\n\n";
    ss << "#define UniformAttrs_host_shared_ UniformAttrs\n";
    ss << "#define UniformAttrs_host_shared_uniform_ UniformAttrs\n\n";

    /* TODO(fclem): Use the macro for length. Currently not working for EEVEE. */
    /* DRW_RESOURCE_CHUNK_LEN = 512 */
    info.uniform_buf(2, "UniformAttrs", GPU_ATTRIBUTE_UBO_BLOCK_NAME "[512]", Frequency::BATCH);
  }

  if (!graph.layer_attrs.is_empty()) {
    info.additional_info("draw_layer_attributes");
  }

  info.typedef_source_generated = ss.str();
}

void GPUCodegen::node_serialize(Set<StringRefNull> &used_libraries,
                                std::stringstream &eval_ss,
                                const GPUNode *node)
{
  if (node->use_static_function) {
    gpu_material_library_use_function(used_libraries, node->name);
  }
  else if (node->dependency_name[0] != '\0') {
    used_libraries.add(node->dependency_name);
  }
  if (node->dependency_flags & GPU_CUSTOM_NODE_DEPENDENCY_GLSL_GEOMETRY_HELPERS) {
    used_libraries.add(GPU_GLSL_FUNCTION_GEOMETRY_HELPER_FILENAME);
  }
  if (node->dependency_flags & GPU_CUSTOM_NODE_DEPENDENCY_GLSL_LIGHTPROBE_HELPERS) {
    used_libraries.add(GPU_GLSL_FUNCTION_LIGHTPROBE_HELPER_FILENAME);
  }

  auto source_reference = [&](GPUInput *input) {
    BLI_assert(ELEM(input->source, GPU_SOURCE_OUTPUT, GPU_SOURCE_ATTR));
    /* These inputs can have non matching types. Do conversion. */
    GPUType to = input->type;
    GPUType from = (input->source == GPU_SOURCE_ATTR) ? input->attr->gputype :
                                                        input->link->output->type;
    if (from != to) {
      /* Use defines declared inside codegen_lib (e.g. vec4_from_float). */
      eval_ss << to << "_from_" << from << "(";
    }

    if (input->source == GPU_SOURCE_ATTR) {
      eval_ss << input;
    }
    else {
      eval_ss << input->link->output;
    }

    if (from != to) {
      /* Special case that needs luminance coefficients as argument. */
      if ((from == GPU_VEC4 || from == GPU_TEX_HANDLE) && to == GPU_FLOAT) {
        float coefficients[3];
        IMB_colormanagement_get_luminance_coefficients(coefficients);
        eval_ss << ", " << Span<float>(coefficients, 3);
      }
      eval_ss << ")";
    }
  };

  /* Declare constants. */
  for (GPUInput &input : node->inputs) {
    auto type = [&]() {
      /* Don't declare zone io variables twice. */
      std::stringstream ss;
      if (!input.is_duplicate) {
        ss << input.type;
      }
      return ss.str();
    };
    switch (input.source) {
      case GPU_SOURCE_FUNCTION_CALL:
      {
        std::stringstream output_name_ss;
        output_name_ss << &input;
        eval_ss << type() << " " << &input << "; "
                << gpu_function_call_expand(input.function_call, output_name_ss.str()) << ";\n";
        break;
      }
      case GPU_SOURCE_STRUCT:
        eval_ss << type() << " " << &input << " = "
                << (input.type == GPU_CLOSURE ? "CLOSURE_DEFAULT" : "TEXTURE_HANDLE_DEFAULT")
                << ";\n";
        break;
      case GPU_SOURCE_CONSTANT:
        if (!input.is_duplicate) {
          eval_ss << type() << " " << &input << " = " << static_cast<GPUConstant *>(&input)
                  << ";\n";
        }
        break;
      case GPU_SOURCE_OUTPUT:
      case GPU_SOURCE_ATTR:
        if (input.is_zone_io) {
          eval_ss << type() << " " << &input << " = ";
          source_reference(&input);
          eval_ss << ";\n";
        }
        break;
      default:
        if (input.is_zone_io && (!input.is_duplicate || !input.link)) {
          eval_ss << type() << " zone" << input.id << " = " << &input << ";\n";
        }
        break;
    }
  }
  /* Declare temporary variables for node output storage. */
  for (GPUOutput &output : node->outputs) {
    if (output.is_zone_io) {
      break;
    }
    eval_ss << output.type << " " << &output << ";\n";
  }

  if (node->skip_call) {
    return;
  }

  /* Function call. */
  eval_ss << node->name << "(";
  /* Input arguments. */
  for (GPUInput &input : node->inputs) {
    if (input.is_zone_io) {
      break;
    }
    switch (input.source) {
      case GPU_SOURCE_OUTPUT:
      case GPU_SOURCE_ATTR: {
        source_reference(&input);
        break;
      }
      default:
        eval_ss << &input;
        break;
    }
    GPUOutput *output = static_cast<GPUOutput *>(node->outputs.first);
    if ((input.next && !input.next->is_zone_io) || (output && !output->is_zone_io)) {
      eval_ss << ", ";
    }
  }
  /* Output arguments. */
  for (GPUOutput &output : node->outputs) {
    if (output.is_zone_io) {
      break;
    }
    eval_ss << &output;
    if (output.next && !output.next->is_zone_io) {
      eval_ss << ", ";
    }
  }
  eval_ss << ");\n\n";

  /* Increment heuristic. */
  nodes_total_++;
}

static Vector<StringRefNull> set_to_vector_stable(Set<StringRefNull> &set)
{
  Vector<StringRefNull> source_files;
  for (const StringRefNull &str : set) {
    source_files.append(str);
  }
  /* Sort dependencies to avoid random order causing shader caching to fail (see #108289). */
  std::ranges::sort(source_files);
  return source_files;
}

GPUGraphOutput GPUCodegen::graph_serialize(GPUNodeTag tree_tag,
                                           GPUNodeLink *output_link,
                                           const char *output_default)
{
  if (output_link == nullptr && output_default == nullptr) {
    return {};
  }

  Set<StringRefNull> used_libraries;
  std::stringstream eval_ss;
  bool has_nodes = false;
  /* NOTE: The node order is already top to bottom (or left to right in node editor)
   * because of the evaluation order inside ntreeExecGPUNodes(). */
  for (GPUNode &node : graph.nodes) {
    if ((node.tag & tree_tag) == 0) {
      continue;
    }
    node_serialize(used_libraries, eval_ss, &node);
    has_nodes = true;
  }

  if (!has_nodes) {
    return {};
  }

  if (output_link) {
    eval_ss << "return " << output_link->output << ";\n";
  }
  else {
    /* Default output in case there are only AOVs. */
    eval_ss << "return " << output_default << ";\n";
  }

  std::string str = eval_ss.str();
  BLI_hash_mm2a_add(&hm2a_, reinterpret_cast<const uchar *>(str.c_str()), str.size());
  return {str, set_to_vector_stable(used_libraries)};
}

GPUGraphOutput GPUCodegen::graph_serialize(GPUNodeTag tree_tag)
{
  std::stringstream eval_ss;
  Set<StringRefNull> used_libraries;
  for (GPUNode &node : graph.nodes) {
    if (node.tag & tree_tag) {
      node_serialize(used_libraries, eval_ss, &node);
    }
  }
  std::string str = eval_ss.str();
  BLI_hash_mm2a_add(&hm2a_, reinterpret_cast<const uchar *>(str.c_str()), str.size());
  return {str, set_to_vector_stable(used_libraries)};
}

void GPUCodegen::generate_cryptomatte()
{
  cryptomatte_input_ = MEM_new_zeroed<GPUInput>(__func__);
  cryptomatte_input_->type = GPU_FLOAT;
  cryptomatte_input_->source = GPU_SOURCE_CRYPTOMATTE;

  float material_hash = 0.0f;
  Material *material = GPU_material_get_material(&mat);
  if (material) {
    bke::cryptomatte::CryptomatteHash hash(material->id.name + 2,
                                           BLI_strnlen(material->id.name + 2, MAX_NAME - 2));
    material_hash = hash.float_encoded();
  }
  cryptomatte_input_->vec[0] = material_hash;

  BLI_addtail(&ubo_inputs_, BLI_genericNodeN(cryptomatte_input_));
}

void GPUCodegen::generate_uniform_buffer()
{
  /* Extract uniform inputs. */
  for (GPUNode &node : graph.nodes) {
    for (GPUInput &input : node.inputs) {
      if (input.source == GPU_SOURCE_UNIFORM && !input.link) {
        /* We handle the UBO uniforms separately. */
        BLI_addtail(&ubo_inputs_, BLI_genericNodeN(&input));
        uniforms_total_++;
      }
    }
  }
  if (!ubo_inputs_.is_empty()) {
    /* This sorts the inputs based on size. */
    GPU_material_uniform_buffer_create(&mat, &ubo_inputs_);
  }
}

/* Sets id for unique names for all inputs, resources and temp variables. */
void GPUCodegen::set_unique_ids()
{
  Map<int, GPUNode *> zone_starts;
  Map<int, GPUNode *> zone_ends;

  int id = 1;
  for (GPUNode &node : graph.nodes) {
    for (GPUInput &input : node.inputs) {
      input.id = id++;
    }
    for (GPUOutput &output : node.outputs) {
      output.id = id++;
    }
    if (node.zone_index != -1) {
      auto &map = node.is_zone_end ? zone_ends : zone_starts;
      map.add(node.zone_index, &node);
    }
  }

  auto find_zone_io = [](auto first) {
    while (first && !first->is_zone_io && first->next) {
      first = first->next;
    }
    return first;
  };

  /* Assign the same id to inputs and outputs of start and end zones. */
  for (GPUNode *end : zone_ends.values()) {

    GPUInput *end_input = find_zone_io(static_cast<GPUInput *>(end->inputs.first));
    GPUOutput *end_output = find_zone_io(static_cast<GPUOutput *>(end->outputs.first));

    if (!zone_starts.contains(end->zone_index)) {
      /* The zone input is disconnected, skip the call. */
      end->skip_call = true;
      for (; end_input; end_input = end_input->next, end_output = end_output->next) {
        end_output->id = end_input->id;
        end_output->is_duplicate = true;
      }
      continue;
    }

    GPUNode *start = zone_starts.lookup(end->zone_index);

    GPUInput *start_input = find_zone_io(static_cast<GPUInput *>(start->inputs.first));
    GPUOutput *start_output = find_zone_io(static_cast<GPUOutput *>(start->outputs.first));

    for (; start_input; start_input = start_input->next,
                        start_output = start_output->next,
                        end_input = end_input->next,
                        end_output = end_output->next)
    {
      start_output->id = start_input->id;
      start_output->is_duplicate = true;
      end_input->id = start_input->id;
      end_input->is_duplicate = true;
      end_output->id = start_input->id;
      end_output->is_duplicate = true;
    }
  }

  for (GPUNode *start : zone_starts.values()) {
    if (!zone_ends.contains(start->zone_index)) {
      /* The zone output is disconnected, skip the call. */
      GPUInput *start_input = find_zone_io(static_cast<GPUInput *>(start->inputs.first));
      GPUOutput *start_output = find_zone_io(static_cast<GPUOutput *>(start->outputs.first));
      start->skip_call = true;
      for (; start_input; start_input = start_input->next, start_output = start_output->next) {
        start_output->id = start_input->id;
        start_output->is_duplicate = true;
      }
    }
  }
}

void GPUCodegen::generate_graphs()
{
  set_unique_ids();

  output.surface = graph_serialize(
      GPU_NODE_TAG_SURFACE | GPU_NODE_TAG_AOV | GPU_NODE_TAG_OUTLINE,
      graph.outlink_surface,
      "CLOSURE_DEFAULT");
  output.volume = graph_serialize(GPU_NODE_TAG_VOLUME, graph.outlink_volume, "CLOSURE_DEFAULT");
  output.displacement = graph_serialize(
      GPU_NODE_TAG_DISPLACEMENT, graph.outlink_displacement, nullptr);
  output.thickness = graph_serialize(GPU_NODE_TAG_THICKNESS, graph.outlink_thickness, nullptr);
  if (graph.outlink_depth_offset != nullptr) {
    output.depth_offset = graph_serialize(
        GPU_NODE_TAG_DEPTH_OFFSET, graph.outlink_depth_offset, nullptr);
  }
  output.npr = graph_serialize(
      GPU_NODE_TAG_NPR | GPU_NODE_TAG_OUTLINE, graph.outlink_npr, "float4(0.0f)");
  if (!BLI_listbase_is_empty(&graph.outlink_filters)) {
    for (GPUNodeGraphOutputLink &filter_link : graph.outlink_filters) {
      for (GPUNode &node : graph.nodes) {
        node.tag &= ~GPU_NODE_TAG_FILTER;
      }
      gpu_nodes_tag(&graph, filter_link.outlink, GPU_NODE_TAG_FILTER);
      GPUGraphOutput filter_graph = graph_serialize(GPU_NODE_TAG_FILTER,
                                                    filter_link.outlink,
                                                    nullptr);
      output.filter_output_identifiers.append(filter_link.hash);
      output.filter_outputs.append(filter_graph);
    }
    for (GPUNodeGraphOutputLink &filter_link : graph.outlink_filters) {
      gpu_nodes_tag(&graph, filter_link.outlink, GPU_NODE_TAG_FILTER);
    }
    if (!output.filter_outputs.is_empty()) {
      output.filter = output.filter_outputs.first();
    }
  }
  else {
    output.filter = graph_serialize(GPU_NODE_TAG_FILTER | GPU_NODE_TAG_AOV,
                                    graph.outlink_filter,
                                    nullptr);
  }
  if (graph.outlink_light_shader != nullptr) {
    output.light_shader = graph_serialize(
        GPU_NODE_TAG_LIGHT_SHADER, graph.outlink_light_shader, "float4(1.0f)");
  }
  if (!graph.outlink_compositor.is_empty()) {
    output.composite = graph_serialize(GPU_NODE_TAG_COMPOSITOR);
  }

  if (!graph.material_functions.is_empty()) {
    for (GPUNodeGraphFunctionLink &func_link : graph.material_functions) {
      std::stringstream eval_ss;
      /* Untag every node in the graph to avoid serializing nodes from other functions */
      for (GPUNode &node : graph.nodes) {
        node.tag &= ~GPU_NODE_TAG_FUNCTION;
      }
      /* Tag only the nodes needed for the current function */
      gpu_nodes_tag(&graph, func_link.outlink, GPU_NODE_TAG_FUNCTION);
      GPUGraphOutput graph = graph_serialize(GPU_NODE_TAG_FUNCTION, func_link.outlink);
      if (func_link.dependency_name[0] != '\0') {
        graph.dependencies.append_non_duplicates(func_link.dependency_name);
      }
      eval_ss << func_link.return_type << " " << func_link.name << "() {\n"
              << graph.serialized << "}\n\n";
      output.material_functions.append({eval_ss.str(), graph.dependencies});
    }
    /* Leave the function tags as they were before serialization */
    for (GPUNodeGraphFunctionLink &funclink : graph.material_functions) {
      gpu_nodes_tag(&graph, funclink.outlink, GPU_NODE_TAG_FUNCTION);
    }
  }

  for (int i = 0; i < GPU_material_generated_source_count(&mat); i++) {
    const GPUMaterialGeneratedSource *generated_source = GPU_material_generated_source_get(&mat, i);
    if (generated_source == nullptr) {
      continue;
    }
    BLI_hash_mm2a_add(
        &hm2a_,
        reinterpret_cast<const uchar *>(generated_source->filename.c_str()),
        generated_source->filename.size());
    for (const std::string &dependency : generated_source->dependencies) {
      BLI_hash_mm2a_add(
          &hm2a_, reinterpret_cast<const uchar *>(dependency.c_str()), dependency.size());
    }
    BLI_hash_mm2a_add(
        &hm2a_,
        reinterpret_cast<const uchar *>(generated_source->content.c_str()),
        generated_source->content.size());
  }

  for (GPUMaterialAttribute &attr : graph.attributes) {
    BLI_hash_mm2a_add(&hm2a_, reinterpret_cast<uchar *>(attr.name), strlen(attr.name));
  }

  hash_ = BLI_hash_mm2a_end(&hm2a_);
}

/** \} */

}  // namespace blender
