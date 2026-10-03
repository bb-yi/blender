#pragma once

#include "draw_math_geom_lib.glsl"
#include "gpu_shader_math_vector_safe_lib.glsl"

struct HardwareEnvironmentVisibilityData {
  float3 average_direction;
  float visibility;
  float validity;
};

float hardware_environment_visibility_validity(float3 average_direction, float visibility)
{
  /* The Metal producer clears and early-outs to `(0, 0, 0, 1)`. A traced sample should either bend
   * the direction away from zero or report partial occlusion. Treat only those texels as
   * authoritative HW environment visibility. */
  return ((visibility < 0.9999f) || (dot(average_direction, average_direction) > 1.0e-8f)) ? 1.0f :
                                                                                              0.0f;
}

HardwareEnvironmentVisibilityData hardware_environment_visibility_load(int2 texel,
                                                                      float3 fallback_N)
{
  HardwareEnvironmentVisibilityData data;
  float4 visibility_data = texelFetch(hardware_rt_environment_visibility_tx, texel, 0);
  data.average_direction = visibility_data.xyz;
  data.visibility = saturate(visibility_data.w);
  data.validity = hardware_environment_visibility_validity(data.average_direction, data.visibility);
  if (data.validity < 0.5f) {
    data.visibility = 1.0f;
  }
  if (dot(data.average_direction, data.average_direction) <= 1.0e-8f) {
    data.average_direction = safe_normalize(fallback_N) * (2.0f / 3.0f);
  }
  return data;
}

bool hardware_environment_visibility_is_valid(HardwareEnvironmentVisibilityData data)
{
  return data.validity >= 0.5f;
}

/* Cross-filtered visibility for radiance attenuation. The producer traces few dome rays per
 * frame, so a single texel is heavily quantized and flickers temporally in partially occluded
 * areas. Averaging a 5-tap cross of valid neighbors multiplies the effective sample count
 * without smearing across depth discontinuities more than one tile. */
HardwareEnvironmentVisibilityData hardware_environment_visibility_load_filtered(int2 texel,
                                                                                float3 fallback_N)
{
  HardwareEnvironmentVisibilityData center = hardware_environment_visibility_load(texel, fallback_N);
  if (!hardware_environment_visibility_is_valid(center)) {
    return center;
  }
  float visibility_sum = center.visibility;
  float weight_sum = 1.0f;
  constexpr int2 offsets[4] = int2_array(int2(2, 0), int2(-2, 0), int2(0, 2), int2(0, -2));
  const int2 max_texel = textureSize(hardware_rt_environment_visibility_tx, 0).xy - 1;
  for (int i = 0; i < 4; i++) {
    const int2 neighbor_texel = clamp(texel + offsets[i], int2(0), max_texel);
    float4 neighbor = texelFetch(hardware_rt_environment_visibility_tx, neighbor_texel, 0);
    if (hardware_environment_visibility_validity(neighbor.xyz, saturate(neighbor.w)) >= 0.5f) {
      visibility_sum += saturate(neighbor.w);
      weight_sum += 1.0f;
    }
  }
  center.visibility = visibility_sum / weight_sum;
  return center;
}

float3 hardware_environment_visibility_direction(HardwareEnvironmentVisibilityData data,
                                                 float3 query_direction,
                                                 float3 fallback_N)
{
  float3 bent_direction = data.average_direction;
  if (dot(bent_direction, bent_direction) > 1.0e-8f) {
    bent_direction = normalize(bent_direction);
  }
  else {
    bent_direction = safe_normalize(fallback_N);
  }

  float3 query = safe_normalize(query_direction);
  float mix_fac = saturate(1.0f - data.visibility);
  return safe_normalize(mix(query, bent_direction, mix_fac));
}
