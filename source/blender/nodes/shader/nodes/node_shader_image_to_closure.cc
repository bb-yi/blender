/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "BKE_node_runtime.hh"
#include "CLG_log.h"
#include "DNA_node_types.h"
#include "NOD_geometry_nodes_closure_signature.hh"

namespace blender {

namespace nodes::node_shader_image_to_closure_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Closure>("Closure"_ustr).create_signature([](const bNode & /*node*/) {
    ClosureSignature signature;
    signature.inputs.add({.key = "UV",
                          .type = bke::node_socket_type_find_static(SOCK_VECTOR),
                          .structure_type = NodeSocketInterfaceStructureType::Single,
                          .dimensions = 3});
    signature.outputs.add({.key = "Color",
                           .type = bke::node_socket_type_find_static(SOCK_RGBA),
                           .structure_type = NodeSocketInterfaceStructureType::Single,
                           .ui = {.subtype = PROP_COLOR}});
    signature.outputs.add({.key = "Alpha",
                           .type = bke::node_socket_type_find_static(SOCK_FLOAT),
                           .structure_type = NodeSocketInterfaceStructureType::Single});
    return signature;
  });
}

static void node_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = MEM_new<NodeShaderImageToClosure>(__func__);
}

/* Compiler-only sampling node. Keeping the image provider separate preserves native sampler
 * operations for GLSL consumers even when the same closure is also evaluated at explicit UVs. */
static void sample_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("UV"_ustr);
  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Float>("Alpha"_ustr);
}

static int sample_gpu(GPUMaterial *mat,
                      bNode *node,
                      bNodeExecData * /*execdata*/,
                      GPUNodeStack *in,
                      GPUNodeStack *out)
{
  GPUNodeStack sample_in[3] = {};
  sample_in[0] = in[0];
  sample_in[2].end = true;
  std::string error;
  if (!node_shader_image_to_closure_bind(mat, *node, sample_in[1], error)) {
    static CLG_LogRef LOG = {"node.shader.image_to_closure"};
    CLOG_WARN(&LOG, "Image closure evaluation: %s", error.c_str());
    return false;
  }
  /* Match texture(image, uv) in GLSL Function, including its native sampler settings.
   * A regular Image Texture node also applies Blender-specific LOD/alpha processing. */
  const bool image_2d = sample_in[1].type == GPU_TEX2D;
  const bool cube = sample_in[1].type == GPU_TEXCUBE;
  const char *name = image_2d ? "image_closure_sample_2d" :
                     cube     ? "image_closure_sample_cube" :
                                "image_closure_sample_3d";
  const char *filename = image_2d ? "image_closure_sample_2d.glsl" :
                         cube     ? "image_closure_sample_cube.glsl" :
                                    "image_closure_sample_3d.glsl";
  const std::string source = std::string("void ") + name + "(vec3 uv, " +
                             (image_2d ? "sampler2D" :
                              cube     ? "samplerCube" :
                                         "sampler3D") +
                             " image, out vec4 color, out float alpha) {"
                             "color = texture(image, " +
                             (image_2d ? "uv.xy" : "uv") + "); alpha = color.a;}\n";
  GPU_material_generated_source_add(mat, filename, {}, source.c_str());
  return GPU_stack_link_custom(
      mat, node, name, filename, GPU_CUSTOM_NODE_DEPENDENCY_NONE, sample_in, out);
}

}  // namespace nodes::node_shader_image_to_closure_cc

void register_node_type_sh_image_to_closure()
{
  namespace file_ns = nodes::node_shader_image_to_closure_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeImageToClosure"_ustr, SH_NODE_IMAGE_TO_CLOSURE);
  ntype.enum_name_legacy = "IMAGE_TO_CLOSURE";
  ntype.ui_name = "Image to Closure";
  ntype.ui_description = "Adapt an image into a closure-backed sample source";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_init;
  bke::node_type_storage(
      ntype, "NodeShaderImageToClosure", node_free_standard_storage, node_copy_standard_storage);
  ntype.add_ui_poll = object_filter_or_npr_eevee_shader_nodes_poll;

  bke::node_register_type(ntype);

  static bke::bNodeType sample_type;
  sh_node_type_base(&sample_type, "ShaderNodeImageClosureSample"_ustr);
  sample_type.ui_name = "Image Closure Sample";
  sample_type.ui_description = "Internal image closure evaluation";
  sample_type.declare = file_ns::sample_declare;
  sample_type.initfunc = file_ns::node_init;
  sample_type.gpu_fn = file_ns::sample_gpu;
  sample_type.add_ui_poll = [](const bContext * /*C*/) { return false; };
  sample_type.gather_link_search_ops = nullptr;
  bke::node_type_storage(sample_type,
                         "NodeShaderImageToClosure",
                         node_free_standard_storage,
                         node_copy_standard_storage);
  bke::node_register_type(sample_type);
}

}  // namespace blender
