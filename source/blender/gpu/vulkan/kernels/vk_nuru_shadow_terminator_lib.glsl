/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 * SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Geometric shadow-ray displacement, adapted from Cycles kernel/light/sample.h.
 * Separate from numerical ray bias. Never changes BSDF/NPR shading or skips an object. */
layout(scalar, binding = 8) readonly buffer TerminatorPositions
{
  vec4 terminator_positions[];
};
layout(scalar, binding = 9) readonly buffer TerminatorTransforms
{
  mat4 terminator_transforms[];
};

/* Bit zero in each TLAS instance's mask represents participation in shadow rays. */
const uint HWRT_SHADOW_CASTER_MASK = 0x01u;

float shadow_ray_after_hit(float distance)
{
  /* Advance just beyond the hit. A fixed world-space step can cross another thin surface. */
  /* Avoid a subnormal TMin on drivers that flush it to zero. */
  return uintBitsToFloat(max(floatBitsToUint(abs(distance)) + 1u, 0x00800000u));
}

float shadow_receiver_front_depth(float reverse_depth, bool is_orthographic)
{
  /* Cover rounding in the stored reverse-Z sample as well as in forward-depth unprojection.
   * A neighboring pixel's depth is not an error bound for this receiver. */
  const float depth = 1.0f - reverse_depth;
  /* Orthographic depth is linear. At a long clip range, four stored depth steps could
   * cross an otherwise resolvable close caster. Cover just one stored step there. */
  const uint reverse_steps = is_orthographic ? 1u : 4u;
  const float reverse_front = uintBitsToFloat(min(floatBitsToUint(reverse_depth) + reverse_steps,
                                                floatBitsToUint(1.0f)));
  return min(1.0f - reverse_front,
             uintBitsToFloat(max(floatBitsToUint(depth), 4u) - 4u));
}

struct TerminatorReceiver {
  vec3 offset;
  vec3 Ns;
  vec3 Ng;
  float cutoff;
};

TerminatorReceiver terminator_none()
{
  return TerminatorReceiver(vec3(0), vec3(0), vec3(0), 0.0);
}

TerminatorReceiver terminator_receiver(vec3 P, vec3 receiver_side, uint object_id, uint primitive_id)
{
  if (object_id == 0xFFFFFFFFu || object_id >= triangle_normal_ranges.length()) {
    return terminator_none();
  }
  float cutoff = material_proxies[object_id].packed_thickness.w;
  TriangleNormalRange range = triangle_normal_ranges[object_id];
  if (cutoff <= 0.0 || primitive_id >= range.count) {
    return terminator_none();
  }
  uint index = (range.offset + primitive_id) * 3u;
  mat4 object_to_world = terminator_transforms[object_id];
  mat3 linear = mat3(object_to_world);
  if (abs(determinant(linear)) < 1.0e-20) {
    return terminator_none();
  }
  mat3 inv_linear = inverse(linear);
  vec3 V[3];
  vec3 N[3];
  for (uint i = 0u; i < 3u; i++) {
    V[i] = terminator_positions[index + i].xyz;
    /* The existing caustics buffer stores normalize(object_to_world * local_normal).
     * Undo that convention before constructing the Cycles local-space surface. */
    N[i] = normalize(inv_linear * triangle_smooth_normals[index + i].xyz);
  }
  if (max(length(N[1] - N[0]), length(N[2] - N[0])) < 1.0e-6) {
    return terminator_none();
  }
  vec3 e0 = V[1] - V[0], e1 = V[2] - V[0];
  vec3 local_P = inv_linear * (P - object_to_world[3].xyz);
  vec3 d = local_P - V[0];
  float a00 = dot(e0, e0), a01 = dot(e0, e1), a11 = dot(e1, e1);
  float det = a00 * a11 - a01 * a01;
  if (!(det > 1.0e-20 * max(a00 * a11, 1.0e-20))) {
    return terminator_none();
  }
  float v = (dot(d, e0) * a11 - dot(d, e1) * a01) / det;
  float w = (dot(d, e1) * a00 - dot(d, e0) * a01) / det;
  vec3 bary = vec3(1.0 - v - w, v, w);
  if (any(lessThan(bary, vec3(-0.01))) || any(greaterThan(bary, vec3(1.01)))) {
    return terminator_none();
  }
  bary = max(bary, vec3(0));
  bary /= dot(bary, vec3(1));
  local_P = V[0] * bary.x + V[1] * bary.y + V[2] * bary.z;
  vec3 n = N[0] * bary.x + N[1] * bary.y + N[2] * bary.z;
  vec3 Ns = normalize(transpose(inv_linear) * n);
  const vec3 mesh_Ng = normalize(transpose(inv_linear) * cross(e0, e1));
  /* Reject a bent shading normal before orienting either normal for two-sided shading.
   * Flipping the geometric normal first can validate an offset into the mesh. Use the normal
   * transform for the local winding so mirrored instances take the same hemisphere test. */
  if (!(dot(Ns, mesh_Ng) >= 0.2)) {
    return terminator_none();
  }
  /* A rasterized back face still carries the mesh's original vertex normals. Reconstruct the
   * smoothing offset on the side that was actually hit, so it cannot lift a shadow ray through
   * the receiving sheet onto its lit side. Flip the corner normals together with the face. */
  const float side = dot(mesh_Ng, receiver_side) < 0.0 ? -1.0 : 1.0;
  const vec3 Ng = mesh_Ng * side;
  Ns *= side;
  n *= side;
  for (uint i = 0u; i < 3u; i++) {
    N[i] *= side;
  }
  float a = dot(N[2] - N[0], V[0] - V[2]);
  float b = dot(N[2] - N[1], V[1] - V[2]);
  float c = dot(N[1] - N[0], V[1] - V[0]);
  float u = bary.x;
  v = bary.y;
  float h = a * u * (u - 1.0) + (a + b + c) * u * v + b * v * (v - 1.0);
  float h0 = max(max(dot(V[1] - V[0], N[0]), dot(V[2] - V[0], N[0])), 0.0);
  float h1 = max(max(dot(V[0] - V[1], N[1]), dot(V[2] - V[1], N[1])), 0.0);
  float h2 = max(max(dot(V[0] - V[2], N[2]), dot(V[1] - V[2], N[2])), 0.0);
  h0 = max(dot(V[0] - local_P, N[0]) + h0, 0.0);
  h1 = max(dot(V[1] - local_P, N[1]) + h1, 0.0);
  h2 = max(dot(V[2] - local_P, N[2]) + h2, 0.0);
  h = max(min(min(h0, h1), h2), h * 0.5);
  vec3 offset = linear * (n * h);
  float limit = 0.25 *
                min(min(length(linear * e0), length(linear * e1)), length(linear * (V[2] - V[1])));
  float offset_length = length(offset);
  /* Flat, inward-facing and degenerate candidates keep the unmodified launch. */
  if (!(offset_length > 0.0) || !(dot(offset, Ng) > 0.0) || any(isnan(offset)) ||
      any(isinf(offset)))
  {
    return terminator_none();
  }
  offset *= min(1.0, limit / offset_length);
  return TerminatorReceiver(offset, Ns, Ng, cutoff);
}

