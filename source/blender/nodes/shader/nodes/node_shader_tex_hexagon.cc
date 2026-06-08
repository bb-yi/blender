/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include <array>

#include "BLI_math_base_safe.h"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"

#include "BKE_texture.h"

#include "NOD_multi_function.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender {

namespace nodes::node_shader_tex_hexagon_cc {

NODE_STORAGE_FUNCS(NodeTexHexagon)

enum {
  NODE_HEXAGON_DIRECTION_HORIZONTAL = 0,
  NODE_HEXAGON_DIRECTION_VERTICAL = 1,
  NODE_HEXAGON_DIRECTION_HORIZONTAL_TILED = 2,
  NODE_HEXAGON_DIRECTION_VERTICAL_TILED = 3,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr)
      .hide_value()
      .default_input_type(NODE_DEFAULT_INPUT_POSITION_FIELD)
      .description("Coordinates used to evaluate the hexagon pattern");
  b.add_input<decl::Float>("Scale"_ustr).default_value(5.0f).description("Overall scale of the grid");
  b.add_input<decl::Float>("Size"_ustr)
      .default_value(1.0f)
      .description("Relative size of the generated hexagon cells");
  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(0.0f)
      .description("Blends between edge distance and rounded distance");
  b.add_input<decl::Float>("Roundness"_ustr)
      .default_value(0.0f)
      .subtype(PROP_FACTOR)
      .description("Roundness applied in SDF mode");
  b.add_output<decl::Float>("Value"_ustr).no_muted_links();
  b.add_output<decl::Color>("Color"_ustr).no_muted_links();
  b.add_output<decl::Vector>("Hex Coords"_ustr).no_muted_links();
  b.add_output<decl::Vector>("Position"_ustr).no_muted_links();
  b.add_output<decl::Vector>("Cell UV"_ustr).no_muted_links();
  b.add_output<decl::Vector>("Cell ID"_ustr).no_muted_links();
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "value_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "direction", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "coord_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_clamp", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  NodeTexHexagon *tex = MEM_new<NodeTexHexagon>(__func__);
  BKE_texture_mapping_default(&tex->base.tex_mapping, TEXMAP_TYPE_POINT);
  BKE_texture_colormapping_default(&tex->base.color_mapping);
  node->storage = tex;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  NodeTexHexagon &tex = node_storage(*node);
  bNodeSocket *radius_socket = bke::node_find_socket(*node, SOCK_IN, "Radius"_ustr);
  bNodeSocket *roundness_socket = bke::node_find_socket(*node, SOCK_IN, "Roundness"_ustr);

  bke::node_set_socket_availability(
      *ntree, *radius_socket, !ELEM(tex.value_mode, SHD_HEXAGON_VALUE_DOT));
  bke::node_set_socket_availability(
      *ntree, *roundness_socket, tex.value_mode == SHD_HEXAGON_VALUE_SDF);
}

static int node_shader_gpu_tex_hexagon(GPUMaterial *mat,
                                       bNode *node,
                                       bNodeExecData * /*execdata*/,
                                       GPUNodeStack *in,
                                       GPUNodeStack *out)
{
  node_shader_gpu_default_tex_coord(mat, node, &in[0].link);
  node_shader_gpu_tex_mapping(mat, node, in, out);

  const NodeTexHexagon &tex = node_storage(*node);
  const float coord_mode = float(tex.coord_mode);
  const float value_mode = float(tex.value_mode);
  const float direction = float(tex.direction);
  const int result = GPU_stack_link(
      mat, node, "node_tex_hexagon", in, out, GPU_constant(&coord_mode), GPU_constant(&value_mode), GPU_constant(&direction));

  if (result && tex.use_clamp) {
    const float zero = 0.0f;
    const float one = 1.0f;
    GPU_link(mat, "clamp_value", out[0].link, GPU_constant(&zero), GPU_constant(&one), &out[0].link);
  }
  return result;
}

class HexagonFunction : public mf::MultiFunction {
 private:
  int coord_mode_;
  int direction_;
  int use_clamp_;
  int value_mode_;

