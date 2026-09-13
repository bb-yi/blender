/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_material_glsl_light_access.glsl"

#define PRINCIPLED_NPR_DIFFUSE_SIMPLE 0
#define PRINCIPLED_NPR_DIFFUSE_RAMP 1
#define PRINCIPLED_NPR_DIFFUSE_DRIVEN_RAMP 2
#define PRINCIPLED_NPR_SPECULAR_SIMPLE 0
#define PRINCIPLED_NPR_SPECULAR_RAMP 1
#define PRINCIPLED_NPR_RANGE_FRONT 0
#define PRINCIPLED_NPR_RANGE_FULL 1
#define PRINCIPLED_NPR_COLOR_MULTIPLY 0
#define PRINCIPLED_NPR_MAPPING_PER_LIGHT 0
#define PRINCIPLED_NPR_MAPPING_COMBINED 1
#define PRINCIPLED_NPR_LIGHT_ADD 0
#define PRINCIPLED_NPR_LIGHT_STRONGEST 1
#define PRINCIPLED_NPR_SHADOW_NONE 0
#define PRINCIPLED_NPR_SHADOW_CAST_ONLY 1
#define PRINCIPLED_NPR_INTERP_CONSTANT 0
#define PRINCIPLED_NPR_INTERP_LINEAR 1
#define PRINCIPLED_NPR_MAX_STOPS 8

