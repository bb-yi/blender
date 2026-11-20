/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_draw_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_output<decl::Float>("Is Outline Shell"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description(
          "The current material graph is being evaluated for an outline shell. Returns 0 or 1");
  b.add_output<decl::Float>("Is Shadow Pass"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current draw writes a shadow map. Returns 0 or 1");
  b.add_output<decl::Float>("Is Render Texture Capture"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current draw belongs to a Render Texture capture. Returns 0 or 1");
  b.add_output<decl::Float>("Is Probe Capture"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description(
          "The current draw captures a reflection probe or irradiance surfels. Returns 0 or 1");
  b.add_output<decl::Float>("Is Image Render"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description(
          "The host task is a final image render or a viewport image render. Returns 0 or 1");
  b.add_output<decl::Float>("Is Color Bake"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description(
          "The current material graph is evaluated for Eevee color baking. Returns 0 or 1");
  b.add_output<decl::Float>("Is Viewport"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description(
          "The host task draws a viewport, including viewport image rendering. Returns 0 or 1");
  b.add_output<decl::Float>("Is Depth Prepass"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description(
          "The current draw evaluates a depth prepass, excluding shadow maps. Returns 0 or 1");
  b.add_output<decl::Float>("Is Deferred"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description(
          "The current draw evaluates a deferred GBuffer, hybrid, or NPR shading pass. Returns 0 "
          "or 1");
  b.add_output<decl::Float>("Is Forward"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current draw evaluates a forward shading pass. Returns 0 or 1");
  b.add_output<decl::Float>("Is NPR Evaluation"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current graph is evaluated in an NPR shader context. Returns 0 or 1");
  b.add_output<decl::Float>("Is Filter Pass"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description(
          "The current material graph is evaluated as a full-screen Filter material. Returns 0 or "
          "1");
  b.add_output<decl::Float>("Is World"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current material graph is evaluated as a World shader. Returns 0 or 1");
  b.add_output<decl::Float>("Is Vertex Evaluation"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current expression runs in the vertex shader. Returns 0 or 1");
  b.add_output<decl::Float>("Is Fragment Evaluation"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current expression runs in the fragment shader. Returns 0 or 1");
  b.add_output<decl::Float>("Is Volume Evaluation"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current material graph evaluates volume properties. Returns 0 or 1");
  b.add_output<decl::Float>("Is Sphere Probe Capture"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current draw captures a spherical reflection probe. Returns 0 or 1");
  b.add_output<decl::Float>("Is Planar Probe Capture"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current draw captures a planar reflection probe. Returns 0 or 1");
  b.add_output<decl::Float>("Is Irradiance Capture"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The current draw captures surfels for irradiance baking. Returns 0 or 1");
  b.add_output<decl::Float>("Is Pre-FX Render"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("Legacy output; always false because Pre-FX shares the normal scene draw");
  b.add_output<decl::Float>("Is Playback"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The host viewport is playing animation. Returns 0 or 1");
  b.add_output<decl::Float>("Is Navigating"_ustr)
      .min(0.0f)
      .max(1.0f)
      .description("The host viewport is being navigated. Returns 0 or 1");
}

static int node_shader_gpu_draw_info(GPUMaterial *mat,
                                     bNode * /*node*/,
                                     bNodeExecData * /*execdata*/,
                                     GPUNodeStack * /*in*/,
                                     GPUNodeStack *out)
{
  for (int i = 0; i <= SHD_DRAW_INFO_NAVIGATING; i++) {
    if (out[i].hasoutput) {
      const float query = float(i);
      if (!GPU_link(mat, "node_draw_info", GPU_constant(&query), &out[i].link)) {
        return false;
      }
    }
  }
  return true;
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  return val(0.0f);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_draw_info_cc

void register_node_type_sh_draw_info()
{
  namespace file_ns = nodes::node_shader_draw_info_cc;

  static bke::bNodeType ntype;
  sh_node_type_base(&ntype, "ShaderNodeDrawInfo"_ustr, SH_NODE_DRAW_INFO);
  ntype.ui_name = "Draw Info";
  ntype.ui_description =
      "Test the current Eevee material, drawing, capture, or evaluation context";
  ntype.enum_name_legacy = "DRAW_INFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.default_width = 220;
  ntype.declare = file_ns::node_declare;
  ntype.add_ui_poll = eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_shader_gpu_draw_info;
  ntype.materialx_fn = file_ns::node_shader_materialx;
  bke::node_register_type(ntype);
}

}  // namespace blender
