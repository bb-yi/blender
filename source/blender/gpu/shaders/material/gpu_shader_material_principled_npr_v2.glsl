/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_material_glsl_light_access.glsl"
#include "gpu_shader_material_principled_shared.glsl"
#include "gpu_shader_material_shader_info_shared.glsl"
#include "gpu_shader_material_principled_npr_v2_math.glsl"
#include "gpu_shader_material_principled_npr_v2_light.glsl"
#include "gpu_shader_material_principled_npr_depth_rim.glsl"

[[node]]
void npr_v2_pack4(float x, float y, float z, float w, float4 &value)
{
  value = float4(x, y, z, w);
}

[[node]]
void npr_v2_pack_matrix(float4 a, float4 b, float4 c, float4 d, float4x4 &value)
{
  value = float4x4(a, b, c, d);
}

[[node]]
void npr_v2_pack_vector(float3 vector, float w, float4 &value)
{
  value = float4(vector, w);
}

[[node]]
void npr_v2_pack_color(float4 color, float w, float4 &value)
{
  value = float4(color.rgb, w);
}

[[node]]
void npr_v2_alpha(float alpha, float &value)
{
  value = saturate(alpha);
}

[[node]]
void npr_v2_default_tangent(float4 uv_tangent, float3 orco, float3 N, float3 &tangent)
{
  float3 generated = orco.yxz * float3(-0.5f, 0.5f, 0.0f) + float3(0.25f, -0.25f, 0.0f);
  const ObjectMatrices object = object_matrices_get();
  generated = to_float3x3(object.model) * generated;
  float3 input_tangent = dot(uv_tangent.xyz, uv_tangent.xyz) > 1e-16f ?
                            uv_tangent.xyz : generated;
  float3 bitangent;
  npr_v2_tangent_basis(N, input_tangent, 0.0f, tangent, bitangent);
}

float npr_v2_visibility(uint index,
                       bool is_local,
                       float3 N,
                       float4 policy,
                       bool &casts_shadow)
{
  casts_shadow = false;
#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_GLSL_LIGHT_SHADOW_ACCESS)
  if (int(policy.x) == 0) {
    return 1.0f;
  }
  LightData light;
  LightVector vector;
  bool directional;
  if (!glsl_light_lookup(index, is_local, light, vector, directional) ||
      light.tilemap_index == LIGHT_NO_SHADOW)
  {
    return 1.0f;
  }
  casts_shadow = true;
  ObjectInfos object = object_infos_get();
  float3 Ng = glsl_light_resolve_normal(g_data.Ng);
#  if defined(SHADOW_CASTER_CLASSIFY)
  if (int(policy.x) == 1 || int(policy.x) == 3) {
    ShaderInfoShadowClassification classified = shader_info_shadow_classified_visibility(
        light, directional, vector.L, g_data.P, Ng, N,
        object.shadow_terminator_normal_offset, object.shadow_terminator_geometry_offset,
        policy.y, policy.z);
    return 1.0f - saturate(int(policy.x) == 1 ? classified.cast_shadow :
                                               classified.self_shadow);
  }
#  endif
  return shader_info_shadow_visibility(
      light, directional, vector.L, g_data.P, Ng, N,
      object.shadow_terminator_normal_offset, object.shadow_terminator_geometry_offset,
      policy.y, policy.z);
#else
  return 1.0f;
#endif
}

float npr_v2_rim_shape(float3 N, float3 V, float4 shape, float falloff)
{
  float3 camera_x = float3(1.0f, 0.0f, 0.0f);
  float3 camera_y = float3(0.0f, 1.0f, 0.0f);
#if defined(GPU_FRAGMENT_SHADER)
  camera_x = npr_v2_safe_normalize(drw_view().viewinv[0].xyz, camera_x);
  camera_y = npr_v2_safe_normalize(drw_view().viewinv[1].xyz, camera_y);
#endif
  float2 projected = float2(dot(N, camera_x), dot(N, camera_y));
  float len = length(projected);
  if (len <= 1e-6f || shape.y <= 1e-6f || shape.w <= 1e-6f) {
    return 0.0f;
  }
  projected /= len;
  float turns = atan(projected.y, projected.x) / (2.0f * M_PI);
  float centered = abs(fract(turns - shape.x / 360.0f + 0.5f) - 0.5f);
  float half_length = saturate(shape.y) * 0.5f;
  float arc_width = max(shape.z, 0.0f) * 0.5f;
  float arc = arc_width <= 1e-6f ? 1.0f - step(half_length, centered) :
      1.0f - smoothstep(max(half_length - arc_width, 0.0f), half_length, centered);
  float facing = saturate(1.0f - dot(N, V));
  float edge = 1.0f - saturate(shape.w);
  float width = max(falloff, 0.0f);
  return arc * (width <= 1e-6f ? step(edge, facing) :
                 smoothstep(edge - 0.5f * width, edge + 0.5f * width, facing));
}