float principled_npr_luminance(float3 color)
{
  return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 principled_npr_safe_normalize(float3 value, float3 fallback)
{
  float length_squared = dot(value, value);
  return (length_squared > 1e-16f) ? value * inversesqrt(length_squared) : fallback;
}

float principled_npr_light_energy_bias(float energy, float influence)
{
  /* Use exposure-like growth so ordinary Blender light values do not immediately saturate the
   * mapping coordinate while brighter lights still push the NPR boundary. */
  return log2(1.0f + max(energy, 0.0f)) * 0.03125f * saturate(influence);
}

float principled_npr_light_energy_response(float energy)
{
  /* Keep direct color response bounded while retaining visible changes for dim lights. */
  return max(energy, 0.0f) / (1.0f + max(energy, 0.0f));
}

float principled_npr_gradient(float value, float boundary, float softness)
{
  /* The anisotropic half-vector coordinate is 1 at the lobe center. Convert it to distance from
   * the center, then use the exact NPR boundary interval [boundary - softness,
   * boundary + softness]. */
  float edge_distance = 1.0f - saturate(value);
  float center = saturate(boundary);
  float width = max(softness, 0.0f);
  if (width <= 1e-6f) {
    return 1.0f - step(center, edge_distance);
  }
  float minimum = center - width;
  float maximum = center + width;
  return 1.0f - smoothstep(minimum, maximum, edge_distance);
}

void principled_npr_unpack_modes(float mode0,
                                 float mode1,
                                 int &diffuse_mapping,
                                 int &specular_mapping,
                                 int &coordinate_range,
                                 int &color_application,
                                 int &mapping_stage,
                                 int &light_combine,
                                 int &shadow_mode,
                                 int &driven_interpolation,
                                 int &driven_stop_count,
                                 int &lightgroup_id)
{
  int packed0 = int(max(mode0, 0.0f) + 0.5f);
  diffuse_mapping = packed0 % 10;
  packed0 /= 10;
  specular_mapping = packed0 % 10;
  packed0 /= 10;
  coordinate_range = packed0 % 10;
  packed0 /= 10;
  color_application = packed0 % 10;
  packed0 /= 10;
  mapping_stage = packed0 % 10;
  packed0 /= 10;
  light_combine = packed0 % 10;
  packed0 /= 10;
  shadow_mode = packed0 % 10;

  int packed1 = int(max(mode1, 0.0f) + 0.5f);
  driven_interpolation = packed1 % 10;
  packed1 /= 10;
  driven_stop_count = clamp(packed1 % 10, 2, PRINCIPLED_NPR_MAX_STOPS);
  packed1 /= 10;
  lightgroup_id = max(packed1, 0);
}

float principled_npr_shadow_visibility(uint light_index,
                                       bool is_local,
                                       float3 shading_normal,
                                       int shadow_mode)
{
  if (shadow_mode == PRINCIPLED_NPR_SHADOW_NONE) {
    return 1.0f;
  }
  if (shadow_mode == PRINCIPLED_NPR_SHADOW_CAST_ONLY) {
    return glsl_light_cast_shadow_raw(light_index, is_local, shading_normal);
  }
  return glsl_light_shadow_raw(light_index, is_local, shading_normal);
}

float4 principled_npr_simple_map(float coordinate,
                                 float4 shadow_color,
                                 float4 lit_color,
                                 float boundary,
                                 float softness)
{
  float width = max(softness, 0.0f);
  float factor = (width <= 1e-6f) ? step(boundary, coordinate) :
                                    smoothstep(boundary - width * 0.5f,
                                               boundary + width * 0.5f,
                                               coordinate);
  return mix(max(shadow_color, float4(0.0f)), max(lit_color, float4(0.0f)), factor);
}

float4 principled_npr_driven_map(float coordinate,
                                 int interpolation,
                                 int stop_count,
                                 float4 c0,
                                 float4 c1,
                                 float4 c2,
                                 float4 c3,
                                 float4 c4,
                                 float4 c5,
                                 float4 c6,
                                 float4 c7,
                                 float4 positions_0_3,
                                 float4 positions_4_7)
{
  float4 colors[PRINCIPLED_NPR_MAX_STOPS];
  float positions[PRINCIPLED_NPR_MAX_STOPS];
  colors[0] = max(c0, float4(0.0f));
  colors[1] = max(c1, float4(0.0f));
  colors[2] = max(c2, float4(0.0f));
  colors[3] = max(c3, float4(0.0f));
  colors[4] = max(c4, float4(0.0f));
  colors[5] = max(c5, float4(0.0f));
  colors[6] = max(c6, float4(0.0f));
  colors[7] = max(c7, float4(0.0f));
  positions[0] = saturate(positions_0_3.x);
  positions[1] = max(positions[0], saturate(positions_0_3.y));
  positions[2] = max(positions[1], saturate(positions_0_3.z));
  positions[3] = max(positions[2], saturate(positions_0_3.w));
  positions[4] = max(positions[3], saturate(positions_4_7.x));
  positions[5] = max(positions[4], saturate(positions_4_7.y));
  positions[6] = max(positions[5], saturate(positions_4_7.z));
  positions[7] = max(positions[6], saturate(positions_4_7.w));

  int count = clamp(stop_count, 2, PRINCIPLED_NPR_MAX_STOPS);
  if (coordinate <= positions[0]) {
    return colors[0];
  }
  for (int i = 1; i < count; i++) {
    if (coordinate <= positions[i]) {
      float denominator = max(positions[i] - positions[i - 1], 1e-6f);
      float factor = saturate((coordinate - positions[i - 1]) / denominator);
      if (interpolation == PRINCIPLED_NPR_INTERP_CONSTANT) {
        factor = step(positions[i], coordinate);
      }
      else if (interpolation != PRINCIPLED_NPR_INTERP_LINEAR) {
        factor = factor * factor * (3.0f - 2.0f * factor);
      }
      return mix(colors[i - 1], colors[i], factor);
    }
  }
  return colors[count - 1];
}

float4 principled_npr_diffuse_map(float coordinate,
                                  int diffuse_mapping,
                                  int driven_interpolation,
                                  int driven_stop_count,
                                  float4 shadow_color,
                                  float4 lit_color,
                                  float boundary,
                                  float softness,
                                  sampler1DArray diffuse_ramp,
                                  float diffuse_ramp_layer,
                                  float4 c0,
                                  float4 c1,
                                  float4 c2,
                                  float4 c3,
                                  float4 c4,
                                  float4 c5,
                                  float4 c6,
                                  float4 c7,
                                  float4 positions_0_3,
                                  float4 positions_4_7)
{
  float mapped_coordinate = saturate(coordinate);
  if (diffuse_mapping == PRINCIPLED_NPR_DIFFUSE_RAMP) {
    return max(texture(diffuse_ramp, float2(mapped_coordinate, diffuse_ramp_layer)), float4(0.0f));
  }
  if (diffuse_mapping == PRINCIPLED_NPR_DIFFUSE_DRIVEN_RAMP) {
    return principled_npr_driven_map(mapped_coordinate,
                                     driven_interpolation,
                                     driven_stop_count,
                                     c0,
                                     c1,
                                     c2,
                                     c3,
                                     c4,
                                     c5,
                                     c6,
                                     c7,
                                     positions_0_3,
                                     positions_4_7);
  }
  return principled_npr_simple_map(
      mapped_coordinate, shadow_color, lit_color, boundary, softness);
}

float3 principled_npr_apply_diffuse_color(float3 base_color,
                                          float4 mapped_color,
                                          int color_application)
{
  float3 target = (color_application == PRINCIPLED_NPR_COLOR_MULTIPLY) ?
                      base_color * mapped_color.rgb :
                      mapped_color.rgb;
  return mix(base_color, target, saturate(mapped_color.a));
}

float4 principled_npr_specular_map(float coordinate,
                                   int specular_mapping,
                                   float size,
                                   float softness,
                                   float offset,
                                   sampler1DArray specular_ramp,
                                   float specular_ramp_layer)
{
  float mask = principled_npr_gradient(coordinate + offset, size, softness);
  if (specular_mapping == PRINCIPLED_NPR_SPECULAR_RAMP) {
    float4 mapped = max(texture(specular_ramp, float2(saturate(coordinate + offset),
                                                      specular_ramp_layer)),
                        float4(0.0f));
    mapped.a *= mask;
    return mapped;
  }
  return float4(1.0f, 1.0f, 1.0f, mask);
}

void principled_npr_tangent_basis(float3 N,
                                  float3 tangent,
                                  float rotation,
                                  float3 &T,
                                  float3 &B)
{
  T = tangent - N * dot(N, tangent);
  if (dot(T, T) <= 1e-10f) {
    float3 axis = (abs(N.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) :
                                       float3(1.0f, 0.0f, 0.0f);
    T = cross(axis, N);
  }
  T = principled_npr_safe_normalize(T, float3(1.0f, 0.0f, 0.0f));
  B = principled_npr_safe_normalize(cross(N, T), float3(0.0f, 1.0f, 0.0f));
  float angle = rotation * 6.28318530718f;
  float3 rotated_tangent = T * cos(angle) + B * sin(angle);
  T = principled_npr_safe_normalize(rotated_tangent, T);
  B = principled_npr_safe_normalize(cross(N, T), B);
}

float principled_npr_anisotropic_lobe(float3 N,
                                      float3 V,
                                      float3 L,
                                      float3 T,
                                      float3 B,
                                      float anisotropy)
{
  float3 H = principled_npr_safe_normalize(V + L, N);
  float NoH = saturate(dot(N, H));
  float aspect = sqrt(max(1.0f - 0.9f * saturate(anisotropy), 0.1f));
  float tangent_x = dot(T, H) * aspect;
  float tangent_y = dot(B, H) / aspect;
  float stretched_tangent = sqrt(tangent_x * tangent_x + tangent_y * tangent_y);
  /* Return a half-vector coordinate for the NPR shape controls. The only lobe roughness is the
   * PBR roughness used by the GGX evaluation below; Size and Softness only remap this coordinate. */
  return NoH / max(sqrt(NoH * NoH + stretched_tangent * stretched_tangent), 1e-4f);
}

float principled_npr_anisotropic_g1(float3 N,
                                    float3 direction,
                                    float3 T,
                                    float3 B,
                                    float alpha_x,
                                    float alpha_y)
{
  float NoDirection = saturate(dot(N, direction));
  if (NoDirection <= 1e-6f) {
    return 0.0f;
  }

  float tangent_projection = dot(T, direction);
  float bitangent_projection = dot(B, direction);
  float projected_alpha_squared = alpha_x * alpha_x * tangent_projection * tangent_projection +
                                  alpha_y * alpha_y * bitangent_projection * bitangent_projection;
  float lambda = 0.5f *
                 (sqrt(1.0f + projected_alpha_squared /
                                      max(NoDirection * NoDirection, 1e-6f)) -
                  1.0f);
  return 1.0f / (1.0f + lambda);
}

float principled_npr_ggx_direct_lobe(float3 N,
                                     float3 V,
                                     float3 L,
                                     float3 T,
                                     float3 B,
                                     float roughness,
                                     float anisotropy)
{
  float NoL = saturate(dot(N, L));
  float NoV = saturate(dot(N, V));
  if (NoL <= 1e-6f || NoV <= 1e-6f) {
    return 0.0f;
  }

  float3 H = principled_npr_safe_normalize(V + L, N);
  float NoH = saturate(dot(N, H));
  float aspect = sqrt(max(1.0f - 0.9f * saturate(anisotropy), 0.1f));
  float alpha = max(saturate(roughness) * saturate(roughness), 1e-3f);
  float alpha_x = max(alpha / aspect, 1e-3f);
  float alpha_y = max(alpha * aspect, 1e-3f);
  float tangent_half = dot(T, H);
  float bitangent_half = dot(B, H);
  float distribution_denominator = tangent_half * tangent_half / (alpha_x * alpha_x) +
                                   bitangent_half * bitangent_half / (alpha_y * alpha_y) +
                                   NoH * NoH;
  float distribution = 1.0f /
                       max(3.14159265359f * alpha_x * alpha_y *
                               distribution_denominator * distribution_denominator,
                           1e-6f);
  float geometry_v = principled_npr_anisotropic_g1(N, V, T, B, alpha_x, alpha_y);
  float geometry_l = principled_npr_anisotropic_g1(N, L, T, B, alpha_x, alpha_y);
  return distribution * geometry_v * geometry_l * NoL / max(4.0f * NoV, 1e-6f);
}

float3 principled_npr_fresnel(float3 F0, float cos_theta)
{
  float one_minus_cos = 1.0f - saturate(cos_theta);
  float one_minus_cos_squared = one_minus_cos * one_minus_cos;
  float fresnel_power = one_minus_cos_squared * one_minus_cos_squared * one_minus_cos;
  return F0 + (float3(1.0f) - F0) * fresnel_power;
}

void principled_npr_probe(float3 N,
                          float3 V,
                          float roughness,
                          float ambient_directionality,
                          float3 &reflection,
                          float3 &irradiance)
{
  reflection = float3(0.0f);
  irradiance = float3(0.0f);
#if defined(GPU_FRAGMENT_SHADER) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD) || defined(NPR_SHADER))
#  if defined(CREATE_INFO_eevee_LightprobeRenderData)
  float3 geometry_normal = principled_npr_safe_normalize(g_data.Ng, float3(0.0f, 0.0f, 1.0f));
  float3 reflection_direction = principled_npr_safe_normalize(reflect(-V, N), N);
  float reflection_lod = eevee::lightprobe::sphere::roughness_to_lod(saturate(roughness));

  [[resource_table]] const eevee::LightprobeRenderData &lightprobes = resource_table_get(
      eevee::LightprobeRenderData);
  [[resource_table]] const eevee::LightprobeSphereRenderData &lp_spheres = lightprobes.spheres;
  eevee::LightProbeSample probe_data = lightprobes.load(gl_FragCoord.xy, g_data.P, geometry_normal, -V);
  probe_data.volume_irradiance = spherical_harmonics::clamp_energy(
      probe_data.volume_irradiance, uniform_buf.clamp.surface_indirect);

  reflection = lp_spheres.spherical_sample_normalized_with_parallax(
      probe_data, g_data.P, reflection_direction, reflection_lod);
  float3 directional = max(probe_data.volume_irradiance.evaluate_lambert(N).rgb, float3(0.0f));
  const float3 axes[6] = {float3(1.0f, 0.0f, 0.0f),
                          float3(-1.0f, 0.0f, 0.0f),
                          float3(0.0f, 1.0f, 0.0f),
                          float3(0.0f, -1.0f, 0.0f),
                          float3(0.0f, 0.0f, 1.0f),
                          float3(0.0f, 0.0f, -1.0f)};
  float3 equalized = float3(0.0f);
  for (int i = 0; i < 6; i++) {
    equalized += max(probe_data.volume_irradiance.evaluate_lambert(axes[i]).rgb, float3(0.0f));
  }
  equalized /= 6.0f;
  irradiance = mix(equalized, directional, saturate(ambient_directionality));
#  endif
#endif
}

float principled_npr_rim(float3 N,
                         float3 V,
                         float angle_degrees,
                         float rim_length,
                         float length_falloff,
                         float thickness,
                         float thickness_falloff)
{
  float3 camera_x = float3(1.0f, 0.0f, 0.0f);
  float3 camera_y = float3(0.0f, 1.0f, 0.0f);
#if defined(GPU_FRAGMENT_SHADER)
  camera_x = principled_npr_safe_normalize(drw_view().viewinv[0].xyz, camera_x);
  camera_y = principled_npr_safe_normalize(drw_view().viewinv[1].xyz, camera_y);
#endif
  float2 projected_normal = float2(dot(N, camera_x), dot(N, camera_y));
  float projected_length = length(projected_normal);
  if (projected_length <= 1e-6f || rim_length <= 1e-6f || thickness <= 1e-6f) {
    return 0.0f;
  }
  projected_normal /= projected_length;
  float turns = atan(projected_normal.y, projected_normal.x) * (0.5f / 3.14159265359f);
  float centered = abs(fract(turns - angle_degrees / 360.0f + 0.5f) - 0.5f);
  float half_length = saturate(rim_length) * 0.5f;
  float arc_falloff = max(length_falloff, 0.0f) * 0.5f;
  float arc = (arc_falloff <= 1e-6f) ? 1.0f - step(half_length, centered) :
                                      1.0f - smoothstep(max(half_length - arc_falloff, 0.0f),
                                                        half_length,
                                                        centered);
  float facing = saturate(1.0f - dot(N, V));
  float edge = 1.0f - saturate(thickness);
  float width = max(thickness_falloff, 0.0f);
  float thickness_mask = (width <= 1e-6f) ? step(edge, facing) :
                                           smoothstep(edge - width * 0.5f,
                                                      edge + width * 0.5f,
                                                      facing);
  return arc * thickness_mask;
}

[[node]]
void principled_npr_pack4(float x, float y, float z, float w, float4 &result)
{
  result = float4(x, y, z, w);
}

[[node]]
void node_principled_npr_v1(float4 base_color,
                            float metallic,
                            float roughness,
                            float alpha,
                            float3 N,
                            float weight,
                            float4 shadow_color,
                            float4 lit_color,
                            float4 diffuse_controls,
                            float4 lighting_controls,
                            float4 highlight_color,
                            float4 highlight_controls,
                            float4 highlight_details,
                            float3 tangent,
                            float4 environment_controls,
                            float4 rim_color,
                            float4 rim_controls,
                            float4 rim_details,
                            float4 emission_color,
                            float emission_strength,
                            float4 stop_color_0,
                            float4 stop_color_1,
                            float4 stop_color_2,
                            float4 stop_color_3,
                            float4 stop_color_4,
                            float4 stop_color_5,
                            float4 stop_color_6,
                            float4 stop_color_7,
                            float4 stop_positions_0_3,
                            float4 stop_positions_4_7,
                            sampler1DArray diffuse_ramp,
                            sampler1DArray specular_ramp,
                            float4 mode_controls,
                            Closure &shader,
                            float4 &color,
                            float &alpha_out)
{
  float diffuse_ramp_layer = mode_controls.x;
  float specular_ramp_layer = mode_controls.y;
  float mode0 = mode_controls.z;
  float mode1 = mode_controls.w;
  int diffuse_mapping;
  int specular_mapping;
  int coordinate_range;
  int color_application;
  int mapping_stage;
  int light_combine;
  int shadow_mode;
  int driven_interpolation;
  int driven_stop_count;
  int lightgroup_id;
  principled_npr_unpack_modes(mode0,
                              mode1,
                              diffuse_mapping,
                              specular_mapping,
                              coordinate_range,
                              color_application,
                              mapping_stage,
                              light_combine,
                              shadow_mode,
                              driven_interpolation,
                              driven_stop_count,
                              lightgroup_id);

  float boundary = diffuse_controls.x;
  float softness = diffuse_controls.y;
  float coordinate_scale = diffuse_controls.z;
  float coordinate_offset = diffuse_controls.w;
  float shadow_strength = lighting_controls.x;
  float direct_strength = lighting_controls.y;
  float intensity_influence = lighting_controls.z;
  float light_color_influence = lighting_controls.w;
  float highlight_strength = highlight_controls.x;
  float highlight_size = highlight_controls.y;
  float highlight_softness = highlight_controls.z;
  float highlight_offset = highlight_details.x;
  float highlight_light_color_influence = highlight_details.y;
  float anisotropy = highlight_details.z;
  float anisotropy_rotation = highlight_details.w;
  float reflection_strength = environment_controls.x;
  float ambient_strength = environment_controls.y;
  float ambient_directionality = environment_controls.z;
  float rim_strength = rim_controls.x;
  float rim_angle = rim_controls.y;
  float rim_length = rim_controls.z;
  float rim_length_falloff = rim_controls.w;
  float rim_thickness = rim_details.x;
  float rim_thickness_falloff = rim_details.y;
  float rim_light_bias = rim_details.z;
  float rim_mask = rim_details.w;

  base_color = max(base_color, float4(0.0f));
  metallic = saturate(metallic);
  alpha = saturate(alpha);
  N = principled_npr_safe_normalize(N, principled_npr_safe_normalize(g_data.N, g_data.Ng));
  float3 V = principled_npr_safe_normalize(coordinate_incoming(g_data.P), N);
  float3 F0 = mix(float3(0.04f), base_color.rgb, metallic);
  float NoV = saturate(dot(N, V));
  float3 T;
  float3 B;
  principled_npr_tangent_basis(N, tangent, anisotropy_rotation, T, B);

  float safe_shadow_strength = saturate(shadow_strength);
  float safe_intensity_influence = saturate(intensity_influence);
  float safe_light_color_influence = saturate(light_color_influence);
  float safe_highlight_light_influence = saturate(highlight_light_color_influence);
  bool use_strongest = light_combine == PRINCIPLED_NPR_LIGHT_STRONGEST;

  float3 per_light_add = float3(0.0f);
  float3 strongest_diffuse = float3(0.0f);
  float3 specular_add = float3(0.0f);
  float3 strongest_specular = float3(0.0f);
  float strongest_score = -1.0f;
  float strongest_raw_coordinate = 0.0f;
  float strongest_energy = 0.0f;
  float3 strongest_tint = float3(1.0f);
  float aggregate_raw_coordinate = 0.0f;
  float aggregate_energy = 0.0f;
  float3 aggregate_tint = float3(0.0f);
  float3 aggregate_diffuse_weight = float3(0.0f);
  float3 strongest_diffuse_weight = (float3(1.0f) - F0) * (1.0f - metallic);

#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_GLSL_LIGHT_ACCESS)
  for (uint light_index = 0u; light_index < light_cull_buf.items_count; light_index++) {
    bool is_local = light_index < light_cull_buf.local_lights_len;
    if (is_local && light_index >= light_cull_buf.visible_count) {
      continue;
    }
    if (!glsl_light_loop_accept(light_index, is_local)) {
      continue;
    }
    GLSLLight lamp = glsl_light_build(light_index, is_local, light_index);
    if (!lamp.valid || lamp.lightgroup_id != lightgroup_id) {
      continue;
    }
    float3 L = principled_npr_safe_normalize(lamp.vector, N);
    float lambert_raw = dot(N, L);
    float NoL = max(lambert_raw, 0.0f);
    float visibility = principled_npr_shadow_visibility(light_index, is_local, N, shadow_mode);
    visibility = mix(1.0f, visibility, safe_shadow_strength);

    float3 diffuse_radiance = max(lamp.diffuse_color * lamp.attenuation, float3(0.0f));
    float energy = max(principled_npr_luminance(diffuse_radiance), 0.0f);
    if (energy <= 1e-8f) {
      continue;
    }
    float3 normalized_tint = clamp(diffuse_radiance / energy, float3(0.0f), float3(4.0f));
    float3 controlled_tint = mix(float3(1.0f), normalized_tint, safe_light_color_influence);

    float shadowed_lambert = lambert_raw * visibility;
    float mapping_coordinate = (coordinate_range == PRINCIPLED_NPR_RANGE_FULL) ?
                                   shadowed_lambert * 0.5f + 0.5f :
                                   shadowed_lambert;
    mapping_coordinate += principled_npr_light_energy_bias(energy, safe_intensity_influence);
    float score = max(mapping_coordinate, 0.0f) * energy;
    float mapped_coordinate = mapping_coordinate * coordinate_scale + coordinate_offset;
    float4 mapped = principled_npr_diffuse_map(mapped_coordinate,
                                               diffuse_mapping,
                                               driven_interpolation,
                                               driven_stop_count,
                                               shadow_color,
                                               lit_color,
                                               boundary,
                                               softness,
                                               diffuse_ramp,
                                               diffuse_ramp_layer,
                                               stop_color_0,
                                               stop_color_1,
                                               stop_color_2,
                                               stop_color_3,
                                               stop_color_4,
                                               stop_color_5,
                                               stop_color_6,
                                               stop_color_7,
                                               stop_positions_0_3,
                                               stop_positions_4_7);
    float3 mapped_base = principled_npr_apply_diffuse_color(
        base_color.rgb, mapped, color_application);
    float3 H = principled_npr_safe_normalize(V + L, N);
    float3 fresnel = principled_npr_fresnel(F0, dot(V, H));
    float3 diffuse_weight = (float3(1.0f) - fresnel) * (1.0f - metallic);
    float light_response = principled_npr_light_energy_response(energy);
    float3 diffuse_contribution = mapped_base * controlled_tint * diffuse_weight *
                                  light_response;
    per_light_add += diffuse_contribution;

    float3 specular_contribution = float3(0.0f);
    if (NoL > 0.0f) {
      float3 specular_radiance = max(lamp.specular_color * lamp.attenuation,
                                     float3(0.0f));
      float direct_lobe = principled_npr_ggx_direct_lobe(
          N, V, L, T, B, roughness, anisotropy);
      float lobe = principled_npr_anisotropic_lobe(
          N, V, L, T, B, anisotropy);
      float4 mapped_highlight = principled_npr_specular_map(lobe,
                                                            specular_mapping,
                                                            highlight_size,
                                                            highlight_softness,
                                                            highlight_offset,
                                                            specular_ramp,
                                                            specular_ramp_layer);
      float specular_energy = max(principled_npr_luminance(specular_radiance), 0.0f);
      float3 specular_tint = (specular_energy > 1e-8f) ?
                                 clamp(specular_radiance / specular_energy,
                                       float3(0.0f),
                                       float3(4.0f)) :
                                 float3(1.0f);
      specular_tint = mix(float3(1.0f), specular_tint, safe_highlight_light_influence);
      float3 specular_light = principled_npr_light_energy_response(specular_energy) *
                              specular_tint;
      float3 npr_color = max(highlight_color.rgb, float3(0.0f)) * mapped_highlight.rgb *
                         saturate(mapped_highlight.a);
      specular_contribution = fresnel * direct_lobe * specular_light * visibility *
                              npr_color * max(highlight_strength, 0.0f);
    }
    specular_add += specular_contribution;

    aggregate_raw_coordinate += mapping_coordinate * energy;
    aggregate_energy += energy;
    aggregate_tint += controlled_tint * energy;
    aggregate_diffuse_weight += diffuse_weight * energy;
    if (score > strongest_score) {
      strongest_score = score;
      strongest_raw_coordinate = mapping_coordinate;
      strongest_energy = energy;
      strongest_tint = controlled_tint;
      strongest_diffuse_weight = diffuse_weight;
      strongest_diffuse = diffuse_contribution;
      strongest_specular = specular_contribution;
    }
  }
#endif

  float3 direct_diffuse;
  if (mapping_stage == PRINCIPLED_NPR_MAPPING_COMBINED) {
    float mapping_coordinate = use_strongest ?
                               strongest_raw_coordinate :
                               ((aggregate_energy > 1e-8f) ?
                                    aggregate_raw_coordinate / aggregate_energy :
                                    0.0f);
    float energy = use_strongest ? strongest_energy : aggregate_energy;
    float light_response = principled_npr_light_energy_response(energy);
    float3 tint = use_strongest ? strongest_tint :
                                  ((aggregate_energy > 1e-8f) ?
                                       aggregate_tint / aggregate_energy :
                                       float3(1.0f));
    float3 diffuse_weight = use_strongest ?
                                strongest_diffuse_weight :
                                ((aggregate_energy > 1e-8f) ?
                                     aggregate_diffuse_weight / aggregate_energy :
                                     (float3(1.0f) - F0) * (1.0f - metallic));
    float4 mapped = principled_npr_diffuse_map(mapping_coordinate * coordinate_scale +
                                                   coordinate_offset,
                                               diffuse_mapping,
                                               driven_interpolation,
                                               driven_stop_count,
                                               shadow_color,
                                               lit_color,
                                               boundary,
                                               softness,
                                               diffuse_ramp,
                                               diffuse_ramp_layer,
                                               stop_color_0,
                                               stop_color_1,
                                               stop_color_2,
                                               stop_color_3,
                                               stop_color_4,
                                               stop_color_5,
                                               stop_color_6,
                                               stop_color_7,
                                               stop_positions_0_3,
                                               stop_positions_4_7);
    direct_diffuse = principled_npr_apply_diffuse_color(
                         base_color.rgb, mapped, color_application) *
                     tint * diffuse_weight * light_response;
  }
  else {
    direct_diffuse = use_strongest ? strongest_diffuse : per_light_add;
  }
  direct_diffuse *= max(direct_strength, 0.0f);
  float3 direct_specular = use_strongest ? strongest_specular : specular_add;
  direct_specular *= max(direct_strength, 0.0f);

  float aggregate_reference = (aggregate_energy > 1e-8f) ?
                                  aggregate_raw_coordinate / aggregate_energy :
                                  0.0f;
  float reference_raw = use_strongest ? strongest_raw_coordinate : aggregate_reference;
  float lighting_reference = saturate(reference_raw * coordinate_scale + coordinate_offset);

  float3 probe_reflection;
  float3 probe_irradiance;
  principled_npr_probe(
      N, V, roughness, ambient_directionality, probe_reflection, probe_irradiance);
  float3 fresnel = principled_npr_fresnel(F0, NoV);
  float safe_reflection_strength = max(reflection_strength, 0.0f);
  float3 reflection = max(probe_reflection, float3(0.0f)) * fresnel *
                      safe_reflection_strength;
  float3 ambient = max(probe_irradiance, float3(0.0f)) * base_color.rgb *
                   max(ambient_strength, 0.0f) * (1.0f - metallic);

  float rim_shape = principled_npr_rim(N,
                                       V,
                                       rim_angle,
                                       rim_length,
                                       rim_length_falloff,
                                       rim_thickness,
                                       rim_thickness_falloff);
  float light_side = lighting_reference;
  float shadow_side = 1.0f - light_side;
  float bias = clamp(rim_light_bias, -1.0f, 1.0f);
  float side_weight = (bias >= 0.0f) ? mix(1.0f, light_side, bias) :
                                      mix(1.0f, shadow_side, -bias);
  float3 rim = max(rim_color.rgb, float3(0.0f)) * rim_shape * side_weight *
               saturate(rim_mask) * max(rim_strength, 0.0f);
  float3 emission = max(emission_color.rgb, float3(0.0f)) * max(emission_strength, 0.0f);

  float3 result = max(direct_diffuse + direct_specular + reflection +
                          ambient + rim + emission,
                      float3(0.0f));
  color = float4(result, alpha);
  alpha_out = alpha;

  ClosureEmission emission_data;
  emission_data.weight = weight * alpha;
  emission_data.emission = result;
  ClosureTransparency transparency_data;
  transparency_data.weight = weight;
  transparency_data.transmittance = float3(1.0f - alpha);
  transparency_data.holdout = 0.0f;
  shader = closure_add(closure_eval(emission_data), closure_eval(transparency_data));
}
