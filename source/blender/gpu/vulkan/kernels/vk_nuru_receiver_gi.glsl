/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_scalar_block_layout : require

/* One diffuse bounce at the surface reached by a specular path. The result is E / PI,
 * demodulated ONLY with respect to this receiver. Newly sampled surfaces use the scene's
 * physical diffuse/emission proxies; they do not evaluate an NPR or arbitrary node graph.
 * No screen radiance reuse, recursive specular transport, or unoccluded ambient fill. */
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct TriangleNormalRange {
  uint offset;
  uint count;
};
struct FastGILightRecord {
  vec4 object_to_world_x;
  vec4 object_to_world_y;
  vec4 object_to_world_z;
  vec4 color_diffuse_power;
  vec4 direction_type;
  vec4 attenuation_spot;
  vec4 spot_size_inv;
};
layout(scalar, binding = 0) uniform ReceiverGIUniforms {
  ivec4 samples_lights_environment_seed;
  vec4 world_probe_atlas_coord;
  vec4 sampling_rand;
} uniforms;
layout(binding = 1) uniform accelerationStructureEXT scene;
layout(scalar, binding = 2) readonly buffer B2 { vec4 emissive_radiance[]; };
layout(scalar, binding = 3) readonly buffer B3 { vec4 diffuse_albedo[]; };
layout(scalar, binding = 4) readonly buffer B4 { vec4 triangle_normals[]; };
layout(scalar, binding = 5) readonly buffer B5 { TriangleNormalRange triangle_normal_ranges[]; };
layout(scalar, binding = 6) readonly buffer B6 { uint tiles_coord_buf[]; };
layout(scalar, binding = 7) readonly buffer B7 { FastGILightRecord lights[]; };
layout(binding = 16) uniform sampler2D receiver_position_tx;
layout(binding = 17) uniform sampler2D receiver_normal_tx;
layout(binding = 18) uniform usampler2D receiver_identity_tx;
layout(binding = 19) uniform sampler2DArray world_probe_tx;
layout(rgba16f, binding = 40) uniform writeonly image2D receiver_gi_img;

uvec4 pcg4d(uvec4 v)
{
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.w;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v.w += v.y * v.z;
  v ^= v >> 16u;
  v.x += v.y * v.w;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v.w += v.y * v.z;
  return v;
}

vec4 random_sample(uvec2 pixel, uint sample_index)
{
  uvec4 seed = uvec4(pixel, sample_index, uint(uniforms.samples_lights_environment_seed.w));
  seed ^= floatBitsToUint(uniforms.sampling_rand);
  return vec4(pcg4d(seed) >> 8u) * (1.0 / 16777216.0);
}

float ray_epsilon(vec3 P, float distance_hint)
{
  float scale = max(max(abs(P.x), max(abs(P.y), abs(P.z))), distance_hint);
  return max(scale * 1.25e-6, 1.0e-8);
}

struct Hit {
  bool valid;
  float distance;
  uint instance;
  uint primitive;
};

Hit trace_ray(vec3 P, vec3 L, float max_distance)
{
  Hit hit = Hit(false, 0.0, 0u, 0u);
  if (any(isnan(P)) || any(isinf(P)) || any(isnan(L)) || any(isinf(L)) ||
      !(dot(L, L) > 1.0e-10) || !(max_distance > 0.0))
  {
    return hit;
  }
  rayQueryEXT query;
  rayQueryInitializeEXT(query, scene, gl_RayFlagsOpaqueEXT, 0xFFu, P, 0.0, L, max_distance);
  int guard = 4096;
  while (rayQueryProceedEXT(query) && guard-- > 0) {
  }
  if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
    hit.valid = true;
    hit.distance = rayQueryGetIntersectionTEXT(query, true);
    hit.instance = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
    hit.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
  }
  return hit;
}

vec3 geometric_normal(uint instance, uint primitive, vec3 fallback)
{
  if (instance >= triangle_normal_ranges.length()) {
    return fallback;
  }
  TriangleNormalRange range = triangle_normal_ranges[instance];
  if (primitive >= range.count) {
    return fallback;
  }
  vec3 N = triangle_normals[range.offset + primitive].xyz;
  if (!(dot(N, N) > 1.0e-10)) {
    return fallback;
  }
  N = normalize(N);
  return dot(N, fallback) < 0.0 ? -N : N;
}

vec3 world_radiance(vec3 direction)
{
  vec4 atlas = uniforms.world_probe_atlas_coord;
  if (uniforms.samples_lights_environment_seed.z == 0 || atlas.z <= 0.0 || atlas.w < 0.0) {
    return vec3(0.0);
  }
  vec3 co = direction / dot(vec3(1.0), abs(direction));
  if (co.z < 0.0) {
    co.xy = (1.0 - abs(co.yx)) * vec2(co.x >= 0.0 ? 1.0 : -1.0,
                                     co.y >= 0.0 ? 1.0 : -1.0);
  }
  vec2 uv = co.xy * 0.5 + 0.5;
  float resolution = max(atlas.z * 4096.0, 1.0);
  uv = (uv * ((resolution - 2.0) / resolution) + 0.5 / resolution) * atlas.z + atlas.xy;
  return max(textureLod(world_probe_tx, vec3(uv, atlas.w), 0.0).rgb, vec3(0.0));
}

/* Analytic small-source Lambert integral, matching the existing physical light proxy.
 * Sample one light uniformly per diffuse ray; account for its selection probability.
 * Local/area-light visibility is center sampled (no extra soft-shadow rays at this bounce). */
