/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_geometry_lines_graph.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "DNA_collection_types.h"
#include "DNA_color_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_texture_types.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.hh"
#include "BKE_node.hh"
#include "BKE_colortools.hh"
#include "BKE_colorband.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "DEG_depsgraph_query.hh"

namespace blender::eevee::geometry_lines {
namespace {

uint64_t hash_bytes(uint64_t hash, const void *bytes, size_t length)
{
  const auto *data = static_cast<const unsigned char *>(bytes);
  for (size_t i = 0; i < length; i++) {
    hash = (hash ^ data[i]) * 1099511628211ULL;
  }
  return hash;
}
template<class T> void hash_value(uint64_t &hash, const T &value)
{
  hash = hash_bytes(hash, &value, sizeof(value));
}

const bNodeSocket *input(const bNode &node, const char *identifier)
{
  for (const bNodeSocket &socket : node.inputs) {
    if (STREQ(socket.identifier, identifier)) {
      return &socket;
    }
  }
  return nullptr;
}

template<class T> T *original(T *id)
{
  return id ? reinterpret_cast<T *>(DEG_get_original_id(&id->id)) : nullptr;
}

struct Context {
  const bNodeTree *tree = nullptr;
  const Context *parent = nullptr;
  const bNode *group = nullptr;
  uint64_t identity = 14695981039346656037ULL;
};

struct SocketRef {
  const Context *context = nullptr;
  const bNodeSocket *socket = nullptr;
  explicit operator bool() const { return socket != nullptr; }
  const bNode &node() const { return socket->owner_node(); }
};

class Compiler {
  Graph graph_;
  int solo_node_ = 0;
  Context root_;
  std::vector<std::unique_ptr<Context>> contexts_;
  using Key = std::pair<const Context *, const bNode *>;
  std::map<Key, Context *> groups_;
  std::map<Key, std::shared_ptr<ObjectScope>> sources_;
  std::map<Key, int> brushes_;
  std::map<Key, int> reductions_;
  std::map<Key, int> maps_;

  struct FieldBuild {
    BrushFields &fields;
    const Context *closure_context = nullptr;
    const bNode *closure_input = nullptr;
    bool invalid_closure = false;
    std::map<std::pair<const Context *, const bNodeSocket *>, int> values;
    std::map<Key, int> positions;
    std::set<std::pair<const Context *, const bNodeSocket *>> visiting;
  };

  int emit(FieldBuild &build, int opcode, int a = -1, int b = -1, int c = -1,
           float4 value = float4(0), int option = 0, int component = 0, int flags = 0,
           int d = -1, int e = -1, int f = -1)
  {
    if (build.fields.registers >= LINE_FIELD_MAX_REGISTERS) {
      error("Brush region exceeds 128 field registers; simplify the connected graph");
      return 0;
    }
    const int result = build.fields.registers++;
    GeometryLineFieldInstruction instruction{};
    instruction.op = int4(opcode, a, b, c);
    instruction.meta = int4(result, option, component, flags);
    instruction.value = value;
    instruction.extra = int4(d, e, f, 0);
    build.fields.instructions.push_back(instruction);
    return result;
  }

  int constant(FieldBuild &build, const bNodeSocket *socket)
  {
    float4 value(0);
    if (socket && socket->default_value) {
      switch (socket->type) {
        case SOCK_FLOAT: value = float4(socket->default_value_typed<bNodeSocketValueFloat>()->value); break;
        case SOCK_INT: value = float4(float(socket->default_value_typed<bNodeSocketValueInt>()->value)); break;
        case SOCK_BOOLEAN: value = float4(socket->default_value_typed<bNodeSocketValueBoolean>()->value ? 1 : 0); break;
        case SOCK_VECTOR: value = float4(float3(socket->default_value_typed<bNodeSocketValueVector>()->value), 0); break;
        case SOCK_RGBA: value = float4(socket->default_value_typed<bNodeSocketValueRGBA>()->value); break;
        default: break;
      }
    }
    for (int i = 0; i < 4; i++) { if (!std::isfinite(value[i])) { value[i] = 0; } }
    return emit(build, LINE_FIELD_CONSTANT, -1, -1, -1, value);
  }

  int position(FieldBuild &build, SocketRef ref, int depth = 0)
  {
    if (!ref || ref.socket->in_out != SOCK_OUT) {
      return emit(build, LINE_FIELD_INFO, -1, -1, -1, float4(0), 0);
    }
    if (depth > 128) { error("Brush position chain contains a cycle"); return constant(build, nullptr); }
    const Key key{ref.context, &ref.node()};
    if (auto found = build.positions.find(key); found != build.positions.end()) {
      if (found->second < 0) { error("Brush position depends on its own result"); return constant(build, nullptr); }
      return found->second;
    }
    build.positions[key] = -1;
    const bNode &node = ref.node();
    int result;
    if (ELEM(node.type_legacy, NPR_LINE_NODE_BRUSH_INPUT, NPR_LINE_NODE_BRUSH)) {
      result = emit(build, LINE_FIELD_INFO, -1, -1, -1, float4(0), 0);
    }
    else if (ELEM(node.type_legacy, NPR_LINE_NODE_SET_POSITION, NPR_LINE_NODE_RESAMPLE)) {
      const int incoming = position(build, value(*ref.context, node, "brush"), depth + 1);
      if (node.type_legacy == NPR_LINE_NODE_RESAMPLE) {
        build.fields.resample = std::clamp(number(*ref.context, node, "length", 1), 0.1f, 1000.0f);
        result = incoming;
      }
      else {
        const bNodeSocket *target = input(node, "position");
        const int desired = target && target->is_directly_linked() ? field(build, *ref.context, target) : incoming;
        const int offset = field(build, *ref.context, input(node, "offset"));
        const int selection = field(build, *ref.context, input(node, "selection"));
        result = emit(build, LINE_FIELD_POSITION, incoming, desired, offset, float4(0), 0, 0, 0, selection);
      }
    }
    else { error("Brush processing must start from a Brush Input or Line Brush"); result = constant(build, nullptr); }
    build.positions[key] = result;
    return result;
  }

