/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender {

namespace nodes::node_shader_npr_output_cc {

class NPRColorOrImage : public decl::Color {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_RGBA;
  using Builder = decl::ColorBuilder;

  bool can_connect(const bNodeSocket &socket) const override
  {
    if (decl::Color::can_connect(socket)) {
      return true;
    }
    return this->in_out != socket.in_out && socket.type == SOCK_IMAGE;
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<NPRColorOrImage>("Color"_ustr).hide_value();
}

static int node_shader_gpu_npr_output(GPUMaterial *mat,
                                      bNode * /*node*/,
                                      bNodeExecData * /*execdata*/,
                                      GPUNodeStack *in,
                                      GPUNodeStack * /*out*/)
{
  GPUNodeLink *outlink_npr = nullptr;
  if (!in[0].link) {
    return true;
  }

  switch (in[0].sockettype) {
    case SOCK_FLOAT:
    case SOCK_INT:
    case SOCK_BOOLEAN:
      GPU_link(mat, "npr_output_float", in[0].link, &outlink_npr);
      break;
    case SOCK_VECTOR:
      GPU_link(mat, "npr_output_vec3", in[0].link, &outlink_npr);
      break;
    case SOCK_IMAGE:
      GPU_link(mat, "npr_output_texture_handle", in[0].link, &outlink_npr);
      break;
    default:
      GPU_link(mat, "npr_output", in[0].link, &outlink_npr);
      break;
  }

  if (outlink_npr != nullptr) {
    GPU_material_output_npr(mat, outlink_npr);
  }
  return true;
}

}  // namespace nodes::node_shader_npr_output_cc

void register_node_type_sh_npr_output()
{
  namespace file_ns = nodes::node_shader_npr_output_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeNPR_Output"_ustr, SH_NODE_NPR_OUTPUT);
  ntype.enum_name_legacy = "NPR_OUTPUT";
  ntype.ui_name = "NPR Output";
  ntype.ui_description = "Output color from the NPR shader tree";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = npr_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_npr_output;

  bke::node_register_type(ntype);
}

}  // namespace blender