vec3 direct_radiance(vec3 P, vec3 N, float epsilon, float random_light)
{
  int count = uniforms.samples_lights_environment_seed.y;
  if (count == 0) {
    return vec3(0.0);
  }
  FastGILightRecord light = lights[min(int(random_light * float(count)), count - 1)];
  uint type = uint(light.direction_type.w + 0.5);
  bool sun = type <= 1u;
  vec3 L;
  float distance_to_light = 100000.0;
  float power, attenuation = 1.0;
  if (sun) {
    L = normalize(light.direction_type.xyz);
    float radius_squared = light.attenuation_spot.x * light.attenuation_spot.x;
    power = radius_squared / (1.0 + radius_squared);
  }
  else {
    vec3 light_position = vec3(light.object_to_world_x.w,
                               light.object_to_world_y.w,
                               light.object_to_world_z.w);
    L = light_position - P;
    distance_to_light = length(L);
    if (distance_to_light <= epsilon) {
      return vec3(0.0);
    }
    L /= distance_to_light;
    float factor = distance_to_light * distance_to_light * light.attenuation_spot.y;
    attenuation = pow(clamp(1.0 - factor * factor, 0.0, 1.0), 2.0);
    float radius = max(light.attenuation_spot.x, 1.0e-4);
    if (type == 10u || type == 12u) {
      radius *= inversesqrt(max(1.0e-8, 1.0 - radius * radius /
                                (distance_to_light * distance_to_light)));
    }
    float x = clamp(radius / distance_to_light, 0.0, 1.0);
    /* Stable form of 2*(1-sqrt(1-x*x)) for small distant lights. */
    power = 2.0 * x * x / (1.0 + sqrt(max(1.0 - x * x, 0.0)));
    if (type >= 20u) {
      vec3 axis = vec3(light.object_to_world_x.z, light.object_to_world_y.z,
                       light.object_to_world_z.z);
      power *= max(dot(axis, L), 0.0);
    }
    if (type == 12u || type == 13u) {
      vec3 local_L = mat3(light.object_to_world_x.xyz,
                          light.object_to_world_y.xyz,
                          light.object_to_world_z.xyz) * L;
      if (local_L.z <= 0.0) {
        return vec3(0.0);
      }
      vec2 scaled = local_L.xy * light.spot_size_inv.xy / local_L.z;
      float ellipse = inversesqrt(1.0 + dot(scaled, scaled));
      attenuation *= smoothstep(0.0, 1.0,
                                 ellipse * light.attenuation_spot.z + light.attenuation_spot.w);
    }
  }
  float facing = max(dot(N, L), 0.0);
  if (facing <= 0.0 || attenuation <= 0.0 || power <= 0.0) {
    return vec3(0.0);
  }
  if (trace_ray(P + N * epsilon, L, distance_to_light - epsilon).valid) {
    return vec3(0.0);
  }
  return max(light.color_diffuse_power.rgb, vec3(0.0)) *
         (light.color_diffuse_power.w * attenuation * power * facing * float(count));
}

void main()
{
  uint tile = tiles_coord_buf[gl_WorkGroupID.x];
  uvec2 pixel = uvec2(tile & 0xFFFFu, tile >> 16u) * 8u + gl_LocalInvocationID.xy;
  ivec2 texel = ivec2(pixel);
  if (any(greaterThanEqual(texel, imageSize(receiver_gi_img)))) {
    return;
  }
  vec4 receiver = texelFetch(receiver_position_tx, texel, 0);
  vec3 N = texelFetch(receiver_normal_tx, texel, 0).xyz;
  if (receiver.w <= 0.0 || !(dot(N, N) > 1.0e-10)) {
    imageStore(receiver_gi_img, texel, vec4(0.0));
    return;
  }
  N = normalize(N);
  uvec4 identity = texelFetch(receiver_identity_tx, texel, 0);
  vec3 Ng = geometric_normal(identity.x, identity.y, N);
  float epsilon = ray_epsilon(receiver.xyz, receiver.w);
  vec3 origin = receiver.xyz + Ng * epsilon;
  vec3 tangent = normalize(cross(abs(N.z) < 0.999 ? vec3(0, 0, 1) : vec3(0, 1, 0), N));
  vec3 bitangent = cross(N, tangent);
  vec3 incoming = vec3(0.0);
  int count = uniforms.samples_lights_environment_seed.x;
  for (int i = 0; i < count; i++) {
    vec4 random = random_sample(pixel, uint(i));
    float radius = sqrt(random.x), angle = 6.28318530718 * random.y;
    vec3 L = tangent * (radius * cos(angle)) + bitangent * (radius * sin(angle)) +
             N * sqrt(max(1.0 - random.x, 0.0));
    /* Never start a GI ray through the geometric back side of a smooth-shaded surface. */
    if (dot(L, Ng) <= 0.0) {
      continue;
    }
    Hit hit = trace_ray(origin, L, 100000.0);
    if (!hit.valid) {
      incoming += world_radiance(L);
      continue;
    }
    vec3 P = origin + L * hit.distance;
    vec3 hit_N = geometric_normal(hit.instance, hit.primitive, -L);
    vec3 albedo = clamp(diffuse_albedo[hit.instance].rgb, vec3(0.0), vec3(1.0));
    /* Hit albedo B belongs here; receiver albedo A belongs to Eevee's material replay resolve. */
    incoming += max(emissive_radiance[hit.instance].rgb, vec3(0.0));
    /* Pure metals/glass/black proxies cannot contribute a diffuse light sample. Avoid their
     * shadow traversal rather than tracing it and multiplying its result by zero. */
    if (any(greaterThan(albedo, vec3(0.0)))) {
      incoming += albedo * direct_radiance(P, hit_N, ray_epsilon(P, hit.distance), random.z);
    }
  }
  incoming /= float(count);
  /* Alpha means evaluated, including a physically black result in a sealed room. */
  imageStore(receiver_gi_img, texel, vec4(min(incoming, vec3(65504.0)), 1.0));
}