/* Raster receivers lack a primitive ID. Recover only a very short segment around the
 * known depth position, along the view ray; a miss never substitutes another surface. */
TerminatorReceiver terminator_raster_receiver(vec3 P, vec3 toward_camera, float epsilon)
{
  rayQueryEXT query;
  rayQueryInitializeEXT(query,
                        scene,
                        gl_RayFlagsOpaqueEXT,
                        0xFFu,
                        P + toward_camera * epsilon,
                        0.0,
                        -toward_camera,
                        2.0 * epsilon);
  int remaining = 4096;
  while (remaining-- > 0 && rayQueryProceedEXT(query)) {
  }
  if (remaining < 0 ||
      rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionTriangleEXT)
  {
    return terminator_none();
  }
  return terminator_receiver(P,
                             toward_camera,
                             rayQueryGetIntersectionInstanceCustomIndexEXT(query, true),
                             rayQueryGetIntersectionPrimitiveIndexEXT(query, true));
}

vec3 terminator_shadow_offset(TerminatorReceiver receiver, vec3 origin, vec3 L, float epsilon)
{
  if (receiver.cutoff <= 0.0) {
    return vec3(0);
  }
  float NL = dot(receiver.Ns, L), NgL = dot(receiver.Ng, L);
  if (NL <= 0.0) {
    return vec3(0);
  }
  float amount = NL < receiver.cutoff ? clamp(2.0 - (NgL + NL) / receiver.cutoff, 0.0, 1.0) :
                                        clamp(1.0 - NgL / receiver.cutoff, 0.0, 1.0);
  vec3 offset = receiver.offset * amount;
  float distance = length(offset);
  if (!(distance > epsilon)) {
    return vec3(0);
  }
  /* Do not jump through a nearby separate surface or across a concave fold. This guard
   * is restricted to nonzero correction segments, and never masks an entire object. */
  rayQueryEXT guard;
  rayQueryInitializeEXT(guard,
                        scene,
                        gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                        0xFFu,
                        origin,
                        0.0,
                        offset / distance,
                        distance);
  int remaining = 4096;
  while (remaining-- > 0 && rayQueryProceedEXT(guard)) {
  }
  if (remaining < 0 ||
      rayQueryGetIntersectionTypeEXT(guard, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
  {
    return vec3(0);
  }
  return offset;
}
/* Source 1 carries an interpolated camera-prepass normal. Source 2 uses depth alone. */
vec3 prepass_shadow_smooth_normal(sampler2D normals, ivec2 texel, int source, vec3 fallback)
{
  if (source != 1) {
    return fallback;
  }
  const vec3 encoded = texelFetch(normals, texel, 0).xyz;
  return any(greaterThan(encoded, vec3(0.0))) ? normalize(encoded * 2.0 - 1.0) : fallback;
}
