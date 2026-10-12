/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Evaluate a screen-space direct-light modifier for forward and Shader to RGB passes.
 *
 * This runs before the GBuffer exists, so it reconstructs the front-most visible surface from the
 * prepass depth and normal buffers.
 */

#include "infos/eevee_light_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_light_shader_front)

#include "draw_view_lib.glsl"
#include "eevee_attributes_world_lib.bsl.hh"
#include "eevee_light_lib.glsl"
#include "eevee_light_shader_common_lib.glsl"
#include "eevee_nodetree_frag_lib.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"

float4 nodetree_light_shader(KernelGlobals kg, ShadingData &sd);

void main()
{
  [[resource_table]] KernelGlobals &kg = resource_table_get(KernelGlobals);
  ShadingData sd;
  light_shader_globals_init(sd);
  sd.frag_co = gl_FragCoord;

  const int2 texel = int2(gl_FragCoord.xy);
  const int2 extent = textureSize(prepass_normal_tx, 0);
  if (any(greaterThanEqual(texel, extent))) {
    out_light_shader = float4(1.0f);
    return;
  }

  const float depth = texelFetch(hiz_tx, texel, 0).r;
  if (depth == 1.0f) {
    out_light_shader = float4(1.0f);
    return;
  }

  const float2 screen_uv = (float2(texel) + 0.5f) / float2(extent);
  const float3 P = drw_point_screen_to_world(float3(screen_uv, depth));
  const float3 packed_normal = texelFetch(prepass_normal_tx, texel, 0).rgb;
  if (!any(greaterThan(packed_normal, float3(0.0f)))) {
    out_light_shader = float4(1.0f);
    return;
  }
  const float3 N = normalize(packed_normal * 2.0f - 1.0f);

  sd.P = P;
  sd.N = sd.Ni = N;
  sd.Ng = N;
  sd.ray_length = distance(P, drw_view_position());

  attrib_load(WorldPoint{float3(0.0f)});

  out_light_shader = light_shader_result_clamp(nodetree_light_shader(kg, sd));
}