float3 npr_v2_highlight_filtered(float3 response, float3 reference, float softness, float beta)
{
#if defined(MAT_NPR_REFERENCE_HIGHLIGHT)
  float3 result = npr_v2_highlight_rgb(response, reference, softness, beta);
#if defined(GPU_FRAGMENT_SHADER)
  if (softness <= 0.0f) {
    /* Pixel coverage, not an artificial minimum artistic softness. Interior pixels keep the
     * constant reference color, and the footprint shrinks with increasing image resolution. */
    float signed_distance = npr_v2_luminance(response) -
                            beta * npr_v2_luminance(reference);
    float coverage = npr_v2_hard_edge_coverage(signed_distance, fwidth(signed_distance));
    result = reference * coverage;
  }
#endif
  return result;
#else
  return response;
#endif
}

float3 npr_v2_reflection_transport_gain(float3 base,
                                       float metallic,
                                       float transmission,
                                       bool thin_wall,
                                       float ior,
                                       float roughness,
                                       float NV)
{
  /* Match the native hemispherical energy budget before reshaping the direct lobe.
   * This includes multi-scattering and the thin slab's internal reflections; multiplying
   * it after reshaping would reintroduce a view-dependent gradient into the plateau. */
  float3 F0 = float3(F0_from_ior(ior));
  float3 dielectric_single, dielectric_multi, unused;
  bsdf_lut(F0, float3(1.0f), float3(0.0f), NV, roughness, ior, false,
           dielectric_single, unused);
  bsdf_lut(F0, float3(1.0f), float3(0.0f), NV, roughness, ior, true,
           dielectric_multi, unused);
  float3 metal_single, metal_multi;
  brdf_f82_tint_lut(base, float3(1.0f), NV, roughness, false, metal_single);
  brdf_f82_tint_lut(base, float3(1.0f), NV, roughness, true, metal_multi);
  float3 transmission_reflection = dielectric_multi;
  if (transmission > 0.0f && metallic < 1.0f) {
    float3 transmittance;
    bsdf_lut(F0, float3(1.0f), thin_wall ? float3(1.0f) : sqrt(base),
             NV, roughness, ior, true, transmission_reflection, transmittance);
    if (thin_wall) {
      float3 tint = slab_transmittance_at_angle(base, NV, ior);
      transmittance = safe_divide(tint * square(transmittance),
                                  float3(1.0f) - square(transmission_reflection * tint));
      transmission_reflection *= float3(1.0f) + transmittance * tint;
    }
  }
  float3 single = mix(dielectric_single, metal_single, metallic);
  float3 layered = mix(mix(dielectric_multi, transmission_reflection, transmission),
                        metal_multi, metallic);
  return safe_divide(layered, single);
}

/* Matrices are internal argument packets, not user-visible matrix sockets. This keeps the
 * material-library entry point below MAX_PARAMETER without baking the 32 point ramp. */
