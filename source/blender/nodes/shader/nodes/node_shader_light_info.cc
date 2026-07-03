/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"

#include "BKE_lib_id.hh"

#include "DNA_light_types.h"
#include "DNA_object_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_light_info_cc {

enum eNodeLightInfoOutputType {
  NODE_LIGHT_INFO_TYPE_NONE = -1,
  NODE_LIGHT_INFO_TYPE_POINT = 0,
  NODE_LIGHT_INFO_TYPE_SUN = 1,
  NODE_LIGHT_INFO_TYPE_SPOT = 2,
  NODE_LIGHT_INFO_TYPE_AREA = 3,
};

static int node_light_info_type(const bNode &node)
{
  const Object *light_object = reinterpret_cast<const Object *>(node.id);
  if (light_object == nullptr || light_object->type != OB_LAMP || light_object->data == nullptr) {
    return -1;
  }
  const Light *light = id_cast<Light *>(light_object->data);
  return light->type;
}

static int node_light_info_output_type_value(const int light_type)
{
  switch (light_type) {
    case LA_LOCAL:
      return NODE_LIGHT_INFO_TYPE_POINT;
    case LA_SUN:
      return NODE_LIGHT_INFO_TYPE_SUN;
    case LA_SPOT:
      return NODE_LIGHT_INFO_TYPE_SPOT;
    case LA_AREA:
      return NODE_LIGHT_INFO_TYPE_AREA;
    default:
      return NODE_LIGHT_INFO_TYPE_NONE;
  }
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Float>("Power"_ustr);
  b.add_output<decl::Int>("Type"_ustr)
      .description("Light type index: 0 Point, 1 Sun, 2 Spot, 3 Area. Outputs -1 when no light is assigned");
  b.add_output<decl::Vector>("Position"_ustr)
      .description("World-space location of the selected light object")
      .available(false);
  b.add_output<decl::Vector>("Direction"_ustr)
      .description("World-space emission direction")
      .available(false);
  b.add_output<decl::Float>("Radius"_ustr)
      .description("Primary size parameter of the selected light")
      .available(false);
  b.add_output<decl::Float>("Spot Size"_ustr)
      .description("Spot cone angle in radians")
      .available(false);
  b.add_output<decl::Float>("Sun Angle"_ustr)
      .description("Sun angular diameter in radians")
      .available(false);
  b.add_output<decl::Float>("Visible"_ustr)
      .description("Outputs 1 when the selected light object is not hidden, otherwise 0");
}

static void node_shader_buts_light_info(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "light_object", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *position_socket = bke::node_find_socket(*node, SOCK_OUT, "Position"_ustr);
  bNodeSocket *direction_socket = bke::node_find_socket(*node, SOCK_OUT, "Direction"_ustr);
  bNodeSocket *radius_socket = bke::node_find_socket(*node, SOCK_OUT, "Radius"_ustr);
  bNodeSocket *spot_size_socket = bke::node_find_socket(*node, SOCK_OUT, "Spot Size"_ustr);
  bNodeSocket *sun_angle_socket = bke::node_find_socket(*node, SOCK_OUT, "Sun Angle"_ustr);

  const int light_type = node_light_info_type(*node);
  bke::node_set_socket_availability(
      *ntree, *position_socket, ELEM(light_type, LA_LOCAL, LA_SPOT, LA_AREA));
  bke::node_set_socket_availability(
      *ntree, *direction_socket, ELEM(light_type, LA_SUN, LA_SPOT, LA_AREA));
  bke::node_set_socket_availability(
      *ntree, *radius_socket, ELEM(light_type, LA_LOCAL, LA_SPOT, LA_AREA));
  bke::node_set_socket_availability(*ntree, *spot_size_socket, light_type == LA_SPOT);
  bke::node_set_socket_availability(*ntree, *sun_angle_socket, light_type == LA_SUN);
}