  int field(FieldBuild &build, const Context &context, const bNodeSocket *socket)
  {
    if (!socket) { return constant(build, nullptr); }
    SocketRef ref = resolve(context, socket);
    if (!ref) { return constant(build, socket); }
    const auto key = std::make_pair(ref.context, ref.socket);
    int result;
    if (auto found = build.values.find(key); found != build.values.end()) { result = found->second; }
    else {
      if (!build.visiting.insert(key).second) {
        error("Brush field contains a circular dependency on a later position stage");
        return constant(build, nullptr);
      }
      const bNode &node = ref.node();
      auto arg = [&](const char *name) { return field(build, *ref.context, input(node, name)); };
      auto idx = [&](int index) { return field(build, *ref.context, node.input_sockets()[index]); };
      const char *output = ref.socket->identifier;
      if (ref.socket->in_out == SOCK_IN ||
          ELEM(node.type_legacy, SH_NODE_VALUE, SH_NODE_RGB, NODE_GROUP_INPUT)) {
        result = constant(build, ref.socket);
      }
      else if (ELEM(node.type_legacy, NPR_LINE_NODE_STROKE_INFO,
                    NPR_LINE_NODE_BRUSH_CLOSURE_INPUT)) {
        const bool parameter = node.type_legacy == NPR_LINE_NODE_BRUSH_CLOSURE_INPUT;
        if (parameter && (build.closure_input != &node || build.closure_context != ref.context)) {
          error("Brush parameter belongs to a different closure; reuse its node group instead");
          build.invalid_closure = true;
          return constant(build, nullptr);
        }
        const int current = parameter ? -1 : position(build, value(*ref.context, node, "brush"));
        const char *names[] = {"position", "Distance", "Length", "Factor", "ID", "Start", "End", "Closed"};
        int which = 0;
        for (int i = 0; i < 8; i++) { if (STREQ(output, names[i])) { which = i; } }
        result = emit(build, LINE_FIELD_INFO, current, -1, -1, float4(0), which);
      }
      else if (node.type_legacy == SH_NODE_MATH) {
        result = emit(build, LINE_FIELD_MATH, idx(0), idx(1), idx(2), float4(0), node.custom1, 0, node.custom2);
      }
      else if (node.type_legacy == SH_NODE_VECTOR_MATH) {
        result = emit(build, LINE_FIELD_VECTOR_MATH, idx(0), idx(1), idx(2), float4(0), node.custom1, 0, 0, idx(3));
      }
      else if (node.type_legacy == SH_NODE_TEX_WHITE_NOISE) {
        result = emit(build, LINE_FIELD_WHITE_NOISE, idx(0), idx(1), -1,
                      float4(0), node.custom1, STREQ(output, "Color") ? 1 : 0);
      }
      else if (node.type_legacy == SH_NODE_SEPXYZ) {
        result = emit(build, LINE_FIELD_SEPARATE, idx(0), -1, -1, float4(0), 0, STREQ(output, "Y") ? 1 : STREQ(output, "Z") ? 2 : 0);
      }
      else if (node.type_legacy == SH_NODE_COMBXYZ) { result = emit(build, LINE_FIELD_COMBINE, idx(0), idx(1), idx(2)); }
      else if (node.type_legacy == SH_NODE_VALTORGB) {
        const int first = int(build.fields.ramps.size());
        const auto &band = *static_cast<const ColorBand *>(node.storage);
        for (int i = 0; i <= 256; i++) {
          float4 color;
          BKE_colorband_evaluate(&band, float(i) / 256, color);
          build.fields.ramps.push_back(color);
        }
        result = emit(build, LINE_FIELD_RAMP, idx(0), -1, -1, float4(0), first, STREQ(output, "Alpha") ? 1 : 0);
      }
      else if (node.type_legacy == SH_NODE_CURVE_FLOAT) {
        auto *mapping = static_cast<CurveMapping *>(node.storage);
        BKE_curvemapping_init(mapping);
        float minimum[CM_TOT], divider[CM_TOT], start[CM_TOT], end[CM_TOT];
        BKE_curvemapping_get_range_minimums(mapping, minimum);
        BKE_curvemapping_compute_range_dividers(mapping, divider);
        BKE_curvemapping_compute_slopes(mapping, start, end);
        const int first = int(build.fields.ramps.size());
        for (int i = 0; i <= 256; i++) {
          build.fields.ramps.push_back(float4(BKE_curvemapping_evaluateF(
              mapping, 0, minimum[0] + float(i) / (256 * divider[0]))));
        }
        result = emit(build, LINE_FIELD_CURVE, idx(1), idx(0), -1,
                      float4(minimum[0], divider[0], start[0], end[0]), first);
      }
      else if (node.type_legacy == SH_NODE_TEX_IMAGE) {
        Image *image = original(reinterpret_cast<Image *>(node.id));
        auto found = std::find(build.fields.images.begin(), build.fields.images.end(), image);
        int index = int(found - build.fields.images.begin());
        if (found == build.fields.images.end()) { build.fields.images.push_back(image); }
        if (index >= LINE_FIELD_MAX_IMAGES) { error("A brush region supports at most four connected images"); index = -1; }
        const auto &settings = *static_cast<const NodeTexImage *>(node.storage);
        if (settings.projection != SHD_PROJ_FLAT || (image && image->source == IMA_SRC_TILED)) {
          error("Brush Image Texture requires a flat source UV image; tiled images are not supported");
        }
        result = emit(build, LINE_FIELD_IMAGE, idx(0), -1, -1, float4(0), index,
                      STREQ(output, "Alpha") ? 1 : 0, settings.extension | (settings.interpolation << 8));
      }
      else if (ELEM(node.type_legacy, NPR_LINE_NODE_UV, NPR_LINE_NODE_COLOR_ATTRIBUTE, NPR_LINE_NODE_AOV)) {
        FieldSource source;
        source.type = node.type_legacy == NPR_LINE_NODE_UV ? 0 : node.type_legacy == NPR_LINE_NODE_COLOR_ATTRIBUTE ? 1 : 2;
        source.attribute.name = string(*ref.context, node, "name");
        source.aov_type = static_cast<const NodeNPRLine *>(node.storage)->mode;
        int index = 0;
        for (; index < int(build.fields.sources.size()); index++) {
          const auto &other = build.fields.sources[index];
          if (other.type == source.type && other.attribute.name == source.attribute.name && other.aov_type == source.aov_type) { break; }
        }
        if (index == int(build.fields.sources.size())) { build.fields.sources.push_back(source); }
        if (index >= LINE_FIELD_MAX_SOURCES) { error("A brush region supports at most eight source attributes or AOVs"); index = -1; }
        const int component = STREQ(output, "Valid") ? 2 : STREQ(output, "Alpha") ? 1 : STREQ(output, "Value") ? 3 : 0;
        result = emit(build, source.type == 2 ? LINE_FIELD_AOV : LINE_FIELD_SOURCE,
                      source.type == 2 ? arg(source.aov_type ? "value" : "color") : -1,
                      -1, -1, float4(0), index, component, source.aov_type);
      }
      else if (node.type_legacy == SH_NODE_MAP_RANGE) {
        const auto &settings = *static_cast<const NodeMapRange *>(node.storage);
        const int first = settings.data_type == CD_PROP_FLOAT ? 0 : 6;
        result = emit(build, LINE_FIELD_MAP_RANGE, idx(first), idx(first + 1), idx(first + 2),
                      float4(0), settings.interpolation_type, 0, settings.clamp,
                      idx(first + 3), idx(first + 4), idx(first + 5));
      }
      else if (node.type_legacy == SH_NODE_MIX) {
        const auto &settings = *static_cast<const NodeShaderMix *>(node.storage);
        const int first = settings.data_type == SOCK_FLOAT ? 2 : settings.data_type == SOCK_VECTOR ? 4 : 6;
        result = emit(build, LINE_FIELD_MIX, idx(settings.data_type == SOCK_VECTOR && settings.factor_mode ? 1 : 0),
                      idx(first), idx(first + 1), float4(0), settings.data_type == SOCK_RGBA ? settings.blend_type : 0, 0,
                      settings.clamp_factor | (settings.clamp_result << 1));
      }
      else { error("Unsupported field node in Brush region"); result = constant(build, nullptr); }
      build.visiting.erase(key);
      build.values[key] = result;
    }
    /* Match Blender's numeric socket conversions at every link, including group boundaries. */
    if (socket->type != ref.socket->type && socket->in_out == SOCK_IN) {
      result = emit(build, LINE_FIELD_CONVERT, result, -1, -1, float4(0), socket->type, ref.socket->type);
    }
    return result;
  }

