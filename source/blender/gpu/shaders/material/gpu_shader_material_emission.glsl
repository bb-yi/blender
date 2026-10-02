/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_emission(float4 color, float strength, float weight, Closure &result)
{
  color = max(color, float4(0.0f));
  strength = max(strength, 0.0f);

  ClosureEmission emission_data;
  emission_data.weight = weight;
  emission_data.emission = color.rgb * strength;

  result = closure_eval(emission_data);
}

[[node]]
void node_emission_outline_shell(
    float4 color, float alpha, float clip, float threshold, Closure &result)
{
  alpha = saturate(alpha);
  if (clip != 0.0f) {
    alpha = (alpha > 0.0f && alpha >= threshold) ? 1.0f : 0.0f;
  }
  ClosureEmission emission_data;
  emission_data.weight = alpha;
  emission_data.emission = max(color.rgb, float3(0.0f));
  ClosureTransparency transparency_data;
  transparency_data.weight = 1.0f - alpha;
  transparency_data.transmittance = float3(1.0f);
  transparency_data.holdout = 0.0f;
  result = closure_eval(emission_data);
  result = closure_eval(transparency_data);
}