static void node_shader_light_info_values(const Object &light_object,
                                          const Light &light,
                                          float3 &r_position,
                                          float3 &r_direction,
                                          float &r_radius,
                                          float &r_spot_size,
                                          float &r_sun_angle)
{
  const float4x4 object_to_world = light_object.object_to_world();
  r_position = object_to_world.location();
  r_direction = float3(0.0f);
  r_radius = 0.0f;
  r_spot_size = 0.0f;
  r_sun_angle = 0.0f;

  const float3 z_axis = object_to_world.z_axis();
  if (light.type != LA_LOCAL && math::length_squared(z_axis) > 1e-12f) {
    r_direction = -math::normalize(z_axis);
  }

  const float3 scale = {
      math::length(object_to_world.x_axis()),
      math::length(object_to_world.y_axis()),
      math::length(object_to_world.z_axis()),
  };
  const float max_scale = max_ff(max_ff(scale.x, scale.y), scale.z);

  switch (light.type) {
    case LA_LOCAL:
      r_radius = light.radius * max_scale;
      break;
    case LA_SUN:
      r_radius = light.sun_angle;
      r_sun_angle = light.sun_angle;
      break;
    case LA_SPOT:
      r_radius = light.radius * max_scale;
      r_spot_size = light.spotsize;
      break;
    case LA_AREA: {
      const bool is_irregular = ELEM(light.area_shape, LA_AREA_RECT, LA_AREA_ELLIPSE);
      const float size_x = light.area_size * scale.x;
      const float size_y = (is_irregular ? light.area_sizey : light.area_size) * scale.y;
      r_radius = 0.5f * max_ff(size_x, size_y);
      break;
    }
  }
}

static float node_shader_light_info_visible(const Object &light_object)
{
  return (light_object.visibility_flag & (OB_HIDE_VIEWPORT | OB_HIDE_RENDER)) == 0 ? 1.0f : 0.0f;
}

static int node_shader_gpu_light_info(GPUMaterial *mat,
                                      bNode *node,
                                      bNodeExecData * /*execdata*/,
                                      GPUNodeStack *in,
                                      GPUNodeStack *out)
{
  static const float zero = 0.0f;
  static const float one = 1.0f;
  static const float invalid_type = float(NODE_LIGHT_INFO_TYPE_NONE);
  static const float zero_color[3] = {0.0f, 0.0f, 0.0f};
  static const float zero_vector[3] = {0.0f, 0.0f, 0.0f};

  GPUNodeLink *color_link = GPU_constant(zero_color);
  GPUNodeLink *power_link = GPU_constant(&zero);
  GPUNodeLink *type_link = GPU_constant(&invalid_type);
  GPUNodeLink *position_link = GPU_constant(zero_vector);
  GPUNodeLink *direction_link = GPU_constant(zero_vector);
  GPUNodeLink *radius_link = GPU_constant(&zero);
  GPUNodeLink *spot_size_link = GPU_constant(&zero);
  GPUNodeLink *sun_angle_link = GPU_constant(&zero);
  GPUNodeLink *visible_link = GPU_constant(&zero);
  Object *light_object = reinterpret_cast<Object *>(node->id);

  if (light_object != nullptr && light_object->type == OB_LAMP && light_object->data != nullptr) {
    Light *light = id_cast<Light *>(light_object->data);
    const float output_type = float(node_light_info_output_type_value(light->type));
    const float visible = node_shader_light_info_visible(*light_object);
    float3 position;
    float3 direction;
    float radius;
    float spot_size;
    float sun_angle;
    node_shader_light_info_values(
        *light_object, *light, position, direction, radius, spot_size, sun_angle);

    color_link = GPU_uniform(&light->r);
    power_link = GPU_uniform(&light->energy);
    type_link = GPU_uniform(&output_type);
    position_link = GPU_uniform(position);
    direction_link = GPU_uniform(direction);
    radius_link = GPU_uniform(&radius);
    spot_size_link = GPU_uniform(&spot_size);
    sun_angle_link = GPU_uniform(&sun_angle);
    visible_link = visible > 0.0f ? GPU_constant(&one) : GPU_constant(&zero);
  }

  return GPU_stack_link(mat,
                        node,
                        "node_light_info",
                        in,
                        out,
                        color_link,
                        power_link,
                        type_link,
                        position_link,
                        direction_link,
                        radius_link,
                        spot_size_link,
                        sun_angle_link,
                        visible_link);
}

}  // namespace nodes::node_shader_light_info_cc

void register_node_type_sh_light_info()
{
  namespace file_ns = nodes::node_shader_light_info_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeLightInfo"_ustr, SH_NODE_LIGHT_INFO);
  ntype.ui_name = "Light Info";
  ntype.ui_description =
      "Read flat color, power, transform, visibility, and size values from a chosen light";
  ntype.enum_name_legacy = "LIGHTINFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_light_info;
  ntype.updatefunc = file_ns::node_update;
  ntype.add_ui_poll = object_or_npr_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_light_info;

  bke::node_register_type(ntype);
}

}  // namespace blender