  void error(const char *message)
  {
    if (graph_.error.empty()) {
      graph_.error = message;
    }
  }

  SocketRef resolve(const Context &context, const bNodeSocket *socket, int depth = 0)
  {
    if (!socket) {
      return {};
    }
    if (depth > 128) {
      error("Geometry line graph contains a cycle or exceeds 128 nested links");
      return {};
    }
    if (socket->in_out == SOCK_IN) {
      for (const bNodeLink *link : socket->directly_linked_links()) {
        if (!(link->flag & NODE_LINK_MUTED)) {
          return resolve(context, link->fromsock, depth + 1);
        }
      }
      return {&context, socket};
    }
    const bNode &node = socket->owner_node();
    if (node.type_legacy == NODE_REROUTE) {
      return resolve(context, static_cast<const bNodeSocket *>(node.inputs.first_), depth + 1);
    }
    if (node.flag & NODE_MUTED) {
      for (const bNodeInternalLink &link : node.internal_links()) {
        if (link.out == socket) { return resolve(context, link.in, depth + 1); }
      }
      return {};
    }
    if (node.type_legacy == NODE_GROUP_INPUT && context.parent && context.group) {
      return resolve(*context.parent, input(*context.group, socket->identifier), depth + 1);
    }
    if (node.type_legacy == NODE_GROUP && node.id) {
      const auto *tree = reinterpret_cast<const bNodeTree *>(node.id);
      for (const Context *parent = &context; parent; parent = parent->parent) {
        if (parent->tree == tree) {
          error("Recursive geometry line node groups are not supported");
          return {};
        }
      }
      Context *child;
      const Key key{&context, &node};
      const auto found = groups_.find(key);
      if (found != groups_.end()) {
        child = found->second;
      }
      else {
        auto created = std::make_unique<Context>();
        created->parent = &context;
        created->group = &node;
        created->tree = tree;
        created->identity = context.identity;
        hash_value(created->identity, node.identifier);
        child = created.get();
        contexts_.push_back(std::move(created));
        groups_[key] = child;
        tree->ensure_topology_cache();
      }
      const bNode *output = tree->group_output_node();
      return output ? resolve(*child, input(*output, socket->identifier), depth + 1) : SocketRef{};
    }
    return {&context, socket};
  }

  SocketRef value(const Context &context, const bNode &node, const char *identifier)
  {
    return resolve(context, input(node, identifier));
  }

  float number(const Context &context, const bNode &node, const char *identifier, float fallback = 0)
  {
    SocketRef ref = value(context, node, identifier);
    if (!ref || !ref.socket->default_value) {
      return fallback;
    }
    switch (ref.socket->type) {
      case SOCK_FLOAT: {
        const float value = static_cast<const bNodeSocketValueFloat *>(ref.socket->default_value)->value;
        return std::isfinite(value) ? value : fallback;
      }
      case SOCK_INT: return float(static_cast<const bNodeSocketValueInt *>(ref.socket->default_value)->value);
      case SOCK_BOOLEAN: return static_cast<const bNodeSocketValueBoolean *>(ref.socket->default_value)->value ? 1 : 0;
      default: return fallback;
    }
  }

  float4 color(const Context &context, const bNode &node, const char *identifier)
  {
    SocketRef ref = value(context, node, identifier);
    if (ref && ref.socket->type == SOCK_RGBA && ref.socket->default_value) {
      return float4(static_cast<const bNodeSocketValueRGBA *>(ref.socket->default_value)->value);
    }
    return float4(0, 0, 0, 1);
  }

  ID *id(const Context &context, const bNode &node, const char *identifier)
  {
    SocketRef ref = value(context, node, identifier);
    if (!ref || !ref.socket->default_value) {
      return nullptr;
    }
    switch (ref.socket->type) {
      case SOCK_OBJECT: return reinterpret_cast<ID *>(original(static_cast<const bNodeSocketValueObject *>(ref.socket->default_value)->value));
      case SOCK_COLLECTION: return reinterpret_cast<ID *>(original(static_cast<const bNodeSocketValueCollection *>(ref.socket->default_value)->value));
      case SOCK_MATERIAL: return reinterpret_cast<ID *>(original(static_cast<const bNodeSocketValueMaterial *>(ref.socket->default_value)->value));
      case SOCK_IMAGE: return reinterpret_cast<ID *>(original(static_cast<const bNodeSocketValueImage *>(ref.socket->default_value)->value));
      default: return nullptr;
    }
  }

  std::string string(const Context &context, const bNode &node, const char *identifier)
  {
    SocketRef ref = value(context, node, identifier);
    if (ref && ref.socket->type == SOCK_STRING && ref.socket->default_value) {
      const char *text = static_cast<const bNodeSocketValueString *>(ref.socket->default_value)->value;
      return text ? text : "";
    }
    return "";
  }

  std::shared_ptr<ObjectScope> source(SocketRef ref)
  {
    if (!ref || ref.socket->in_out != SOCK_OUT || ref.node().type_legacy != NPR_LINE_NODE_OBJECTS) {
      return std::make_shared<ObjectScope>(ObjectScope{2}); /* An empty input is not the whole scene. */
    }
    const Key key{ref.context, &ref.node()};
    const auto found = sources_.find(key);
    if (found != sources_.end()) {
      return found->second;
    }
    const auto &data = *static_cast<const NodeNPRLine *>(ref.node().storage);
    auto result = std::make_shared<ObjectScope>();
    result->mode = data.mode;
    result->collection = reinterpret_cast<Collection *>(id(*ref.context, ref.node(), "collection"));
    result->exclude_collection = reinterpret_cast<Collection *>(id(*ref.context, ref.node(), "exclude_collection"));
    result->children = number(*ref.context, ref.node(), "children", 1) != 0;
    result->instances = number(*ref.context, ref.node(), "instances", 1) != 0;
    result->repair_seams = number(*ref.context, ref.node(), "repair_seams") != 0;
    for (int i = 0; i < data.item_count; i++) {
      if (ID *object = id(*ref.context, ref.node(), ("object_" + std::to_string(i)).c_str())) {
        result->objects.push_back(reinterpret_cast<Object *>(object));
      }
    }
    for (int i = 0; i < data.exclude_count; i++) {
      if (ID *object = id(*ref.context, ref.node(), ("exclude_" + std::to_string(i)).c_str())) {
        result->excluded.push_back(reinterpret_cast<Object *>(object));
      }
    }
    sources_[key] = result;
    return result;
  }

