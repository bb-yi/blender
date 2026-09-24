/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_output_outline_shell_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Color"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .description("Unlit color drawn by the additional surface. Alpha is ignored");
  b.add_input<decl::Vector>("Displacement"_ustr)
      .hide_value()
      .description(
          "World-space offset direction applied to each vertex. "
          "Uses the vertex normal when not connected");
  b.add_input<decl::Float>("Strength"_ustr)
      .default_value(0.01f)
      .subtype(PROP_DISTANCE)
      .description("Scale applied to the displacement direction");
}

static int node_shader_gpu_output_outline_shell(GPUMaterial *mat,
                                                  bNode *node,
                                                  bNodeExecData * /*execdata*/,
                                                  GPUNodeStack *in,
                                                  GPUNodeStack * /*out*/)
{
  static const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  GPU_material_flag_set(mat, GPU_MATFLAG_EMISSION);

  GPUNodeLink *color_link = in[0].link ? in[0].link : GPU_uniform(in[0].vec);
  GPUNodeLink *surface_link = nullptr;
  GPU_link(mat,
           "node_emission",
           color_link,
           GPU_constant(one),
           GPU_constant(one),
           &surface_link);
  GPU_material_output_surface(mat, surface_link);

  GPUNodeLink *displacement_link = in[1].link;
  if (displacement_link == nullptr) {
    GPU_link(mat, "world_normals_get", &displacement_link);
  }
  GPUNodeLink *strength_link = in[2].link ? in[2].link : GPU_uniform(in[2].vec);
  GPUNodeLink *offset_link = nullptr;
  GPU_link(mat,
           "vector_math_scale",
           displacement_link,
           GPU_constant(zero),
           GPU_constant(zero),
           strength_link,
           &offset_link,
           nullptr);
  GPUNodeLink *displacement_output = nullptr;
  GPU_link(mat, "node_output_material_displacement", offset_link, &displacement_output);
  GPU_material_output_displacement(mat, displacement_output);

  return true;
}

}  // namespace nodes::node_shader_output_outline_shell_cc

void register_node_type_sh_output_outline_shell()
{
  namespace file_ns = nodes::node_shader_output_outline_shell_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeOutputOutlineShell"_ustr, SH_NODE_OUTPUT_OUTLINE_SHELL);
  ntype.ui_name = "Outline Shell Output";
  ntype.ui_description =
      "Draw the surface a second time in Eevee as an outline shell, with an unlit color and a "
      "world-space vertex offset";
  ntype.enum_name_legacy = "OUTPUT_OUTLINE_SHELL";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = object_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_output_outline_shell;

  bke::node_register_type(ntype);
}

}  // namespace blender
