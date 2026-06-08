/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include <cmath>

#include "FN_multi_function_builder.hh"

#include "NOD_multi_function.hh"

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_twirl_cc {

static void sh_node_twirl_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr)
      .default_value(float3{0.0f, 0.0f, 0.0f})
      .description("Input vector to twist around the center");
  b.add_input<decl::Vector>("Center"_ustr)
      .default_value(float3{0.5f, 0.5f, 0.0f})
      .description("Center point of the twirl");
  b.add_input<decl::Float>("Amount"_ustr)
      .default_value(0.0f)
      .description("Twirl amount applied based on the distance to the center");
  b.add_output<decl::Vector>("Vector"_ustr).description("Twisted vector");
}

static int gpu_shader_twirl(GPUMaterial *mat,
                            bNode *node,
                            bNodeExecData * /*execdata*/,
                            GPUNodeStack *in,
                            GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "node_twirl", in, out);
}

static float3 twirl_vector(const float3 &vector, const float3 &center, const float amount)
{
  const float uv_x = vector.x - center.x;
  const float uv_y = vector.y - center.y;
  const float radius = std::sqrt(uv_x * uv_x + uv_y * uv_y);
  const float angle = std::atan2(uv_y, uv_x) + radius * amount;

  return float3(std::cos(angle) * radius + center.x,
                std::sin(angle) * radius + center.y,
                vector.z);
}

static void sh_node_twirl_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI3_SO<float3, float3, float, float3>(
      "Twirl", [](const float3 &vector, const float3 &center, const float amount) {
        return twirl_vector(vector, center, amount);
      });
  builder.set_matching_fn(fn);
}

}  // namespace nodes::node_shader_twirl_cc

void register_node_type_sh_twirl()
{
  namespace file_ns = nodes::node_shader_twirl_cc;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeTwirl"_ustr, SH_NODE_TWIRL);
  ntype.ui_name = "Twirl";
  ntype.ui_description = "Twirl the input vector around a center point";
  ntype.enum_name_legacy = "TWIRL";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = file_ns::sh_node_twirl_declare;
  ntype.gpu_fn = file_ns::gpu_shader_twirl;
  ntype.build_multi_function = file_ns::sh_node_twirl_build_multi_function;

  bke::node_register_type(ntype);
}

}  // namespace blender