  RuleScope scope(const Context &context, const bNode &node)
  {
    RuleScope result;
    result.objects = source(value(context, node, "objects"));
    const auto &data = *static_cast<const NodeNPRLine *>(node.storage);
    for (int i = 0; i < data.material_count; i++) {
      if (ID *material = id(context, node, ("material_" + std::to_string(i)).c_str())) {
        result.materials.push_back(reinterpret_cast<Material *>(material));
      }
    }
    result.exclude_materials = number(context, node, "exclude_materials") != 0;
    return result;
  }

  int texture_map(SocketRef ref)
  {
    if (!ref || ref.socket->in_out != SOCK_OUT || ref.node().type_legacy != NPR_LINE_NODE_MAP) { return -1; }
    const auto &data = *static_cast<const NodeNPRLine *>(ref.node().storage);
    if (!data.enabled || (ref.node().flag & NODE_MUTED)) { return -1; }
    const Key key{ref.context, &ref.node()};
    const auto found = maps_.find(key);
    if (found != maps_.end()) { return found->second; }
    TextureMap map;
    map.coordinates = std::clamp(data.mode, 0, 2);
    if (map.coordinates != 2) { map.image = reinterpret_cast<Image *>(id(*ref.context, ref.node(), "image")); }
    if (map.coordinates != 2 && !map.image) { return -1; }
    const bool named = map.coordinates == 2 ? data.curve_flags != 0 : data.uv_selection != 0;
    map.uv.index = named ? -1 : int(std::clamp(number(*ref.context, ref.node(),
                                    map.coordinates == 2 ? "attribute_index" : "uv_index"), 0.0f, 7.0f));
    if (named) { map.uv.name = string(*ref.context, ref.node(), map.coordinates == 2 ? "attribute_name" : "uv_name"); }
    map.wrap_u = data.wrap_u;
    map.wrap_v = data.wrap_v;
    map.filter = data.texture_filter;
    auto bounded = [&](const char *name, float fallback, float limit) {
      return std::clamp(number(*ref.context, ref.node(), name, fallback), -limit, limit);
    };
    map.tiling = float2(bounded("tiling_u", 1, 100000), bounded("tiling_v", 1, 100000));
    map.offset = float2(bounded("offset_u", 0, 100000), bounded("offset_v", 0, 100000));
    map.rotation = bounded("rotation", 0, 3600);
    const int index = int(graph_.maps.size());
    maps_.emplace(key, index);
    graph_.maps.push_back(std::move(map));
    return index;
  }

  int reduction(SocketRef ref)
  {
    if (!ref || ref.socket->in_out != SOCK_OUT || ref.node().type_legacy != NPR_LINE_NODE_REDUCTION ||
        (ref.node().flag & NODE_MUTED)) { return -1; }
    const auto &data = *static_cast<const NodeNPRLine *>(ref.node().storage);
    if (!data.enabled || !data.width_curve) { return -1; }
    const Key key{ref.context, &ref.node()};
    const auto found = reductions_.find(key);
    if (found != reductions_.end()) { return found->second; }
    Reduction item;
    item.start = std::clamp(number(*ref.context, ref.node(), "start", 1), 0.0f, 100000.0f);
    item.end = std::clamp(number(*ref.context, ref.node(), "end", 10), 0.0f, 100000.0f);
    item.use_object = data.mode == 1;
    item.object = reinterpret_cast<Object *>(id(*ref.context, ref.node(), "object"));
    BKE_curvemapping_init(data.width_curve);
    for (int i = 0; i < int(item.curve.size()); i++) {
      const float value = BKE_curvemapping_evaluateF(data.width_curve, 0, float(i) / (item.curve.size() - 1));
      item.curve[i] = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f;
    }
    const int index = int(graph_.reductions.size());
    graph_.reductions.push_back(item);
    reductions_[key] = index;
    return index;
  }

