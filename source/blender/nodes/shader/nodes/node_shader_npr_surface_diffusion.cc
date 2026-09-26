/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"
#include "BKE_context.hh"
#include "DNA_scene_types.h"
#include "NOD_node_extra_info.hh"
#include "RE_engine.h"
#include "UI_resources.hh"

#include <sstream>

namespace blender {
namespace nodes::node_shader_npr_surface_diffusion_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNodeTree *tree = b.tree_or_null();
  b.add_input<decl::Shader>("Shader"_ustr)
      .description("Shaded surface to diffuse, including highlights. Colors are accepted without relighting");
  b.add_input<decl::Float>("Strength"_ustr)
      .default_value(1.0f).min(0.0f).max(1.0f).subtype(PROP_FACTOR)
      .description("Blend between the original surface and its diffused color; Alpha is unchanged");
  b.add_input<decl::Vector>("Radius"_ustr)
      .default_value({1.0f, 1.0f, 1.0f}).min(0.0f).max(100.0f)
      .description("Relative diffusion distances for red, green and blue; zero preserves that channel");
  b.add_input<decl::Float>("Scale"_ustr)
      .default_value(0.005f).min(0.0f).max(10.0f).subtype(PROP_DISTANCE)
      .description("Overall diffusion distance in world units");
  b.add_input<decl::Float>("Weight"_ustr)
      .available(tree && (tree->flag & NTREE_IS_GPU_SHADER_INTERNAL));
  b.add_output<decl::Shader>("Shader"_ustr);
}

static void node_extra_info(NodeExtraInfoParams &params)
{
  {
    NodeExtraInfoRow row;
    row.text = RPT_("Experimental");
    row.tooltip = TIP_("This node is experimental and may change in future versions");
    row.icon = ICON_EXPERIMENTAL;
    params.rows.append(std::move(row));
  }
  const Scene *scene = CTX_data_scene(&params.C);
  if (scene != nullptr && StringRef(scene->r.engine) == RE_engine_id_BLENDER_EEVEE) {
    return;
  }
  NodeExtraInfoRow row;
  row.text = RPT_("Eevee Only");
  row.tooltip = TIP_("NPR Surface Diffusion is evaluated only by the Eevee render engine");
  row.icon = ICON_ERROR;
  params.rows.append(std::move(row));
}

static int node_gpu(GPUMaterial *mat, bNode *node, bNodeExecData *,
                    GPUNodeStack *in, GPUNodeStack *out)
{
  if (!in[0].link) {
    return GPU_stack_link(mat, node, "npr_surface_diffusion_empty", nullptr, out);
  }
  GPU_material_surface_diffusion_set(mat);
  GPU_material_principled_npr_v2_set(mat);
  GPU_material_flag_set(mat, GPU_MATFLAG_DIFFUSE | GPU_MATFLAG_SUBSURFACE);

  /* Capture only this input branch. Its locally inverted weights start at one;
   * the outer Mix/Add weight is applied by the evaluation scope below. */
  GPUNodeLink *branch = nullptr;
  GPU_link(mat, "npr_surface_diffusion_capture", in[0].link, &branch);
  const char *function = GPU_material_split_sub_function(
      mat, GPU_FLOAT, &branch, "gpu_shader_material_npr_surface_diffusion.glsl");
  const std::string wrapper = std::string("npr_diffusion_") + function;
  const std::string filename = wrapper + ".glsl";
  std::stringstream source;
  source << "float " << function << "();\nvoid " << wrapper
         << "(float strength, float3 radius, float scale, float weight, out Closure result) {\n"
         << "float4 saved = g_npr_diffusion;\n"
         << "float saved_weight = g_npr_diffusion_weight;\n"
         << "bool saved_scope = g_npr_diffusion_scope;\n"
         << "g_npr_diffusion_scope = true;\n"
         << "g_npr_diffusion = float4(max(radius, float3(0.0f)) * max(scale, 0.0f) * "
            "12.566370614359172f, clamp(strength, 0.0f, 1.0f));\n"
         << "g_npr_diffusion_weight *= weight;\n"
         << "result = " << function << "();\n"
         << "g_npr_diffusion = saved;\n"
         << "g_npr_diffusion_scope = saved_scope;\n"
         << "g_npr_diffusion_weight = saved_weight;\n}\n";
  GPU_material_generated_source_add(mat, filename.c_str(),
      {"gpu_shader_material_npr_surface_diffusion.glsl"}, source.str().c_str());
  in[0].type = GPU_NONE;
  in[0].link = nullptr;
  return GPU_stack_link_custom(mat, node, wrapper.c_str(), filename.c_str(),
                               GPU_CUSTOM_NODE_DEPENDENCY_NONE, in, out);
}

}  // namespace nodes::node_shader_npr_surface_diffusion_cc

void register_node_type_sh_npr_surface_diffusion()
{
  namespace file_ns = nodes::node_shader_npr_surface_diffusion_cc;
  static bke::bNodeType ntype;
  sh_node_type_base(
      &ntype, "ShaderNodeNPRSurfaceDiffusion"_ustr, SH_NODE_NPR_SURFACE_DIFFUSION);
  ntype.ui_name = "NPR Surface Diffusion";
  ntype.ui_description = "Diffuse the shaded input surface in screen space without relighting it. "
                        "EEVEE deferred surfaces only; coverage is preserved";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = file_ns::node_declare;
  ntype.get_extra_info = file_ns::node_extra_info;
  ntype.add_ui_poll = object_eevee_shader_nodes_poll;
  ntype.gpu_fn = file_ns::node_gpu;
  bke::node_register_type(ntype);
}
}  // namespace blender
