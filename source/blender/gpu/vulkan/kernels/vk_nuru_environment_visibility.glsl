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

layout(binding = 16) uniform sampler2D depth_tx;
layout(binding = 17) uniform usampler2DArray gbuf_header_tx;
layout(binding = 18) uniform sampler2DArray gbuf_normal_tx;
layout(binding = 43) uniform writeonly image2D environment_visibility_img;

vec3 point_screen_to_world(vec2 uv, float depth, mat4 wininv, mat4 viewinv)
{
  vec3 ssP = vec3(uv, depth);
  vec3 ndc = ssP * 2.0 - 1.0;
  vec4 viewP = wininv * vec4(ndc, 1.0);
  vec3 vP = viewP.xyz / viewP.w;
  return (viewinv * vec4(vP, 1.0)).xyz;
}

vec3 point_screen_to_world(ivec2 texel, float depth)
{
  const vec2 uv = (vec2(texel) + 0.5) / vec2(uniforms.resolution_samples.xy);
  return point_screen_to_world(uv, depth, uniforms.wininv, uniforms.viewinv);
}

bool depth_is_valid(float depth)
{
  return depth > 0.0 && depth < 1.0;
}

float sample_depth_clamped(ivec2 texel, sampler2D depth_tx)
{
  const ivec2 clamped = clamp(texel, ivec2(0), ivec2(uniforms.resolution_samples.xy) - ivec2(1));
  const vec2 uv = (vec2(clamped) + 0.5) / vec2(uniforms.resolution_samples.xy);
  return 1.0 - textureLod(depth_tx, uv, 0.0).r;
}

vec3 normal_unpack(vec2 N_packed)
{
  N_packed = N_packed * 2.0 - 1.0;
  vec3 N = vec3(N_packed.x, N_packed.y, 1.0 - abs(N_packed.x) - abs(N_packed.y));
  const float t = clamp(-N.z, 0.0, 1.0);
  N.x += (N.x >= 0.0) ? -t : t;
  N.y += (N.y >= 0.0) ? -t : t;
  return normalize(N);
}

vec3 geometry_normal_unpack(uint data, vec3 N)
{
  if ((data & (63u << 20u)) == 0u) {
    return N;
  }
  vec3 Ng = vec3((uvec3(data) >> (uvec3(0u, 1u, 2u) + 20u)) & 1u) -
            vec3((uvec3(data) >> (uvec3(3u, 4u, 5u) + 20u)) & 1u);
  return normalize(Ng);
}

bool load_gbuffer_receiver_normal(ivec2 texel,
                                  usampler2DArray gbuf_header_tx,
                                  sampler2DArray gbuf_normal_tx,
                                  out vec3 r_N)
{
  const uint header = texelFetch(gbuf_header_tx, ivec3(texel, 0), 0).x;
  if (header == 0u) {
    r_N = vec3(0.0);
    return false;
  }
  const vec2 packed_N = texelFetch(gbuf_normal_tx, ivec3(texel, 0), 0).xy;
  const vec3 surface_N = normal_unpack(packed_N);
  r_N = geometry_normal_unpack(header, surface_N);
  return !any(isnan(r_N)) && !any(isinf(r_N)) && (dot(r_N, r_N) > 1.0e-10);
}

/* See `vk_nuru_directional_shadow.glsl` `view_ray_receiver_slack`: bound the depth
 * reconstruction error along the view ray to keep visibility-ray origins on the camera side of
 * the receiver surface. Accept coplanar deltas (grazing recession) and near-range deltas
 * (adjacent perpendicular face of the same edge); far off-plane silhouette gaps must not pull
 * the origin or they rim edges with light. */