  int brush(SocketRef ref, int depth = 0)
  {
    if (!ref || ref.socket->in_out != SOCK_OUT) {
      return -1;
    }
    if (depth > 128) { error("Brush chain contains a cycle"); return -1; }
    const Key key{ref.context, &ref.node()};
    const auto found = brushes_.find(key);
    if (found != brushes_.end()) {
      return found->second;
    }
    const bNode &node = ref.node();
    const Context &context = *ref.context;
    if (ELEM(node.type_legacy, NPR_LINE_NODE_BRUSH_OUTPUT, NPR_LINE_NODE_SET_POSITION,
             NPR_LINE_NODE_RESAMPLE, NPR_LINE_NODE_BRUSH_CLOSURE_OUTPUT)) {
      const bool closure = node.type_legacy == NPR_LINE_NODE_BRUSH_CLOSURE_OUTPUT;
      const bool standalone = closure || (node.type_legacy == NPR_LINE_NODE_BRUSH_OUTPUT &&
          (static_cast<const NodeNPRLine *>(node.storage)->curve_flags & (1 << 7)));
      Brush result{};
      SocketRef incoming;
      const bNode *parameters = nullptr;
      if (closure) {
        const auto *zone = bke::zone_type_by_node_type(node.type_legacy);
        parameters = zone->get_corresponding_input(*context.tree, node);
        if (!parameters) {
          error("Brush Closure requires its paired parameter input");
          return -1;
        }
        /* Start from a neutral continuous stroke. Legacy taper, maps, randomization
         * and distortion must never multiply the closure's explicit result fields. */
        for (float2 &sample : result.curve) { sample = float2(1); }
        /* Width fields use the same calibrated pixel coverage as the default
         * quick Simple brush. This is a raster rule, not a second style control. */
        result.raster_size_response = true;
      }
      else if (standalone) {
        result = brush_settings(context, node);
      }
      else {
        incoming = value(context, node, "brush");
        const int base = brush(incoming, depth + 1);
        if (base < 0) { error("Legacy brush chain has no connected source"); return -1; }
        result = graph_.brushes[base];
      }
      result.fields = std::make_shared<BrushFields>();
      result.fields->project_parameters = closure;
      FieldBuild build{*result.fields};
      build.closure_context = &context;
      build.closure_input = parameters;
      if (standalone) {
        const int original = emit(build, LINE_FIELD_INFO, -1, -1, -1, float4(0), 0);
        const int offset = field(build, context, input(node, "position_offset"));
        result.fields->position = emit(build, LINE_FIELD_VECTOR_MATH, original, offset, -1,
                                       float4(0), NODE_VECTOR_MATH_ADD);
        result.fields->resample = closure ? graph_.field_sample_length :
            std::clamp(number(context, node, "sample_length", 0.9f), 0.1f, 1000.0f);
      }
      else {
        result.fields->position = position(build, node.type_legacy == NPR_LINE_NODE_BRUSH_OUTPUT ? incoming : ref);
      }
      if (closure || node.type_legacy == NPR_LINE_NODE_BRUSH_OUTPUT) {
        result.fields->width = field(build, context, input(node, "width"));
        result.fields->color = field(build, context, input(node, "color"));
        result.fields->opacity = field(build, context, input(node, "opacity"));
        /* RGB and opacity are independent fields. Quick brushes retain color alpha. */
        result.color = float4(1);
        result.width = result.opacity = 1;
      }
      result.identity = context.identity;
      if (build.invalid_closure) { return -1; }
      hash_value(result.identity, node.identifier);
      auto &fields = *result.fields;
      /* Mark the dependency slice used by each evaluation stage. Appearance-only
       * image/AOV work must not run for every shape/tangent sample. */
      auto mark = [&](auto &&self, int reg, int stage) -> void {
        if (reg < 0) { return; }
        auto &instruction = fields.instructions[reg];
        if (instruction.extra.w & stage) { return; }
        instruction.extra.w |= stage;
        for (int dependency : {instruction.op.y, instruction.op.z, instruction.op.w,
                               instruction.extra.x, instruction.extra.y, instruction.extra.z}) {
          self(self, dependency, stage);
        }
      };
      mark(mark, fields.position, 1);
      mark(mark, fields.width, 2);
      mark(mark, fields.color, 4);
      mark(mark, fields.opacity, 8);
      fields.shape_signature = result.identity;
      hash_value(fields.shape_signature, fields.resample);
      for (auto instruction : fields.instructions) {
        if (!(instruction.extra.w & 3)) { continue; }
        instruction.extra.w &= 3;
        hash_value(fields.shape_signature, instruction);
        if (ELEM(instruction.op.x, LINE_FIELD_RAMP, LINE_FIELD_CURVE)) {
          fields.shape_signature = hash_bytes(fields.shape_signature,
              fields.ramps.data() + instruction.meta.y, 257 * sizeof(float4));
        }
        if (instruction.op.x == LINE_FIELD_IMAGE) {
          fields.shape_uses_image = true;
          if (instruction.meta.y >= 0) { hash_value(fields.shape_signature, fields.images[instruction.meta.y]); }
        }
        fields.shape_uses_aov |= instruction.op.x == LINE_FIELD_AOV;
      }
      /* Source descriptors are baked in draw parts even when only color uses them. */
      for (const auto &source : fields.sources) {
        hash_value(fields.shape_signature, source.type);
        hash_value(fields.shape_signature, source.aov_type);
        fields.shape_signature = hash_bytes(fields.shape_signature,
            source.attribute.name.data(), source.attribute.name.size());
      }
      fields.signature = hash_bytes(result.identity, fields.instructions.data(),
                                    fields.instructions.size() * sizeof(GeometryLineFieldInstruction));
      fields.signature = hash_bytes(fields.signature, fields.ramps.data(), fields.ramps.size() * sizeof(float4));
      for (const auto &source : fields.sources) {
        hash_value(fields.signature, source.type);
        hash_value(fields.signature, source.aov_type);
        fields.signature = hash_bytes(fields.signature, source.attribute.name.data(), source.attribute.name.size());
      }
      for (Image *image : fields.images) { hash_value(fields.signature, image); }
      hash_value(fields.signature, fields.resample);
      hash_value(fields.signature, fields.project_parameters);
      const int index = int(graph_.brushes.size());
      graph_.brushes.push_back(std::move(result));
      brushes_[key] = index;
      return index;
    }
    if (!ELEM(node.type_legacy, NPR_LINE_NODE_BRUSH, NPR_LINE_NODE_BRUSH_INPUT)) { return -1; }
    Brush result = brush_settings(context, node);
    const int index = int(graph_.brushes.size());
    graph_.brushes.push_back(std::move(result));
    brushes_[key] = index;
    return index;
  }

