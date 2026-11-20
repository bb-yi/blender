/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"

[[node]]
void node_emission(float4 color,
                   float strength,
                   float weight,
                   KernelGlobals &kg,
                   ShadingData &sd,
                   Closure &result)
{
  color = max(color, float4(0.0f));
  strength = max(strength, 0.0f);

  ClosureEmission emission_data;
  emission_data.emission = color.rgb * (strength * weight);

  result = closure_eval(kg, sd, emission_data);
}

[[node]]
void node_emission_outline_shell(float4 color,
                                 float alpha,
                                 float clip,
                                 float threshold,
                                 KernelGlobals &kg,
                                 ShadingData &sd,
                                 Closure &result)
{
  alpha = saturate(alpha);
  if (clip != 0.0f) {
    alpha = (alpha > 0.0f && alpha >= threshold) ? 1.0f : 0.0f;
  }
  ClosureEmission emission_data;
  emission_data.emission = max(color.rgb, float3(0.0f)) * alpha;
  ClosureTransparency transparency_data;
  transparency_data.transmittance = float3(1.0f - alpha);
  transparency_data.holdout = 0.0f;
  result = closure_eval(kg, sd, emission_data);
  result = closure_eval(kg, sd, transparency_data);
}