[[node]]
void node_principled_npr_v2(float4 base_color,
                          float3 N,
                          float3 tangent,
                          float3 coat_normal,
                          float4x4 material,
                          float4x4 surface,
                          float4x4 shading,
                          float4x4 highlight,
                          float4x4 rim,
                          float4 coat_color,
                          float4 sheen_color,
                          float4x4 colors0, float4x4 colors1, float4x4 colors2, float4x4 colors3,
                          float4x4 colors4, float4x4 colors5, float4x4 colors6, float4x4 colors7,
                          float4x4 points0, float4x4 points1, float4x4 points2, float4x4 points3,
                          float4x4 points4, float4x4 points5, float4x4 points6, float4x4 points7,
                          sampler1DArray tables,
                          sampler1DArray specular_table,
                          float4 table_info,
                          float4 execution,
                          Closure &shader,
                          float4 &local_color,
                          float &alpha_out)
{
  shader = Closure(0);
  const bool emit_shader = execution.y > 0.5f;
  float metallic = saturate(material[0].x);
  float roughness = saturate(material[0].y);
  float alpha = saturate(material[0].z);
  float ior = max(material[0].w, 1.0f);
  float transmission = saturate(material[1].x);
  float subsurface = saturate(material[1].y);
  bool thin_wall = material[1].w > 0.5f;
  float body_preservation = saturate(material[2].w);
  float indirect_diffuse = max(material[3].x, 0.0f);
  float indirect_specular = max(material[3].y, 0.0f);
  ClosureNPRLightPolicy light_policy = closure_npr_light_policy(
      shading[3], shading[1].w, max(shading[1].x, 0.0f));
  float3 base = clamp(base_color.rgb, float3(0.0f), float3(1.0f));
  N = npr_v2_safe_normalize(N, npr_v2_safe_normalize(g_data.N, g_data.Ng));
  float3 V = npr_v2_safe_normalize(coordinate_incoming(g_data.P), N);
  float NV = max(dot(N, V), 1e-6f);
  float3 CN = npr_v2_safe_normalize(coat_normal, N);
  float3 T;
  float3 B;
  npr_v2_tangent_basis(N, tangent, highlight[2].y, T, B);
  float weight = execution.x * alpha;
  float layer = 1.0f;
  float3 coat_tint = float3(1.0f);
  float3 sheen_tint = float3(0.0f);

#ifdef MAT_BAKE_COLOR
  if (emit_shader) {
    /* Match native albedo baking: do not accumulate coat/metal/SSS lobe weights
     * or artistic direct radiance as a replacement for the material's Base Color. */
    bake_color_principled_begin(base, execution.x);
  }
#endif

  /* Reuse the native layer order and Fresnel/transport coefficients. Optional layers are
   * submitted only when Shader is consumed; Local Color remains a pure data evaluation. */
  if (surface[0].w > 0.0f) {
    float sheen_NV = saturate(dot(npr_v2_safe_normalize(mix(N, CN, saturate(surface[0].x)), N), V));
    sheen_tint = max(surface[0].w, 0.0f) * max(sheen_color.rgb, float3(0.0f)) *
                 principled_sheen(sheen_NV, saturate(surface[1].x));
    layer *= max(1.0f - max(sheen_tint.x, max(sheen_tint.y, sheen_tint.z)), 0.0f);
  }
  if (surface[0].x > 0.0f) {
    float coat_weight = saturate(surface[0].x);
    float coat_r = saturate(surface[0].y);
    float coat_ior = max(surface[0].z, 1.0f);
    float coat_NV = max(dot(CN, V), 1e-6f);
    float coat_reflectance = bsdf_lut(coat_NV, coat_r, coat_ior, false).x;
    if (emit_shader) {
      ClosureReflection coat;
      coat.N = CN;
      coat.roughness = coat_r;
      coat.color = float3(coat_reflectance);
      coat.weight = weight * layer * coat_weight;
      closure_eval_npr_native(coat, indirect_specular, light_policy);
    }
    layer *= max(1.0f - coat_reflectance * coat_weight, 0.0f);
    coat_tint = mix(float3(1.0f),
                    slab_transmittance_at_angle(clamp(coat_color.rgb, float3(0.0f), float3(1.0f)),
                                               NV, coat_ior),
                    coat_weight);
  }
  float3 emission = max(surface[2].rgb, float3(0.0f)) *
                    max(surface[1].y, 0.0f) * layer * coat_tint;

  float Fd = F0_from_ior(ior);
  float3 dielectric_reflectance;
  float3 unused_transmittance;
  bsdf_lut(float3(Fd), float3(1.0f), float3(0.0f), NV, roughness, ior, true,
           dielectric_reflectance, unused_transmittance);
  float3 metal_reflectance;
  brdf_f82_tint_lut(base, float3(1.0f), NV, roughness, true, metal_reflectance);
  float3 transmission_reflectance = dielectric_reflectance;
  float3 transmittance = float3(0.0f);
  if (transmission > 0.0f && metallic < 1.0f) {
    if (thin_wall) {
      bsdf_lut(float3(Fd), float3(1.0f), float3(1.0f), NV, roughness, ior, true,
               transmission_reflectance, transmittance);
      float3 tint = slab_transmittance_at_angle(base, NV, ior);
      transmittance = safe_divide(tint * square(transmittance),
                                  float3(1.0f) - square(transmission_reflectance * tint));
      transmission_reflectance *= float3(1.0f) + transmittance * tint;
    }
    else {
      bsdf_lut(float3(Fd), float3(1.0f), sqrt(base), NV, roughness, ior, true,
               transmission_reflectance, transmittance);
    }
    if (emit_shader) {
      if (thin_wall) {
        ClosureThinRefraction refract;
        refract.N = N;
        refract.roughness = thin_glass_transmission_roughness(roughness, ior);
        refract.color = transmittance * coat_tint;
        refract.weight = weight * layer * (1.0f - metallic) * transmission;
        closure_eval_npr_native(refract, light_policy);
      }
      else {
        ClosureRefraction refract;
        refract.N = N;
        refract.roughness = roughness;
        refract.ior = ior;
        refract.color = transmittance * coat_tint;
        refract.weight = weight * layer * (1.0f - metallic) * transmission;
        closure_eval_npr_native(refract, light_policy);
      }
    }
  }

  float4 colors[NPR_V2_MAX_POINTS];
  float4 points[NPR_V2_MAX_POINTS];
  for (int i = 0; i < 4; i++) {
    colors[i] = colors0[i]; colors[i+4] = colors1[i];
    colors[i+8] = colors2[i]; colors[i+12] = colors3[i];
    colors[i+16] = colors4[i]; colors[i+20] = colors5[i];
    colors[i+24] = colors6[i]; colors[i+28] = colors7[i];
    points[i] = points0[i]; points[i+4] = points1[i];
    points[i+8] = points2[i]; points[i+12] = points3[i];
    points[i+16] = points4[i]; points[i+20] = points5[i];
    points[i+24] = points6[i]; points[i+28] = points7[i];
  }
  int point_count = clamp(int(table_info.z), 2, NPR_V2_MAX_POINTS);
  if (execution.z < 0.5f) {
    /* Only connected point positions need GPU sorting. CPU/RNA ramp points are
     * uploaded in evaluation order without changing their animation identities. */
    npr_v2_sort_points(colors, points, point_count);
  }
  float4 map_parameters = float4(shading[0].xyz, table_info.w);
  bool full_range = int(shading[2].x) == 1;
  bool strongest = int(shading[2].w) == 1;
  bool combined = int(shading[2].z) == 1;
  bool energy_response = shading[0].w >= 1.0f;
  float global_strength = max(shading[1].x, 0.0f);
  float shadow_strength = saturate(shading[1].w);
  float softness = saturate(highlight[1].y);
  float size_scale = exp2(4.0f * clamp(highlight[1].z, -1.0f, 1.0f));
  float beta = 0.0f;
#ifdef MAT_NPR_REFERENCE_HIGHLIGHT
  beta = npr_v2_sample_beta(tables, table_info.x, softness);
#endif
  float4 spec_shape = float4(roughness, highlight[2].x, highlight[2].y, size_scale);
  float3 direct_reflection_gain = npr_v2_reflection_transport_gain(
      base, metallic, transmission, thin_wall, ior, roughness, NV);
  float3 reference_reflection_gain = float3(0.0f);
#ifdef MAT_NPR_REFERENCE_HIGHLIGHT
  reference_reflection_gain = npr_v2_reflection_transport_gain(
      base, metallic, transmission, thin_wall, ior, roughness, 1.0f);
#endif
  float3 diffuse_mul = float3(0.0f);
  float3 diffuse_add = float3(0.0f);
  float3 direct_specular = float3(0.0f);
  float strongest_diffuse_score = -1.0f;
  float strongest_specular_score = -1.0f;
  float strongest_rim_score = -1.0f;
  float aggregate_coordinate = 0.0f;
  float aggregate_energy = 0.0f;
  float3 aggregate_light = float3(0.0f);
  float winner_coordinate = 0.0f;
  float rim_winner_coordinate = 0.0f;
  float3 winner_light = float3(0.0f);
  float lighting_reference = 0.0f;

#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_GLSL_LIGHT_ACCESS)
  float shared_available_energy = 0.0f;
  bool share_energy = energy_response && combined && !strongest && shading[1].y > 0.0f;
  if (share_energy) {
    /* Combined shares light energy, not one average shadow or one mapped color.
     * A fully blocked lamp cannot push the other lamps' bands. Keep this extra
     * shadow pass out of the default Per Light path and the k=0 endpoint. */
    for (uint light_index = 0u; light_index < light_cull_buf.items_count; light_index++) {
      bool is_local = light_index < light_cull_buf.local_lights_len;
      if ((is_local && light_index >= light_cull_buf.visible_count) ||
          !glsl_light_loop_accept(light_index, is_local))
      {
        continue;
      }
      GLSLLight lamp = glsl_light_build(light_index, is_local, light_index);
      if (!lamp.valid || (shading[3].w >= 0.0f && lamp.lightgroup_id != int(shading[3].w))) {
        continue;
      }
      /* Keep operation order identical to the main pass, also for colored lights. */
      float3 diffuse_light = max(lamp.diffuse_color * lamp.attenuation, float3(0.0f)) *
                             float(M_1_PI);
      float energy = max(npr_v2_luminance(diffuse_light), 0.0f);
      if (energy <= 0.0f) {
        continue;
      }
      float visible_fraction = 1.0f;
      if (shadow_strength > 0.0f && int(shading[3].x) != 0) {
        bool casts_shadow;
        float visibility = npr_v2_visibility(light_index, is_local, N, shading[3], casts_shadow);
        if (casts_shadow) {
          visible_fraction = mix(1.0f, visibility, shadow_strength);
        }
      }
      shared_available_energy += energy * visible_fraction;
    }
  }
  for (uint light_index = 0u; light_index < light_cull_buf.items_count; light_index++) {
    bool is_local = light_index < light_cull_buf.local_lights_len;
    if ((is_local && light_index >= light_cull_buf.visible_count) ||
        !glsl_light_loop_accept(light_index, is_local))
    {
      continue;
    }
    GLSLLight lamp = glsl_light_build(light_index, is_local, light_index);
    if (!lamp.valid ||
        (shading[3].w >= 0.0f && lamp.lightgroup_id != int(shading[3].w)))
    {
      continue;
    }
    float3 L = npr_v2_safe_normalize(lamp.vector, N);
    bool casts_shadow;
    float visibility = shadow_strength <= 0.0f ? 1.0f :
        npr_v2_visibility(light_index, is_local, N, shading[3], casts_shadow);
    if (shadow_strength <= 0.0f) {
      casts_shadow = false;
    }
    float3 diffuse_light = max(lamp.diffuse_color * lamp.attenuation, float3(0.0f)) *
                           float(M_1_PI);
    float energy = max(npr_v2_luminance(diffuse_light), 0.0f);
    diffuse_light = mix(float3(energy), diffuse_light, saturate(shading[1].z));
    float NoL = dot(N, L);
    /* Preserve saved V2 nodes, including their animated/linked legacy energy bias. */
    float coordinate_bias = energy_response ? 0.0f :
        log2(1.0f + energy) * 0.03125f * saturate(shading[1].y);
    float raw = NoL + coordinate_bias * (full_range ? 2.0f : 1.0f);
    float coordinate = full_range ? raw * 0.5f + 0.5f : raw;
    if (casts_shadow && int(shading[3].x) != 0) {
      coordinate = mix(coordinate, min(coordinate, visibility), shadow_strength);
    }
    /* The new control shapes diffuse bands only, not the independent Rim light bias. */
    float lighting_coordinate = coordinate;
    if (energy_response) {
      float visible_fraction = casts_shadow && int(shading[3].x) != 0 ?
                                   mix(1.0f, visibility, shadow_strength) : 1.0f;
      float drive_energy = share_energy ?
          npr_v2_shared_energy(energy, visible_fraction, shared_available_energy) : energy;
      coordinate = npr_v2_energy_coordinate(coordinate, drive_energy, shading[1].y);
    }
    if (energy > 0.0f) {
      float4 mapped = npr_v2_map_sorted(coordinate, colors, points, point_count, map_parameters);
      float3 mul;
      float3 add;
      npr_v2_color_decompose(mapped, int(shading[2].y) == 1, mul, add);
      float3 current_mul = mul * diffuse_light;
      float3 current_add = add * diffuse_light;
      float score = max(coordinate, 0.0f) * energy;
      float rim_score = max(lighting_coordinate, 0.0f) * energy;
      if (rim_score > strongest_rim_score) {
        strongest_rim_score = rim_score;
        rim_winner_coordinate = lighting_coordinate;
      }
      aggregate_coordinate += lighting_coordinate * energy;
      aggregate_energy += energy;
      aggregate_light += diffuse_light;
      if (!strongest) {
        diffuse_mul += current_mul;
        diffuse_add += current_add;
      }
      if (score > strongest_diffuse_score) {
        strongest_diffuse_score = score;
        winner_coordinate = lighting_coordinate;
        winner_light = diffuse_light;
        if (strongest) {
          diffuse_mul = current_mul;
          diffuse_add = current_add;
        }
      }
    }

    /* A specular-only lamp must not be rejected because its diffuse power is zero. */
    if (highlight[1].x > 0.0f) {
      float3 raw_response;
      float3 reference_response;
      /* Change the incident color before evaluation. Dividing the colored result cannot
       * recover a channel that a saturated light color made exactly zero. */
      GLSLLight specular_lamp = lamp;
      float3 lamp_color = max(lamp.specular_color, float3(0.0f));
      specular_lamp.specular_color = mix(float3(npr_v2_luminance(lamp_color)), lamp_color,
                                         saturate(highlight[1].w));
      npr_v2_light_response(light_index, is_local, specular_lamp, N, V, tangent, spec_shape,
                            base, metallic, ior,
                            int(highlight[2].w) == 0,
                            softness < 1.0f || size_scale != 1.0f || int(highlight[2].z) == 1,
                            raw_response, reference_response);
      raw_response *= direct_reflection_gain;
      reference_response *= reference_reflection_gain;
      /* The emitter can cross the tangent plane even when its center is below it. Its
       * individual sampled directions, not the center NoL, decide the GGX contribution. */
      float3 profile = npr_v2_highlight_filtered(raw_response, reference_response, softness, beta);
      if (int(highlight[2].z) == 1) {
        float reference_luminance = npr_v2_luminance(reference_response);
        float ratio = reference_luminance > 0.0f ?
                          npr_v2_luminance(profile) / reference_luminance : 0.0f;
        float4 tint = texture(specular_table, float2(saturate(ratio), table_info.y));
        profile *= max(tint.rgb, float3(0.0f)) * saturate(tint.a);
      }
      profile *= max(highlight[0].rgb, float3(0.0f)) * max(highlight[1].x, 0.0f) *
                 mix(1.0f, visibility, shadow_strength);
      float score = npr_v2_luminance(max(raw_response, float3(0.0f))) *
                    mix(1.0f, visibility, shadow_strength);
      if (!strongest) {
        direct_specular += profile;
      }
      else if (score > strongest_specular_score) {
        strongest_specular_score = score;
        direct_specular = profile;
      }
    }
  }
#endif

  if (combined && !energy_response && aggregate_energy > 0.0f) {
    float coordinate = strongest ? winner_coordinate : aggregate_coordinate / aggregate_energy;
    float4 mapped = npr_v2_map_sorted(coordinate, colors, points, point_count, map_parameters);
    npr_v2_color_decompose(mapped, int(shading[2].y) == 1, diffuse_mul, diffuse_add);
    float3 light = strongest ? winner_light : aggregate_light;
    diffuse_mul *= light;
    diffuse_add *= light;
  }
  diffuse_mul *= global_strength;
  diffuse_add *= global_strength;
  direct_specular *= global_strength;
  if (aggregate_energy > 0.0f) {
    lighting_reference = saturate(
        (strongest ? rim_winner_coordinate : aggregate_coordinate / aggregate_energy) *
        shading[0].x + shading[0].y);
  }

  float diffuse_remaining = max(1.0f - max(dielectric_reflectance.x,
      max(dielectric_reflectance.y, dielectric_reflectance.z)), 0.0f);
  float diffuse_weight = layer * (1.0f - metallic) * (1.0f - transmission) * diffuse_remaining;
  float3 reflection_color = metallic * metal_reflectance +
      (1.0f - metallic) * mix(dielectric_reflectance, transmission_reflectance, transmission);
  float rim_shape = 0.0f;
  if (rim[0].w > 0.0f && rim[2].z > 0.0f) {
    if (int(rim[2].w) == 1) {
      /* Fractional stochastic coverage is not a solid silhouette. Never
       * substitute unrelated opaque depth for a Blended surface. */
      if (rim[3].w > 0.5f && alpha >= 1.0f) {
        rim_shape = npr_v2_depth_rim(rim[3].x, rim[3].z, rim[3].y,
                                     rim[1].x, rim[1].y, rim[1].z);
      }
    }
    else {
      rim_shape = npr_v2_rim_shape(N, V, rim[1], rim[2].x);
    }
  }
  float3 rim_radiance = max(rim[0].rgb, float3(0.0f)) * max(rim[0].w, 0.0f) *
                       rim_shape * saturate(rim[2].z);
  float light_bias = clamp(rim[2].y, -1.0f, 1.0f);
  rim_radiance *= light_bias >= 0.0f ? mix(1.0f, lighting_reference, light_bias) :
                                      mix(1.0f, 1.0f - lighting_reference, -light_bias);
  rim_radiance *= layer * coat_tint;
  float body_weight = layer * metallic * body_preservation;
  float3 local_diffuse = (base * diffuse_mul + diffuse_add) * coat_tint *
                        (diffuse_weight + body_weight);
  local_color = float4(max(local_diffuse + direct_specular * layer * coat_tint +
                           rim_radiance + emission, float3(0.0f)), alpha);
  alpha_out = alpha;
  if (!emit_shader) {
    return;
  }

  ClosureEmission emissive;
  emissive.emission = emission;
  emissive.weight = execution.x * alpha;
  closure_eval(emissive);
  ClosureTransparency transparent;
  transparent.transmittance = float3(1.0f - alpha);
  transparent.weight = execution.x;
  transparent.holdout = 0.0f;
  closure_eval(transparent);
  closure_npr_rim_add(rim_radiance, weight);

  /* Transport color remains physical. Artificial metal body is direct-only. */
  if (body_weight > 0.0f) {
    ClosureDiffuse body;
    body.N = N;
    body.color = float3(0.0f);
    body.weight = weight * body_weight;
    closure_eval_npr(body, float3(0.0f),
                     (base * diffuse_mul + diffuse_add) * coat_tint, 0.0f, light_policy);
  }
  if (diffuse_weight > 0.0f || max(sheen_tint.x, max(sheen_tint.y, sheen_tint.z)) > 0.0f) {
    float3 physical_color = base * coat_tint;
    /* The transport gain is separate from albedo and direct radiance. */
    ClosureDiffuse diffuse;
    diffuse.N = N;
    diffuse.color = physical_color;
    diffuse.weight = weight * diffuse_weight * (1.0f - subsurface);
    closure_eval_npr(diffuse, diffuse_mul, diffuse_add * coat_tint, indirect_diffuse, light_policy);
    if (subsurface > 0.0f) {
      if (thin_wall) {
        diffuse.weight = weight * diffuse_weight * subsurface * 0.5f;
        closure_eval_npr(diffuse, diffuse_mul, diffuse_add * coat_tint, indirect_diffuse, light_policy);
        ClosureTranslucent transmitted;
        transmitted.N = N;
        transmitted.color = physical_color;
        transmitted.weight = weight * diffuse_weight * subsurface * 0.5f;
        closure_eval_npr_native(transmitted, indirect_diffuse, light_policy);
      }
      else {
        ClosureSubsurface scattered;
        scattered.N = N;
        scattered.color = physical_color;
        scattered.weight = weight * diffuse_weight * subsurface;
        /* Match the default native Random Walk radius units. EEVEE's diffusion kernel
         * retains the legacy 1/(4*pi) conversion internally. */
        scattered.sss_radius = max(material[2].xyz, float3(0.0f)) *
                               max(material[1].z, 0.0f) * (4.0f * float(M_PI));
        closure_eval_npr(scattered, diffuse_mul, diffuse_add * coat_tint, indirect_diffuse, light_policy);
      }
    }
    if (max(sheen_tint.x, max(sheen_tint.y, sheen_tint.z)) > 0.0f) {
      ClosureDiffuse sheen;
      sheen.N = N;
      sheen.color = sheen_tint;
      sheen.weight = weight;
      closure_eval_npr_native(sheen, indirect_diffuse, light_policy);
    }
  }
  ClosureReflection reflection;
  reflection.N = N;
  reflection.roughness = roughness;
  reflection.color = reflection_color * coat_tint;
  reflection.weight = weight * layer;
  closure_eval_npr(reflection, float3(0.0f), direct_specular * coat_tint,
                   saturate(highlight[2].x), T, indirect_specular, light_policy);
#ifdef MAT_BAKE_COLOR
  bake_color_principled_end();
#endif
}