float view_ray_receiver_slack(
    ivec2 texel, float depth, vec3 P, vec3 view_dir, vec3 receiver_Ng, sampler2D depth_tx)
{
  const float pitch = length(point_screen_to_world(texel + ivec2(1, 0), depth) - P);
  float slack = 0.0;
  const ivec2 offsets[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
  for (int i = 0; i < 4; i++) {
    const float neighbor_depth = sample_depth_clamped(texel + offsets[i], depth_tx);
    if (!depth_is_valid(neighbor_depth)) {
      continue;
    }
    const vec3 offset = point_screen_to_world(texel + offsets[i], neighbor_depth) - P;
    const float offset_len = length(offset);
    if (!(offset_len > 1.0e-9)) {
      continue;
    }
    const float along_view = dot(offset, view_dir);
    if (along_view < 0.0) {
      /* Nearer neighbor: always occlusion-safe to pull by (see directional kernel). */
      slack = max(slack, -along_view);
      continue;
    }
    const bool coplanar = abs(dot(offset, receiver_Ng)) <= 0.25 * offset_len;
    const bool near_range = offset_len <= 8.0 * max(pitch, 1.0e-9);
    if (!coplanar && !near_range) {
      continue;
    }
    slack = max(slack, along_view);
  }
  return slack;
}

vec3 estimate_world_normal(ivec2 texel, float depth, sampler2D depth_tx)
{
  const vec3 P = point_screen_to_world(texel, depth);
  const float depth_px = sample_depth_clamped(texel + ivec2(1, 0), depth_tx);
  const float depth_nx = sample_depth_clamped(texel + ivec2(-1, 0), depth_tx);
  const float depth_py = sample_depth_clamped(texel + ivec2(0, 1), depth_tx);
  const float depth_ny = sample_depth_clamped(texel + ivec2(0, -1), depth_tx);
  vec3 dPdx = vec3(0.0);
  vec3 dPdy = vec3(0.0);
  if (depth_is_valid(depth_px) && depth_is_valid(depth_nx)) {
    const bool use_pos = abs(depth_px - depth) < abs(depth_nx - depth);
    const vec3 Pn = point_screen_to_world(texel + (use_pos ? ivec2(1, 0) : ivec2(-1, 0)), use_pos ? depth_px : depth_nx);
    dPdx = use_pos ? (Pn - P) : (P - Pn);
  }
  else if (depth_is_valid(depth_px)) {
    dPdx = point_screen_to_world(texel + ivec2(1, 0), depth_px) - P;
  }
  else if (depth_is_valid(depth_nx)) {
    dPdx = P - point_screen_to_world(texel + ivec2(-1, 0), depth_nx);
  }
  if (depth_is_valid(depth_py) && depth_is_valid(depth_ny)) {
    const bool use_pos = abs(depth_py - depth) < abs(depth_ny - depth);
    const vec3 Pn = point_screen_to_world(texel + (use_pos ? ivec2(0, 1) : ivec2(0, -1)), use_pos ? depth_py : depth_ny);
    dPdy = use_pos ? (Pn - P) : (P - Pn);
  }
  else if (depth_is_valid(depth_py)) {
    dPdy = point_screen_to_world(texel + ivec2(0, 1), depth_py) - P;
  }
  else if (depth_is_valid(depth_ny)) {
    dPdy = P - point_screen_to_world(texel + ivec2(0, -1), depth_ny);
  }
  if (dot(dPdx, dPdx) <= 1.0e-16 || dot(dPdy, dPdy) <= 1.0e-16) {
    return vec3(0.0);
  }
  vec3 N = cross(dPdx, dPdy);
  const float len_sq = dot(N, N);
  if (!(len_sq > 1.0e-16)) {
    return vec3(0.0);
  }
  N *= inversesqrt(len_sq);
  const vec3 camera_pos = uniforms.viewinv[3].xyz;
  if (dot(N, camera_pos - P) < 0.0) {
    N = -N;
  }
  return N;
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
  const uvec2 tid = gl_GlobalInvocationID.xy;
  if (tid.x >= uint(uniforms.resolution_samples.x) || tid.y >= uint(uniforms.resolution_samples.y)) {
    return;
  }
  const vec2 uv = (vec2(tid) + 0.5) / vec2(uniforms.resolution_samples.xy);
  const float depth = 1.0 - textureLod(depth_tx, uv, 0.0).r;
  if (!depth_is_valid(depth)) {
    imageStore(environment_visibility_img, ivec2(tid), vec4(0.0, 0.0, 0.0, 1.0));
    return;
  }
  const vec3 P = point_screen_to_world(ivec2(tid), depth);
  vec3 N = vec3(0.0);
  if (!load_gbuffer_receiver_normal(ivec2(tid), gbuf_header_tx, gbuf_normal_tx, N)) {
    N = estimate_world_normal(ivec2(tid), depth, depth_tx);
  }
  if (dot(N, N) < 1.0e-10) {
    imageStore(environment_visibility_img, ivec2(tid), vec4(0.0, 0.0, 0.0, 1.0));
    return;
  }
  N = normalize(N);
  const float normal_bias = max(4.0e-3, uniforms.normal_bias_pad.x);
  const float ray_tmin = max(5.0e-4, normal_bias * 0.25);
  const int sample_count = max(uniforms.resolution_samples.z, 1);
  float visibility = 0.0;
  const vec3 camera_position = uniforms.viewinv[3].xyz;
  const vec3 to_receiver = P - camera_position;
  const float receiver_distance = max(length(to_receiver), 1.0e-6);
  const vec3 view_dir = to_receiver / receiver_distance;
  const float receiver_slack = min(
      view_ray_receiver_slack(ivec2(tid), depth, P, view_dir, N, depth_tx),
      0.5 * receiver_distance);
  const vec3 origin = (P - view_dir * receiver_slack) + N * normal_bias;
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
