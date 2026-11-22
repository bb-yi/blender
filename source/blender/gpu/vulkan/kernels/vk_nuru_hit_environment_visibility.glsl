#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_control_flow_attributes : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(scalar, binding = 0) uniform NuruUniforms {
  mat4 viewinv;
  mat4 wininv;
  ivec4 resolution_samples;
  vec4 normal_bias_pad;
  vec4 sampling_rand;
} uniforms;

layout(binding = 1) uniform accelerationStructureEXT scene;

layout(scalar, binding = 2) readonly buffer B2 { uint tiles_coord_buf[]; };

layout(binding = 16) uniform sampler2D hit_normal_img;
layout(binding = 17) uniform sampler2D hit_world_position_img;
layout(binding = 42) uniform writeonly image2D environment_visibility_img;

uvec2 unpackUvec2x16(uint packed)
{
  return uvec2(packed & 0xFFFFu, packed >> 16u);
}

float hash12(vec2 p)
{
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

vec2 rand2_shadow(uvec2 tid, int sample_index, int layer, vec4 sampling_rand)
{
  const vec2 seed = vec2(sampling_rand.x * 23.47 + sampling_rand.z * 11.13,
                         sampling_rand.y * 29.59 + sampling_rand.w * 7.71);
  const vec2 base = vec2(float(tid.x), float(tid.y)) + vec2(float(layer) * 13.17, float(sample_index) * 19.31) + seed;
  return vec2(hash12(base + vec2(0.17, 0.31)), hash12(base.yx + vec2(0.73, 0.53)));
}

vec2 sample_circle(float rand)
{
  const float phi = (rand - 0.5) * 6.28318530718;
  return vec2(cos(phi), sin(phi));
}

vec2 sample_disk(vec2 rand)
{
  return sample_circle(rand.y) * sqrt(rand.x);
}

void make_orthonormal_basis(vec3 n, out vec3 right, out vec3 up)
{
  const vec3 helper = (abs(n.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
  right = normalize(cross(helper, n));
  up = normalize(cross(n, right));
}

vec3 sample_environment_visibility_direction(uvec2 tid, int sample_index, vec3 N)
{
  vec3 right, up;
  make_orthonormal_basis(N, right, up);
  const vec2 disk = sample_disk(rand2_shadow(tid, sample_index, uniforms.resolution_samples.w, uniforms.sampling_rand));
  const float z = sqrt(max(0.0, 1.0 - dot(disk, disk)));
  return normalize(right * disk.x + up * disk.y + N * z);
}

/* Per-thread ray budget guarding against an NVIDIA shader-compiler miscompile (595.71,
 * RTX 5090) that spins ray-query loops until the GPU channel dies (Xid 109). Far above any
 * legitimate workload; exhausted threads treat further rays as unoccluded. See
 * vk_nuru_trace_override.glsl for the validation history. */
int g_ray_budget = 65536;

void main()
{
  const uvec2 tile_coord = unpackUvec2x16(tiles_coord_buf[gl_WorkGroupID.x]);
  const uvec2 tid = gl_LocalInvocationID.xy + tile_coord * 8u;
  if (tid.x >= uint(textureSize(hit_world_position_img, 0).x) ||
      tid.y >= uint(textureSize(hit_world_position_img, 0).y)) {
    return;
  }
  const vec3 P = texelFetch(hit_world_position_img, ivec2(tid), 0).xyz;
  vec3 N = texelFetch(hit_normal_img, ivec2(tid), 0).xyz;
  if (any(isnan(P)) || any(isinf(P)) || any(isnan(N)) || any(isinf(N)) || dot(N, N) < 1.0e-10) {
    return;
  }
  N = normalize(N);
  const float normal_bias = max(4.0e-3, uniforms.normal_bias_pad.x);
  const float ray_tmin = max(5.0e-4, normal_bias * 0.25);
  const int sample_count = max(uniforms.resolution_samples.z, 1);
  float visibility = 0.0;
  const vec3 origin = P + N * normal_bias;
  vec3 average_direction = vec3(0.0);
  for (int sample_index = 0; sample_index < sample_count; sample_index++) {
    const vec3 sample_dir = sample_environment_visibility_direction(tid, sample_index, N);
    if (g_ray_budget-- <= 0) {
      visibility += 1.0;
      average_direction += sample_dir;
      continue;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, scene, gl_RayFlagsOpaqueEXT, 0xFFu, origin, ray_tmin, sample_dir, 100000.0);
    int proceed_guard_rq = 4096;
    while (rayQueryProceedEXT(rq) && (proceed_guard_rq-- > 0)) {
    }
    const bool hit = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
    const float sample_visibility = hit ? 0.0 : 1.0;
    visibility += sample_visibility;
    average_direction += sample_dir * sample_visibility;
  }
  visibility /= float(sample_count);
  average_direction /= float(sample_count);
  imageStore(environment_visibility_img, ivec2(tid), vec4(average_direction, visibility));
}