  Brush brush_settings(const Context &context, const bNode &node)
  {
    const auto &data = *static_cast<const NodeNPRLine *>(node.storage);
    Brush result;
    result.mode = data.mode;
    result.stroke_mode = (data.curve_flags >> 3) & 3;
    result.wave_pixels = (data.curve_flags & (1 << 6)) != 0;
    result.raster_size_response = (data.curve_flags & 4) != 0;
    result.width_curve_enabled = (data.curve_flags & 1) && data.width_curve;
    result.opacity_curve_enabled = (data.curve_flags & 2) && data.opacity_curve;
    if (result.width_curve_enabled) { BKE_curvemapping_init(data.width_curve); }
    if (result.opacity_curve_enabled) { BKE_curvemapping_init(data.opacity_curve); }
    for (int i = 0; i < int(result.curve.size()); i++) {
      const float t = float(i) / float(result.curve.size() - 1);
      result.curve[i] = float2(
          result.width_curve_enabled ? std::clamp(BKE_curvemapping_evaluateF(data.width_curve, 0, t), 0.0f, 1.0f) : 1.0f,
          result.opacity_curve_enabled ? std::clamp(BKE_curvemapping_evaluateF(data.opacity_curve, 0, t), 0.0f, 1.0f) : 1.0f);
    }
    result.color = color(context, node, "color");
    result.identity = context.identity;
    hash_value(result.identity, node.identifier);
#define READ_BRUSH_FLOAT(name) result.name = number(context, node, #name, result.name)
    READ_BRUSH_FLOAT(width);
    READ_BRUSH_FLOAT(opacity);
    READ_BRUSH_FLOAT(spacing);
    READ_BRUSH_FLOAT(stretch);
    READ_BRUSH_FLOAT(angle);
    READ_BRUSH_FLOAT(softness);
    READ_BRUSH_FLOAT(dot_size);
    READ_BRUSH_FLOAT(dot_size_random);
    READ_BRUSH_FLOAT(dot_antialiasing);
    READ_BRUSH_FLOAT(dot_space_x);
    READ_BRUSH_FLOAT(dot_space_y);
    READ_BRUSH_FLOAT(dot_space_random_x);
    READ_BRUSH_FLOAT(dot_space_random_y);
    READ_BRUSH_FLOAT(tip_reduction_start);
    READ_BRUSH_FLOAT(tip_reduction_end);
    READ_BRUSH_FLOAT(groove);
    READ_BRUSH_FLOAT(taper_start);
    READ_BRUSH_FLOAT(taper_end);
    READ_BRUSH_FLOAT(alpha_start);
    READ_BRUSH_FLOAT(alpha_end);
    READ_BRUSH_FLOAT(dash_length);
    READ_BRUSH_FLOAT(dash_space);
    READ_BRUSH_FLOAT(dash_phase);
    READ_BRUSH_FLOAT(width_random);
    READ_BRUSH_FLOAT(angle_random);
    READ_BRUSH_FLOAT(grain);
    READ_BRUSH_FLOAT(extend);
    READ_BRUSH_FLOAT(offset);
    READ_BRUSH_FLOAT(shift_x);
    READ_BRUSH_FLOAT(shift_y);
    READ_BRUSH_FLOAT(wave);
    READ_BRUSH_FLOAT(cycles);
    READ_BRUSH_FLOAT(noise);
    READ_BRUSH_FLOAT(wave_length);
    READ_BRUSH_FLOAT(wave_phase);
    READ_BRUSH_FLOAT(wave_random);
    READ_BRUSH_FLOAT(wave_period_random);
    READ_BRUSH_FLOAT(wave_phase_random);
    READ_BRUSH_FLOAT(offset_random);
#undef READ_BRUSH_FLOAT
    /* Linked group values are not limited by a destination socket's UI range. In
     * particular a negative dot space could otherwise produce a zero lattice step. */
    result.dot_size = std::clamp(result.dot_size, 0.1f, 100.0f);
    result.dot_size_random = std::clamp(result.dot_size_random, 0.0f, 1.0f);
    result.dot_antialiasing = std::clamp(result.dot_antialiasing, 0.0f, 10.0f);
    result.dot_space_x = std::clamp(result.dot_space_x, 0.0f, 1.0f);
    result.dot_space_y = std::clamp(result.dot_space_y, 0.0f, 1.0f);
    result.dot_space_random_x = std::clamp(result.dot_space_random_x, 0.0f, 1.0f);
    result.dot_space_random_y = std::clamp(result.dot_space_random_y, 0.0f, 1.0f);
    result.tip_reduction_start = std::clamp(result.tip_reduction_start, 0.0f, 1.0f);
    result.tip_reduction_end = std::clamp(result.tip_reduction_end, 0.0f, 1.0f);
    result.groove = std::clamp(result.groove, 0.0f, 1.0f);
    result.softness = std::clamp(result.softness, 0.0f, 1.0f);
    result.wave_length = std::clamp(result.wave_length, 5.0f, 10000.0f);
    result.wave_phase = std::clamp(result.wave_phase, -30.0f * float(M_PI), 30.0f * float(M_PI));
    result.offset_random = std::clamp(result.offset_random, 0.0f, 1000.0f);
    result.wave_random = std::clamp(result.wave_random, 0.0f, 1.0f);
    result.wave_period_random = std::clamp(result.wave_period_random, 0.0f, 1.0f);
    result.wave_phase_random = std::clamp(result.wave_phase_random, 0.0f, 1.0f);
    result.override_connection = (data.curve_flags & (1 << 5)) != 0;
    result.width_map_strength = std::clamp(number(context, node, "width_map_strength", 1), 0.0f, 1.0f);
    if (result.width_map_strength > 0) { result.width_map = texture_map(value(context, node, "width_map")); }
    result.color_map_strength = std::clamp(number(context, node, "color_map_strength", 1), 0.0f, 1.0f);
    if (result.color_map_strength > 0) { result.color_map = texture_map(value(context, node, "color_map")); }
    result.brush_map_opacity = std::clamp(number(context, node, "brush_map_opacity", 1), 0.0f, 1.0f);
    if (result.mode != 0 && result.brush_map_opacity > 0) {
      result.brush_map = texture_map(value(context, node, "brush_map"));
    }
    result.distortion_map_amount = std::clamp(number(context, node, "distortion_map_amount", 5), 0.0f, 1000.0f);
    if (result.distortion_map_amount > 0) { result.distortion_map = texture_map(value(context, node, "distortion_map")); }
    if (number(context, node, "distortion_enabled", 1) == 0) {
      result.wave = result.noise = 0;
      result.distortion_map = -1;
    }
    if (result.override_connection) {
      auto bounded = [&](const char *name, float fallback, float min, float max) {
        return std::clamp(number(context, node, name, fallback), min, max);
      };
      result.connection.intersection_join = bounded("stroke_intersection_join", 1, 0, 100);
      result.connection.gap_join = bounded("stroke_gap_join", 2, 0, 100);
      result.connection.split_angle = bounded("stroke_split_angle", 90, 0, 180);
      result.connection.minimum_length = bounded("stroke_minimum_length", 0, 0, 10000);
      result.connection.direction = bounded("stroke_direction", -30, -180, 180);
      result.connection.clockwise = number(context, node, "stroke_clockwise", 1) != 0;
    }
    result.seed = int(number(context, node, "seed"));
    result.groove_count = std::clamp(int(number(context, node, "groove_count", 5)), 3, 20);
    result.copies = std::clamp(int(number(context, node, "copies", 1)), 1, 32);
    result.dashed = number(context, node, "dashed") != 0;
    result.image = reinterpret_cast<Image *>(id(context, node, "image"));
    return result;
  }

  Plan plan(SocketRef ref, int depth = 0)
  {
    Plan result;
    if (!ref || ref.socket->in_out != SOCK_OUT) {
      return result;
    }
    if (depth > 128) {
      error("Geometry line plans contain a cycle or exceed 128 levels");
      return result;
    }
    const bNode &node = ref.node();
    const Context &context = *ref.context;
    const auto *data = static_cast<const NodeNPRLine *>(node.storage);
    if (!data || !data->enabled || (node.flag & NODE_MUTED)) {
      return result;
    }
    if (node.type_legacy == NPR_LINE_NODE_COMBINE) {
      result.overlay = data->mode == 1;
      const bNodeSocket *plans = input(node, "plans");
      for (const bNodeLink *link : plans->directly_linked_links()) {
        if (!(link->flag & NODE_LINK_MUTED)) {
          result.children.push_back(plan(resolve(context, link->fromsock), depth + 1));
        }
      }
      return result;
    }
    if (node.type_legacy != NPR_LINE_NODE_SETTINGS) {
      error("A Line Plan input must come from Line Settings or Combine Lines");
      return result;
    }
    Rule rule;
    RuleScope rule_scope = scope(context, node);
    rule.repair_seams = rule_scope.objects->repair_seams;
    rule.visible = rule.hidden = 0;
    const int visible_brush = brush(value(context, node, "visible_brush"));
    int hidden_brush = brush(value(context, node, "hidden_brush"));
    if (hidden_brush < 0) {
      hidden_brush = visible_brush;
    }
    const char *edges[] = {"outline", "object", "intersection", "smoothing", "material", "normal", "wire", "marked"};
    for (int i = 0; i < 8; i++) {
      const std::string edge = edges[i];
      if (number(context, node, ("visible_" + edge).c_str()) != 0) {
        rule.visible |= 1 << i;
      }
      if (number(context, node, ("hidden_" + edge).c_str()) != 0) {
        rule.hidden |= 1 << i;
      }
      rule.visible_brush[i] = visible_brush;
      rule.hidden_brush[i] = hidden_brush;
      if (data->visible_overrides & (1 << i)) {
        const int custom = brush(value(context, node, ("visible_override_" + edge).c_str()));
        if (custom >= 0) {
          rule.visible_brush[i] = custom;
        }
      }
      if (data->hidden_overrides & (1 << i)) {
        const int custom = brush(value(context, node, ("hidden_override_" + edge).c_str()));
        if (custom >= 0) {
          rule.hidden_brush[i] = custom;
        }
      }
      rule.width_scale[i] = number(context, node, ("scale_" + edge).c_str(), 1);
    }
#define READ_RULE_BOOL(name) rule.name = number(context, node, #name, rule.name ? 1 : 0) != 0
#define READ_RULE_FLOAT(name) rule.name = number(context, node, #name, rule.name)
    READ_RULE_BOOL(hidden_enabled);
    READ_RULE_BOOL(open_edges);
    READ_RULE_BOOL(visible_open_outline);
    READ_RULE_BOOL(visible_open_object);
    READ_RULE_BOOL(hidden_open_outline);
    READ_RULE_BOOL(hidden_open_object);
    READ_RULE_BOOL(object_contours);
    READ_RULE_BOOL(contour_priority);
    READ_RULE_BOOL(self_intersection);
    READ_RULE_BOOL(connect_objects);
    READ_RULE_BOOL(connect_coincident);
    READ_RULE_BOOL(screen_space_join);
    READ_RULE_BOOL(clockwise);
    READ_RULE_FLOAT(normal_min);
    READ_RULE_FLOAT(normal_max);
    READ_RULE_FLOAT(intersection_join);
    READ_RULE_FLOAT(gap_join);
    READ_RULE_FLOAT(join_depth);
    READ_RULE_FLOAT(split_angle);
    READ_RULE_FLOAT(minimum_length);
    READ_RULE_FLOAT(direction);
#undef READ_RULE_BOOL
#undef READ_RULE_FLOAT
    rule.width_reduction = reduction(value(context, node, "width_reduction"));
    rule.opacity_reduction = reduction(value(context, node, "opacity_reduction"));
    rule.hidden_width_reduction = reduction(value(context, node, "hidden_width_reduction"));
    rule.hidden_opacity_reduction = reduction(value(context, node, "hidden_opacity_reduction"));
    auto override_connection = [&](int index) {
      if (index < 0 || !graph_.brushes[index].override_connection) { return; }
      const auto found = std::find_if(rule.brush_connections.begin(), rule.brush_connections.end(),
                                      [&](const auto &item) { return item.first == index; });
      if (found == rule.brush_connections.end()) {
        rule.brush_connections.emplace_back(index, graph_.brushes[index].connection);
      }
    };
    for (int i = 0; i < 8; i++) {
      if (rule.visible & (1 << i)) { override_connection(rule.visible_brush[i]); }
      if (rule.hidden_enabled && (rule.hidden & (1 << i))) { override_connection(rule.hidden_brush[i]); }
    }
    result.rule = int(graph_.rules.size());
    graph_.rules.push_back(rule);
    graph_.scopes.push_back(std::move(rule_scope));
    return result;
  }