 public:
  HexagonFunction(int coord_mode, int direction, int use_clamp, int value_mode)
      : coord_mode_(coord_mode),
        direction_(direction),
        use_clamp_(use_clamp),
        value_mode_(value_mode)
  {
    static std::array<mf::Signature, 3> signatures = {create_signature(SHD_HEXAGON_VALUE_HEX),
                                                      create_signature(SHD_HEXAGON_VALUE_SDF),
                                                      create_signature(SHD_HEXAGON_VALUE_DOT)};
    this->set_signature(&signatures[value_mode]);
  }

  static mf::Signature create_signature(int value_mode)
  {
    mf::Signature signature;
    mf::SignatureBuilder builder{"Hexagon", signature};
    builder.single_input<float3>("Vector");
    builder.single_input<float>("Scale");
    builder.single_input<float>("Size");

    switch (value_mode) {
      case SHD_HEXAGON_VALUE_SDF:
        builder.single_input<float>("Radius");
        builder.single_input<float>("Roundness");
        break;
      case SHD_HEXAGON_VALUE_HEX:
        builder.single_input<float>("Radius");
        break;
      case SHD_HEXAGON_VALUE_DOT:
        break;
    }

    builder.single_output<float>("Value", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<ColorGeometry4f>("Color", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<float3>("Hex Coords");
    builder.single_output<float3>("Position");
    builder.single_output<float3>("Cell UV");
    builder.single_output<float3>("Cell ID");
    return signature;
  }

  static float sdf_dimension(float w, float *r)
  {
    float roundness = *r;
    const float sw = math::sign(w);
    w = math::abs(w);
    roundness = math::interpolate(0.0f, w, math::clamp(roundness, 0.0f, 1.0f));
    const float dimension = math::max(w - roundness, 0.0f);
    *r = roundness * 0.5f;
    return dimension * sw;
  }

  static float hex_value_sdf(const float3 pos, float r, float rd)
  {
    float2 p = float2(pos.x, pos.y);
    r = sdf_dimension(r, &rd);
    const float3 k = float3(-0.8660254f, 0.5f, 0.57735026f);
    const float2 kxy = float2(k.x, k.y);
    p = math::abs(p);
    p = p - (2.0f * math::min(math::dot(kxy, p), 0.0f) * kxy);
    p = p - float2(math::clamp(p.x, -k.z * r, k.z * r), r);
    return math::length(p) * math::sign(p.y) - rd * 2.0f;
  }

  static float hex_value(const float3 hp, const float radius)
  {
    const float3 fac = float3(
        math::abs(hp.x - hp.y), math::abs(hp.y - hp.z), math::abs(hp.z - hp.x));
    const float f = math::max(fac.x, math::max(fac.y, fac.z));
    return (radius == 0.0f) ? f : math::interpolate(f, math::length(fac) / 1.4142136f, radius);
  }

  static float3 xy_to_hex(const float3 xy, const float ratio)
  {
    float3 p = xy;
    p.x *= ratio;
    p.z = -0.5f * p.x - p.y;
    p.y = -0.5f * p.x + p.y;
    return p;
  }

  static float hexagon(float3 p,
                       float scale,
                       float size,
                       float radius,
                       float roundness,
                       int coord_mode,
                       int value_mode,
                       int direction,
                       float3 *hex_coords,
                       float3 *grid_position,
                       float3 *cell_coords,
                       float3 *cell_id,
                       bool calc_value)
  {
    const float ratio = (direction == NODE_HEXAGON_DIRECTION_HORIZONTAL_TILED ||
                         direction == NODE_HEXAGON_DIRECTION_VERTICAL_TILED) ?
                            1.0f :
                            1.1547005f;
    if (direction == NODE_HEXAGON_DIRECTION_VERTICAL ||
        direction == NODE_HEXAGON_DIRECTION_VERTICAL_TILED)
    {
      p = float3(p.y, p.x, p.z);
    }
    p = xy_to_hex(p * scale, ratio);
    *hex_coords = p;

    float3 ip = math::floor(p + 0.5f);
    const float s = ip.x + ip.y + ip.z;
    if (s != 0.0f) {
      const float3 abs_d = math::abs(ip - p);
      if (abs_d.x >= abs_d.y && abs_d.x >= abs_d.z) {
        ip.x -= s;
      }
      else if (abs_d.y >= abs_d.x && abs_d.y >= abs_d.z) {
        ip.y -= s;
      }
      else {
        ip.z -= s;
      }
    }

    float3 hp = p - ip;
    hp *= (size != 0.0f) ? 1.0f / size : 0.0f;
    const float3 xy_coords = float3(hp.x * 1.7320508f, hp.y - hp.z, 0.0f);
    if (coord_mode == SHD_HEXAGON_COORDS_HEX) {
      *cell_coords = hp;
      *cell_id = ip;
    }
    else {
      *cell_coords = xy_coords;
      *cell_id = float3(
          ip.x / ratio, (ip.y - ip.z + (1.0f - safe_floored_modf(ip.x, 2.0f))) / 2.0f, 0.0f);
    }
    if (direction == NODE_HEXAGON_DIRECTION_VERTICAL ||
        direction == NODE_HEXAGON_DIRECTION_VERTICAL_TILED)
    {
      hp = float3(hp.y, hp.x, hp.z);
      *cell_coords = float3(cell_coords->y, cell_coords->x, cell_coords->z);
      *cell_id = float3(cell_id->y, cell_id->x, cell_id->z);
    }

    *grid_position = math::safe_divide(*cell_id, float3(scale));
    if (!calc_value) {
      return 0.0f;
    }
    if (value_mode == SHD_HEXAGON_VALUE_DOT) {
      return math::length(hp);
    }
    if (value_mode == SHD_HEXAGON_VALUE_SDF) {
      return hex_value_sdf(xy_coords, radius, roundness);
    }
    return hex_value(hp, radius);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    auto get_float_input = [&](int param_index, StringRef name) -> const VArray<float> {
      return params.readonly_single_input<float>(param_index, name);
    };
    auto get_float3_input = [&](int param_index, StringRef name) -> const VArray<float3> {
      return params.readonly_single_input<float3>(param_index, name);
    };

    int param = 0;
    const VArray<float3> &vector = get_float3_input(param++, "Vector");
    const VArray<float> &scale = get_float_input(param++, "Scale");
    const VArray<float> &size = get_float_input(param++, "Size");

    switch (value_mode_) {
      case SHD_HEXAGON_VALUE_SDF: {
        const VArray<float> &radius = get_float_input(param++, "Radius");
        const VArray<float> &roundness = get_float_input(param++, "Roundness");
        MutableSpan<float> r_value = params.uninitialized_single_output_if_required<float>(param++, "Value");
        MutableSpan<ColorGeometry4f> r_color =
            params.uninitialized_single_output_if_required<ColorGeometry4f>(param++, "Color");
        MutableSpan<float3> r_coords = params.uninitialized_single_output<float3>(param++, "Hex Coords");
        MutableSpan<float3> r_position = params.uninitialized_single_output<float3>(param++, "Position");
        MutableSpan<float3> r_cell_uv = params.uninitialized_single_output<float3>(param++, "Cell UV");
        MutableSpan<float3> r_cell_id = params.uninitialized_single_output<float3>(param++, "Cell ID");
        const bool calc_value = !r_value.is_empty();
        const bool calc_color = !r_color.is_empty();

        mask.foreach_index([&](const int64_t i) {
          float3 cell_id;
          const float value = hexagon(vector[i],
                                      scale[i],
                                      size[i],
                                      radius[i],
                                      roundness[i],
                                      coord_mode_,
                                      value_mode_,
                                      direction_,
                                      &r_coords[i],
                                      &r_position[i],
                                      &r_cell_uv[i],
                                      &cell_id,
                                      calc_value);
          r_cell_id[i] = cell_id;
          if (calc_value) {
            r_value[i] = use_clamp_ ? math::clamp(value, 0.0f, 1.0f) : value;
          }
          if (calc_color) {
            const float3 color = noise::hash_float_to_float3(cell_id);
            r_color[i] = ColorGeometry4f(color.x, color.y, color.z, 1.0f);
          }
        });
        break;
      }
      case SHD_HEXAGON_VALUE_HEX: {
        const VArray<float> &radius = get_float_input(param++, "Radius");
        MutableSpan<float> r_value = params.uninitialized_single_output_if_required<float>(param++, "Value");
        MutableSpan<ColorGeometry4f> r_color =
            params.uninitialized_single_output_if_required<ColorGeometry4f>(param++, "Color");
        MutableSpan<float3> r_coords = params.uninitialized_single_output<float3>(param++, "Hex Coords");
        MutableSpan<float3> r_position = params.uninitialized_single_output<float3>(param++, "Position");
        MutableSpan<float3> r_cell_uv = params.uninitialized_single_output<float3>(param++, "Cell UV");
        MutableSpan<float3> r_cell_id = params.uninitialized_single_output<float3>(param++, "Cell ID");
        const bool calc_value = !r_value.is_empty();
        const bool calc_color = !r_color.is_empty();

        mask.foreach_index([&](const int64_t i) {
          float3 cell_id;
          const float value = hexagon(vector[i],
                                      scale[i],
                                      size[i],
                                      radius[i],
                                      0.0f,
                                      coord_mode_,
                                      value_mode_,
                                      direction_,
                                      &r_coords[i],
                                      &r_position[i],
                                      &r_cell_uv[i],
                                      &cell_id,
                                      calc_value);
          r_cell_id[i] = cell_id;
          if (calc_value) {
            r_value[i] = use_clamp_ ? math::clamp(value, 0.0f, 1.0f) : value;
          }
          if (calc_color) {
            const float3 color = noise::hash_float_to_float3(cell_id);
            r_color[i] = ColorGeometry4f(color.x, color.y, color.z, 1.0f);
          }
        });
        break;
      }
      case SHD_HEXAGON_VALUE_DOT: {
        MutableSpan<float> r_value = params.uninitialized_single_output_if_required<float>(param++, "Value");
        MutableSpan<ColorGeometry4f> r_color =
            params.uninitialized_single_output_if_required<ColorGeometry4f>(param++, "Color");
        MutableSpan<float3> r_coords = params.uninitialized_single_output<float3>(param++, "Hex Coords");
        MutableSpan<float3> r_position = params.uninitialized_single_output<float3>(param++, "Position");
        MutableSpan<float3> r_cell_uv = params.uninitialized_single_output<float3>(param++, "Cell UV");
        MutableSpan<float3> r_cell_id = params.uninitialized_single_output<float3>(param++, "Cell ID");
        const bool calc_value = !r_value.is_empty();
        const bool calc_color = !r_color.is_empty();

        mask.foreach_index([&](const int64_t i) {
          float3 cell_id;
          const float value = hexagon(vector[i],
                                      scale[i],
                                      size[i],
                                      0.0f,
                                      0.0f,
                                      coord_mode_,
                                      value_mode_,
                                      direction_,
                                      &r_coords[i],
                                      &r_position[i],
                                      &r_cell_uv[i],
                                      &cell_id,
                                      calc_value);
          r_cell_id[i] = cell_id;
          if (calc_value) {
            r_value[i] = use_clamp_ ? math::clamp(value, 0.0f, 1.0f) : value;
          }
          if (calc_color) {
            const float3 color = noise::hash_float_to_float3(cell_id);
            r_color[i] = ColorGeometry4f(color.x, color.y, color.z, 1.0f);
          }
        });
        break;
      }
    }
  }
};

static void build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const NodeTexHexagon &tex = node_storage(builder.node());
  builder.construct_and_set_matching_fn<HexagonFunction>(
      tex.coord_mode, tex.direction, tex.use_clamp, tex.value_mode);
}

}  // namespace nodes::node_shader_tex_hexagon_cc

void register_node_type_sh_tex_hexagon()
{
  namespace file_ns = nodes::node_shader_tex_hexagon_cc;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeTexHexagon"_ustr, SH_NODE_TEX_HEXAGON);
  ntype.ui_name = "Hex Grid Texture";
  ntype.ui_description = "Generate a hexagonal grid texture with cell data outputs";
  ntype.enum_name_legacy = "TEX_HEXAGON";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_layout;
  ntype.initfunc = file_ns::node_init;
  ntype.updatefunc = file_ns::node_update;
  bke::node_type_storage(
      ntype, "NodeTexHexagon", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::node_shader_gpu_tex_hexagon;
  ntype.build_multi_function = file_ns::build_multi_function;

  bke::node_register_type(ntype);
}

}  // namespace blender
