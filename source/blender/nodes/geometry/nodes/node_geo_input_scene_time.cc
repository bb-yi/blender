/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <cmath>

#include "BKE_scene.hh"

#include "DEG_depsgraph_query.hh"

#include "node_geometry_util.hh"
#include "shader/node_shader_util.hh"

#include "node_util.hh"

#include "RNA_access.hh"

namespace blender::nodes::node_geo_input_scene_time_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Scale"_ustr)
      .default_value(1.0f);
  b.add_output<decl::Float>("Frame"_ustr);
  b.add_output<decl::Float>("Seconds"_ustr);
  b.add_output<decl::Float>("Timeline"_ustr);
  b.add_output<decl::Float>("Scaled Frame"_ustr);
}

static void node_exec(GeoNodeExecParams params)
{
  const Scene *scene = DEG_get_input_scene(params.depsgraph());
  const float frame = scene ? BKE_scene_frame_get(scene) : 0.0f;
  const float frame_start = scene ? float(scene->r.sfra) : 0.0f;
  const float frame_end = scene ? float(scene->r.efra) : frame_start;
  const float frame_range = frame_end - frame_start;
  const float fps = (scene && scene->r.frs_sec_base != 0.0f) ?
                        (float(scene->r.frs_sec) / scene->r.frs_sec_base) :
                        24.0f;
  const float seconds = (std::abs(fps) > 1e-8f) ? frame / fps : 0.0f;
  const float timeline = (std::abs(frame_range) > 1e-8f) ?
                             std::clamp((frame - frame_start) / frame_range, 0.0f, 1.0f) :
                             0.0f;
  const float scale = params.extract_input<float>("Scale"_ustr);
  const float scale_safe = (std::abs(scale) > 1e-8f) ? scale : 1.0f;

  params.set_output("Frame"_ustr, frame);
  params.set_output("Seconds"_ustr, seconds);
  params.set_output("Timeline"_ustr, timeline);
  params.set_output("Scaled Frame"_ustr, frame / scale_safe);
}

static int node_shader_gpu(GPUMaterial *mat,
                           bNode *node,
                           bNodeExecData * /*execdata*/,
                           GPUNodeStack *in,
                           GPUNodeStack *out)
{
  GPU_material_set_time_dependent(mat);
  return GPU_stack_link(mat, node, "node_scene_time", in, out);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  return get_output_default(socket_out_->identifier, NodeItem::Type::Any);
}
#endif
NODE_SHADER_MATERIALX_END

static void node_register()
{
  static bke::bNodeType ntype;
  sh_geo_node_type_base(&ntype, "GeometryNodeInputSceneTime"_ustr, GEO_NODE_INPUT_SCENE_TIME);
  ntype.ui_name = "Scene Time";
  ntype.ui_description =
      "Retrieve the current scene time in frames, seconds, normalized timeline, or scaled frames";
  ntype.enum_name_legacy = "INPUT_SCENE_TIME";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_exec;
  ntype.declare = node_declare;
  ntype.gpu_fn = node_shader_gpu;
  ntype.materialx_fn = node_shader_materialx;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_scene_time_cc