  void signatures()
  {
    uint64_t visibility = 14695981039346656037ULL;
    auto hash_scope = [&](const RuleScope &scope) {
      const ObjectScope &objects = *scope.objects;
      hash_value(visibility, objects.mode);
      hash_value(visibility, objects.collection);
      hash_value(visibility, objects.exclude_collection);
      hash_value(visibility, objects.children);
      hash_value(visibility, objects.instances);
      hash_value(visibility, objects.repair_seams);
      hash_value(visibility, scope.exclude_materials);
      for (auto *object : objects.objects) { hash_value(visibility, object); }
      hash_value(visibility, objects.objects.size());
      for (auto *object : objects.excluded) { hash_value(visibility, object); }
      hash_value(visibility, objects.excluded.size());
      for (auto *material : scope.materials) { hash_value(visibility, material); }
      hash_value(visibility, scope.materials.size());
    };
    for (int i = 0; i < int(graph_.rules.size()); i++) {
      const Rule &rule = graph_.rules[i];
      hash_scope(graph_.scopes[i]);
      hash_value(visibility, rule.visible);
      hash_value(visibility, rule.hidden);
      hash_value(visibility, rule.hidden_enabled);
      hash_value(visibility, rule.open_edges);
      hash_value(visibility, rule.visible_open_outline);
      hash_value(visibility, rule.visible_open_object);
      hash_value(visibility, rule.hidden_open_outline);
      hash_value(visibility, rule.hidden_open_object);
      hash_value(visibility, rule.object_contours);
      hash_value(visibility, rule.contour_priority);
      hash_value(visibility, rule.self_intersection);
      hash_value(visibility, rule.normal_min);
      hash_value(visibility, rule.normal_max);
    }
    auto hash_plan = [&](const auto &self, const Plan &plan) -> void {
      hash_value(visibility, plan.rule);
      hash_value(visibility, plan.overlay);
      hash_value(visibility, plan.children.size());
      for (const Plan &child : plan.children) { self(self, child); }
    };
    hash_plan(hash_plan, graph_.plan);
    for (const auto &occlusion : graph_.occlusion) {
      hash_scope(occlusion.scope);
      hash_value(visibility, occlusion.mode);
      hash_value(visibility, occlusion.image);
      hash_value(visibility, occlusion.threshold);
      visibility = hash_bytes(visibility, occlusion.uv_map.data(), occlusion.uv_map.size());
    }
    graph_.visibility_signature = visibility;
    uint64_t connection = visibility;
    for (const Rule &rule : graph_.rules) {
      hash_value(connection, rule.connect_objects);
      hash_value(connection, rule.connect_coincident);
      hash_value(connection, rule.screen_space_join);
      hash_value(connection, rule.clockwise);
      hash_value(connection, rule.direction);
      hash_value(connection, rule.intersection_join);
      hash_value(connection, rule.gap_join);
      hash_value(connection, rule.join_depth);
      hash_value(connection, rule.split_angle);
      hash_value(connection, rule.minimum_length);
      hash_value(connection, rule.visible_brush);
      hash_value(connection, rule.hidden_brush);
      for (const auto &[brush, settings] : rule.brush_connections) {
        hash_value(connection, brush);
        hash_value(connection, settings.clockwise);
        hash_value(connection, settings.direction);
        hash_value(connection, settings.intersection_join);
        hash_value(connection, settings.gap_join);
        hash_value(connection, settings.split_angle);
        hash_value(connection, settings.minimum_length);
      }
    }
    graph_.connection_signature = connection;
    uint64_t paint = connection;
    for (const Brush &brush : graph_.brushes) {
      if (brush.fields) { hash_value(paint, brush.fields->signature); }
      hash_value(paint, brush.color);
      const float values[] = {brush.width, brush.opacity, brush.spacing, brush.stretch, brush.angle,
                              brush.softness, brush.taper_start, brush.taper_end, brush.alpha_start,
                              brush.alpha_end, brush.dash_length, brush.dash_space, brush.dash_phase,
                              brush.width_random, brush.angle_random, brush.grain, brush.extend,
                              brush.offset, brush.shift_x, brush.shift_y, brush.wave, brush.cycles, brush.noise,
                              brush.dot_size, brush.dot_size_random, brush.dot_antialiasing,
                              brush.dot_space_x, brush.dot_space_y, brush.dot_space_random_x,
                              brush.dot_space_random_y, brush.tip_reduction_start, brush.tip_reduction_end,
                              brush.groove};
      paint = hash_bytes(paint, values, sizeof(values));
      hash_value(paint, brush.mode);
      hash_value(paint, brush.wave_pixels);
      hash_value(paint, brush.wave_length);
      hash_value(paint, brush.wave_phase);
      hash_value(paint, brush.wave_random);
      hash_value(paint, brush.wave_period_random);
      hash_value(paint, brush.wave_phase_random);
      hash_value(paint, brush.offset_random);
      hash_value(paint, brush.stroke_mode);
      hash_value(paint, brush.groove_count);
      hash_value(paint, brush.width_map);
      hash_value(paint, brush.width_map_strength);
      hash_value(paint, brush.color_map);
      hash_value(paint, brush.color_map_strength);
      hash_value(paint, brush.brush_map);
      hash_value(paint, brush.brush_map_opacity);
      hash_value(paint, brush.distortion_map);
      hash_value(paint, brush.distortion_map_amount);
      hash_value(paint, brush.copies);
      hash_value(paint, brush.seed);
      hash_value(paint, brush.dashed);
      hash_value(paint, brush.image);
      hash_value(paint, brush.width_curve_enabled);
      hash_value(paint, brush.opacity_curve_enabled);
      hash_value(paint, brush.raster_size_response);
      hash_value(paint, brush.curve);
    }
    for (const Rule &rule : graph_.rules) {
      hash_value(paint, rule.width_scale);
      hash_value(paint, rule.width_reduction);
      hash_value(paint, rule.opacity_reduction);
      hash_value(paint, rule.hidden_width_reduction);
      hash_value(paint, rule.hidden_opacity_reduction);
    }
    for (const Reduction &item : graph_.reductions) {
      hash_value(paint, item.start);
      hash_value(paint, item.end);
      hash_value(paint, item.use_object);
      hash_value(paint, item.object);
      hash_value(paint, item.curve);
    }
    for (const TextureMap &map : graph_.maps) {
      hash_value(paint, map.image);
      hash_value(paint, map.coordinates);
      hash_value(paint, map.uv.index);
      paint = hash_bytes(paint, map.uv.name.data(), map.uv.name.size());
      hash_value(paint, map.tiling);
      hash_value(paint, map.offset);
      hash_value(paint, map.rotation);
      hash_value(paint, map.wrap_u);
      hash_value(paint, map.wrap_v);
      hash_value(paint, map.filter);
    }
    hash_value(paint, graph_.width_scale);
    hash_value(paint, graph_.relative_width);
    hash_value(paint, graph_.reference_width);
    hash_value(paint, graph_.antialiasing);
    hash_value(paint, graph_.samples);
    hash_value(paint, graph_.depth_of_field);
    hash_value(paint, graph_.motion_blur);
    graph_.paint_signature = paint;
  }

