/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_shader_light_nodes.hh"

#include "DNA_ID.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"

#include "BLI_string.h"

#include "RE_engine.h"

namespace blender::nodes {

bool light_eevee_shader_nodes_poll(const bContext *C)
{
  const SpaceNode *snode = CTX_wm_space_node(C);
  if (snode == nullptr || snode->shaderfrom != SNODE_SHADER_OBJECT || snode->id == nullptr ||
      GS(snode->id->name) != ID_LA)
  {
    return false;
  }
  const RenderEngineType *engine_type = CTX_data_engine_type(C);
  return engine_type != nullptr && STREQ(engine_type->idname, "BLENDER_EEVEE");
}

bool light_eevee_shader_node_type_supported(const StringRefNull idname)
{
  static constexpr StringRefNull supported_nodes[] = {
      "ShaderNodeEeveeLightShaderInfo",
      "ShaderNodeEeveeLightShaderOutput",
      "ShaderNodeRGB",
      "ShaderNodeValue",
      "GeometryNodeInputSceneTime",
      "ShaderNodeBlackbody",
      "ShaderNodeBrightContrast",
      "ShaderNodeValToRGB",
      "ShaderNodeOKLabColorRamp",
      "ShaderNodeGamma",
      "ShaderNodeHueSaturation",
      "ShaderNodeInvert",
      "ShaderNodeMix",
      "ShaderNodeRGBCurve",
      "ShaderNodeWavelength",
      "ShaderNodeCombineColor",
      "ShaderNodeSeparateColor",
      "ShaderNodeRGBToBW",
      "ShaderNodeTexBrick",
      "ShaderNodeTexChecker",
      "ShaderNodeTexGabor",
      "ShaderNodeTexGradient",
      "ShaderNodeTexHexagon",
      "ShaderNodeTexImage",
      "ShaderNodeTexMagic",
      "ShaderNodeTexNoise",
      "ShaderNodeTexVoronoi",
      "ShaderNodeTexWave",
      "ShaderNodeWaterRipples",
      "ShaderNodeTexWhiteNoise",
      "ShaderNodeCombineXYZ",
      "ShaderNodeSeparateXYZ",
      "ShaderNodeMapRange",
      "ShaderNodeMapping",
      "ShaderNodeNormal",
      "ShaderNodeVectorCurve",
      "ShaderNodeVectorMath",
      "ShaderNodeVectorRotate",
      "ShaderNodeVectorTransform",
      "ShaderNodeClamp",
      "ShaderNodeFloatCurve",
      "ShaderNodeMath",
      "NodeFrame",
      "NodeReroute",
  };
  for (const StringRefNull supported_node : supported_nodes) {
    if (idname == supported_node) {
      return true;
    }
  }
  return false;
}

}  // namespace blender::nodes
