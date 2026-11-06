/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

bool planar_probe_finite(float3 value)
{
  return all(lessThan(abs(value), float3(3.402823e+38f)));
}

[[node]]
void node_planar_probe(float3 position,
                       float3 normal,
                       float3 uv,
                       float selection,
                       float object_uid,
                       float explicit_position,
                       float explicit_normal,
                       float explicit_uv,
                       float supported,
                       out float4 color,
                       out float depth,
                       out float3 result_uv,
                       out float is_valid)
{
  color = float4(0.0f);
  depth = 0.0f;
  result_uv = float3(0.0f);
  is_valid = 0.0f;
#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_PLANAR_PROBE_ACCESS) && \
    !defined(MAT_PROBE_CAPTURE) && !defined(MAT_CAPTURE) && \
    defined(CREATE_INFO_eevee_LightprobePlaneRenderData)
  /* BSL guards the enclosing scope of resource accessors. Keep output initialization outside. */
  {
    [[resource_table]] const eevee::Uniform &probe_uniforms = resource_table_get(eevee::Uniform);
    if (supported == 0.0f || !probe_uniforms.pipeline_buf.planar_probe_ready) {
      return;
    }
    float3 P = explicit_position != 0.0f ? position : g_data.P;
    float3 N = explicit_normal != 0.0f ? normal : g_data.N;
    float normal_length_squared = dot(N, N);
    if (!planar_probe_finite(P) || !planar_probe_finite(N) ||
        !(normal_length_squared >= 1e-16f) || !(normal_length_squared < 3.402823e+38f))
    {
      return;
    }
    N = normalize(N);
    [[resource_table]] const eevee::LightprobePlaneRenderData &planes = resource_table_get(
        eevee::LightprobePlaneRenderData);
    const int index = selection == 0.0f ? planes.select_probe(P, N) :
                                          planes.select_object(P, N, floatBitsToUint(object_uid));
    if (index < 0) {
      return;
    }
    PlanarProbeData probe = planes.probe_planar_buf[index];
    for (int column = 0; column < 3; column++) {
      float4 axis = probe.world_to_object_transposed[column];
      if (!planar_probe_finite(axis.xyz) || !(abs(axis.w) < 3.402823e+38f)) {
        return;
      }
    }
    for (int column = 0; column < 4; column++) {
      if (!planar_probe_finite(probe.wininv[column].xyz) ||
          !(abs(probe.wininv[column].w) < 3.402823e+38f))
      {
        return;
      }
    }
    float object_determinant = determinant(float3x3(probe.world_to_object_transposed));
    float projection_determinant = determinant(probe.wininv);
    if (!(abs(object_determinant) > 0.0f && abs(object_determinant) < 3.402823e+38f) ||
        !(abs(projection_determinant) > 0.0f && abs(projection_determinant) < 3.402823e+38f) ||
        !planar_probe_finite(probe.normal) || !(dot(probe.normal, probe.normal) > 1e-16f) ||
        !(abs(probe.parallax_distance) < 3.402823e+38f))
    {
      return;
    }
    const int layer = probe.layer_id;
    int3 extent = textureSize(planes.planar_radiance_tx, 0);
    if (layer < 0 || layer >= extent.z || any(lessThanEqual(extent.xy, int2(0)))) {
      return;
    }
    float2 public_uv;
    if (explicit_uv != 0.0f) {
      if (!planar_probe_finite(uv)) {
        return;
      }
      public_uv = uv.xy;
    }
    else {
      const ViewMatrices view = view_matrices_get();
      float3 V = view.world_incident_vector(P);
      float3 projected_P = eevee::lightprobe::plane::parallax(probe, P, N, V);
      float4 clip = view.point_world_to_homogenous(projected_P);
      if (!planar_probe_finite(clip.xyz) || !(clip.w > 1e-8f) || !(abs(clip.w) < 3.402823e+38f)) {
        return;
      }
      public_uv = (clip.xy / clip.w) * 0.5f + 0.5f;
    }
    if (!planar_probe_finite(float3(public_uv, 0.0f))) {
      return;
    }
    result_uv = float3(public_uv, 0.0f);
    if (any(lessThan(public_uv, float2(0.0f))) || any(greaterThan(public_uv, float2(1.0f)))) {
      return;
    }

    /* The public coordinates always describe the upright image. Convert exactly once. */
    float2 texture_uv = float2(public_uv.x, 1.0f - public_uv.y);
    float2 half_texel = 0.5f / float2(extent.xy);
    float2 color_uv = clamp(texture_uv, half_texel, 1.0f - half_texel);
    float3 rgb = textureLod(planes.planar_radiance_tx, float3(color_uv, float(layer)), 0.0f).rgb;
    if (!planar_probe_finite(rgb)) {
      return;
    }
    color = float4(rgb, 1.0f);
    is_valid = 1.0f;

    int2 texel = clamp(int2(floor(texture_uv * float2(extent.xy))), int2(0), extent.xy - 1);
    float raw_depth = texelFetch(planes.planar_depth_tx, int3(texel, layer), 0).r;
    float screen_depth = reverse_z::read(raw_depth);
    if (!(screen_depth >= 0.0f && screen_depth < 1.0f)) {
      return;
    }
    float2 depth_uv = (float2(texel) + 0.5f) / float2(extent.xy);
    float4 view_P = probe.wininv * float4(float3(depth_uv, screen_depth) * 2.0f - 1.0f, 1.0f);
    if (!planar_probe_finite(view_P.xyz) || !(abs(view_P.w) > 1e-16f) ||
        !(abs(view_P.w) < 3.402823e+38f))
    {
      return;
    }
    float linear_depth = -view_P.z / view_P.w;
    if (linear_depth > 0.0f && linear_depth < 3.402823e+38f) {
      depth = linear_depth;
    }
  }
#endif
}