 public:
  Graph compile(const bNodeTree &tree, int solo_node)
  {
    solo_node_ = solo_node;
    root_.tree = &tree;
    tree.ensure_topology_cache();
    const bNode *output = nullptr;
    for (const bNode &node : tree.nodes) {
      if (node.type_legacy == NPR_LINE_NODE_OUTPUT && (node.flag & NODE_DO_OUTPUT)) {
        output = &node;
        break;
      }
    }
    if (!output) {
      error("Geometry Lines has no active Line Output");
      return std::move(graph_);
    }
    graph_.field_sample_length = std::clamp(number(root_, *output, "field_sample_length", 0.9f),
                                          0.1f, 1000.0f);
    SocketRef plan_ref = value(root_, *output, "plan");
    if (solo_node_) {
      for (const bNode &node : tree.nodes) {
        if (node.identifier == solo_node_ && ELEM(node.type_legacy, NPR_LINE_NODE_SETTINGS, NPR_LINE_NODE_COMBINE)) {
          /* Compile only the preview branch; offline rendering never supplies a solo ID. */
          plan_ref = SocketRef{&root_, static_cast<const bNodeSocket *>(node.outputs.first_)};
          break;
        }
      }
    }
    graph_.plan = plan(plan_ref);
    /* Intermediate Brush states are compiler inputs, not independently executed
     * brushes. In particular, unreferenced sources must not allocate AOV layers. */
    std::vector<int> remap(graph_.brushes.size(), -1);
    for (auto &rule : graph_.rules) {
      for (int i = 0; i < 8; i++) {
        for (auto pair : {std::pair<int *, bool>{&rule.visible_brush[i], bool(rule.visible & (1 << i))},
                          {&rule.hidden_brush[i], rule.hidden_enabled && bool(rule.hidden & (1 << i))}}) {
          if (!pair.second) { *pair.first = -1; }
          if (*pair.first >= 0) { remap[*pair.first] = 0; }
        }
      }
    }
    std::vector<Brush> live_brushes;
    for (int i = 0; i < int(remap.size()); i++) {
      if (remap[i] >= 0) {
        remap[i] = int(live_brushes.size());
        live_brushes.push_back(std::move(graph_.brushes[i]));
      }
    }
    for (auto &rule : graph_.rules) {
      for (int i = 0; i < 8; i++) {
        if (rule.visible_brush[i] >= 0) { rule.visible_brush[i] = remap[rule.visible_brush[i]]; }
        if (rule.hidden_brush[i] >= 0) { rule.hidden_brush[i] = remap[rule.hidden_brush[i]]; }
      }
      for (auto &connection : rule.brush_connections) { connection.first = remap[connection.first]; }
    }
    graph_.brushes = std::move(live_brushes);
    const auto &data = *static_cast<const NodeNPRLine *>(output->storage);
    graph_.preview = data.mode;
    graph_.composite = number(root_, *output, "composite", 1) != 0;
    graph_.relative_width = number(root_, *output, "relative_width") != 0;
    graph_.width_scale = number(root_, *output, "width_scale", 1);
    graph_.reference_width = number(root_, *output, "reference_width", 1920);
    graph_.depth_of_field = number(root_, *output, "depth_of_field") != 0;
    graph_.motion_blur = number(root_, *output, "motion_blur") != 0;
    graph_.antialiasing = number(root_, *output, "antialiasing", 1);
    graph_.offscreen_margin = number(root_, *output, "offscreen_margin");
    graph_.samples = int(number(root_, *output, "samples", 1));
    for (const bNodeLink *link : input(*output, "occlusion")->directly_linked_links()) {
      if (link->flag & NODE_LINK_MUTED) { continue; }
      SocketRef ref = resolve(root_, link->fromsock);
      if (!ref || ref.node().type_legacy != NPR_LINE_NODE_OCCLUSION) { continue; }
      const auto &settings = *static_cast<const NodeNPRLine *>(ref.node().storage);
      if (!settings.enabled) { continue; }
      OcclusionRule rule;
      rule.scope = scope(*ref.context, ref.node());
      rule.mode = settings.mode;
      rule.image = reinterpret_cast<Image *>(id(*ref.context, ref.node(), "image"));
      rule.uv_map = string(*ref.context, ref.node(), "uv_map");
      rule.threshold = number(*ref.context, ref.node(), "threshold", 0.5f);
      graph_.occlusion.push_back(std::move(rule));
    }
    signatures();
    return std::move(graph_);
  }
};

}  // namespace

Graph compile_graph(const bNodeTree &tree, int solo_node)
{
  return Compiler().compile(tree, solo_node);
}

}  // namespace blender::eevee::geometry_lines
