#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_control_flow_attributes : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

const uint HWRT_CLOSURE_REFRACTION = 12u;
const uint HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT = 1u << 2u;
const uint HWRT_PROXY_FLAG_THIN_GLASS = 1u << 7u;
const uint LIGHT_OMNI_SPHERE = 10u;
const uint LIGHT_SPOT_SPHERE = 12u;
const uint LIGHT_RECT = 20u;

struct HardwareMaterialProxy {
  vec4 reflection_color_roughness;
  vec4 transmission_color_roughness;
  vec4 ior_closure_type;
  vec4 packed_thickness;
};
struct TriangleNormalRange {
  uint offset;
  uint count;
};

layout(scalar, binding = 0) uniform NuruUniforms {
  mat4 viewinv;
  mat4 wininv;
  ivec4 resolution_layer_type;
  vec4 light_position_radius;
  vec4 light_x_axis_size_x;
  vec4 light_y_axis_size_y;
  vec4 shadow_offset_scale;
  vec4 normal_bias_pad;
  vec4 sampling_rand;
  vec4 caustic_params;
} uniforms;
layout(binding = 1) uniform accelerationStructureEXT scene;
layout(scalar, binding = 2) readonly buffer B2 { HardwareMaterialProxy material_proxies[]; };
layout(scalar, binding = 3) readonly buffer B3 { vec4 triangle_normals[]; };
layout(scalar, binding = 4) readonly buffer B4 { vec4 triangle_smooth_normals[]; };
layout(scalar, binding = 5) readonly buffer B5 { TriangleNormalRange triangle_normal_ranges[]; };
layout(binding = 16) uniform sampler2D depth_tx;
layout(binding = 17) uniform usampler2DArray gbuf_header_tx;
layout(binding = 18) uniform sampler2DArray gbuf_normal_tx;
layout(binding = 43) uniform writeonly image2DArray shadow_visibility_img;

vec3 barycentric_expand(vec2 barycentric)
{
  return vec3(max(0.0, 1.0 - barycentric.x - barycentric.y), barycentric.x, barycentric.y);
}
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
  const vec2 uv = (vec2(texel) + 0.5) / vec2(uniforms.resolution_layer_type.xy);
  return point_screen_to_world(uv, depth, uniforms.wininv, uniforms.viewinv);
}
bool depth_is_valid(float depth)
{
  return depth > 0.0 && depth < 1.0;
}
float sample_depth_clamped(ivec2 texel, sampler2D depth_tx)
{
  const ivec2 clamped = clamp(texel, ivec2(0), ivec2(uniforms.resolution_layer_type.xy) - ivec2(1));
  const vec2 uv = (vec2(clamped) + 0.5) / vec2(uniforms.resolution_layer_type.xy);
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
  vec3 Ng = vec3((uvec3(data) >> (uvec3(0, 1, 2) + 20u)) & 1u) -
            vec3((uvec3(data) >> (uvec3(3, 4, 5) + 20u)) & 1u);
  return normalize(Ng);
}
bool load_gbuffer_receiver_normal(ivec2 texel,
                                  usampler2DArray gbuf_header_tx,
                                  sampler2DArray gbuf_normal_tx,
                                  out vec3 r_N)
{
  const uint header = texelFetch(gbuf_header_tx, ivec3(texel, 0), 0).x;
  if (header == 0u) {
    return false;
  }
  const vec2 packed_N = texelFetch(gbuf_normal_tx, ivec3(texel, 0), 0).xy;
  const vec3 surface_N = normal_unpack(packed_N);
  r_N = geometry_normal_unpack(header, surface_N);
  return !any(isnan(r_N)) && !any(isinf(r_N)) && (dot(r_N, r_N) > 1.0e-10);
}
/* See `vk_nuru_directional_shadow.glsl` `view_ray_receiver_slack`: bound the depth
 * reconstruction error along the view ray to keep shadow origins on the camera side of the
 * receiver surface. Accept coplanar deltas (grazing recession) and near-range deltas (adjacent
 * perpendicular face of the same edge); far off-plane silhouette gaps must not pull the origin
 * or they rim edges with light. */
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
  if (dot(N, (uniforms.light_position_radius.xyz + uniforms.shadow_offset_scale.xyz) - P) < 0.0) {
    N = -N;
  }
  return N;
}
bool is_area_light(uint type)
{
  return type >= LIGHT_RECT;
}
bool is_sphere_light(uint type)
{
  return type == LIGHT_OMNI_SPHERE || type == LIGHT_SPOT_SPHERE;
}
vec2 rand2_shadow(uvec2 tid, int sample_index, int layer, vec4 sampling_rand)
{
  /* The float sin(dot(pixel, seed)) hash produces spatially correlated shadow bands.
   * Use the unsigned PCG4D mixing from gpu_shader_common_hash.glsl. Keep pixel, light,
   * ray and temporal-sample identities separate until the integer avalanche. */
  uvec4 v = uvec4(tid, uint(sample_index), uint(layer)) ^ floatBitsToUint(sampling_rand);
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
  return vec2(v.xy >> 8u) * (1.0 / 16777216.0);
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
float projected_sphere_disk_radius(float sphere_radius, float distance_to_sphere)
{
  return sphere_radius * inversesqrt(max(1.0e-8, 1.0 - (sphere_radius * sphere_radius) / max(distance_to_sphere * distance_to_sphere, 1.0e-8)));
}
void make_orthonormal_basis(vec3 n, out vec3 right, out vec3 up)
{
  const vec3 helper = (abs(n.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
  right = normalize(cross(helper, n));
  up = normalize(cross(n, right));
}
vec3 sample_local_shadow_target(uvec2 tid, int sample_index, vec3 P)
{
  const vec3 center = uniforms.light_position_radius.xyz + uniforms.shadow_offset_scale.xyz;
  if (is_area_light(uint(uniforms.resolution_layer_type.w))) {
    vec2 rand = rand2_shadow(tid, sample_index, uniforms.resolution_layer_type.z, uniforms.sampling_rand);
    if (uint(uniforms.resolution_layer_type.w) == LIGHT_RECT) {
      rand = rand * 2.0 - 1.0;
    }
    else {
      rand = sample_disk(rand);
    }
    rand *= vec2(uniforms.light_x_axis_size_x.w, uniforms.light_y_axis_size_y.w) * uniforms.shadow_offset_scale.w;
    return center + uniforms.light_x_axis_size_x.xyz * rand.x + uniforms.light_y_axis_size_y.xyz * rand.y;
  }
  vec3 L = center - P;
  const float distance_to_light = length(L);
  if (!(distance_to_light > 1.0e-5)) {
    return center;
  }
  L /= distance_to_light;
  float radius = uniforms.light_position_radius.w;
  if (is_sphere_light(uint(uniforms.resolution_layer_type.w))) {
    radius = projected_sphere_disk_radius(radius, distance_to_light);
  }
  if (!(radius > 1.0e-6)) {
    return center;
  }
  vec3 right;
  vec3 up;
  make_orthonormal_basis(L, right, up);
  const vec2 disk = sample_disk(rand2_shadow(tid, sample_index, uniforms.resolution_layer_type.z, uniforms.sampling_rand)) * radius;
  return center + right * disk.x + up * disk.y;
}
/* Nuru: classic opaque HWRT shadow. Thin Glass is pass-through; any other triangle blocks. */
/* Per-thread ray budget guarding against an NVIDIA shader-compiler miscompile (595.71,
 * RTX 5090) that spins ray-query loops until the GPU channel dies (Xid 109). Far above any
 * legitimate workload; exhausted threads treat further rays as unoccluded. See
 * vk_nuru_trace_override.glsl for the validation history. */
int g_ray_budget = 65536;

vec3 opaque_shadow_visibility(accelerationStructureEXT scene,
                              vec3 origin,
                              vec3 dir,
                              float t_min,
                              float t_max)
{
  if (g_ray_budget-- <= 0) {
    return vec3(1.0);
  }
  float current_tmin = t_min;
  for (int b = 0; b < 8; b++) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, scene, gl_RayFlagsOpaqueEXT, 0xFFu, origin, current_tmin, dir, t_max);
    int proceed_guard_rq = 4096;
    while (rayQueryProceedEXT(rq) && (proceed_guard_rq-- > 0)) {
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
      return vec3(1.0);
    }
    const float hit_distance = rayQueryGetIntersectionTEXT(rq, true);
    const uint hit_user_id = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    const HardwareMaterialProxy proxy = material_proxies[hit_user_id];
    const uint proxy_flags = uint(proxy.ior_closure_type.w);
    if ((proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) == 0u) {
      return vec3(0.0);
    }
    current_tmin = hit_distance + 1.0e-4;
    if (current_tmin >= t_max) {
      return vec3(1.0);
    }
  }
  return vec3(0.0);
}
vec3 hardware_shadow_apply_color_transmission(vec3 tint, float color_intensity)
{
  const float luma = dot(tint, vec3(0.2126, 0.7152, 0.0722));
  return mix(vec3(luma), tint, clamp(color_intensity, 0.0, 1.0));
}
/* Nuru: Snell-bent caustic trace. Returns ONLY the caustic contribution (tinted throughput
 * along the refracted path, modulated by a tight alignment cone against the light's
 * direction at the exit, scaled by Photons intensity). Returns vec3(0) if the ray never
 * actually bent (no refractive surface hit) so it contributes nothing to non-glass paths. */
vec3 transparent_shadow_caustic_only(accelerationStructureEXT scene,
                                     vec3 origin,
                                     vec3 dir,
                                     float t_min,
                                     float t_max,
                                     bool is_directional,
                                     vec3 light_pos_or_dir,
                                     float photons_intensity)
{
  if (g_ray_budget-- <= 0) {
    return vec3(1.0);
  }
  const int max_transparent_bounces = 8;
  vec3 throughput = vec3(1.0);
  float current_tmin = t_min;
  float current_tmax = t_max;
  bool ray_was_bent = false;
  for (int b = 0; b < max_transparent_bounces; b++) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, scene, gl_RayFlagsOpaqueEXT, 0xFFu, origin, current_tmin, dir, current_tmax);
    int proceed_guard_rq = 4096;
    while (rayQueryProceedEXT(rq) && (proceed_guard_rq-- > 0)) {
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
      if (!ray_was_bent) {
        return vec3(0.0);
      }
      const vec3 to_light = is_directional ? light_pos_or_dir
                                           : normalize(light_pos_or_dir - origin);
      const float align = clamp(dot(dir, to_light), 0.0, 1.0);
      /* Nuru: smooth cone falloff. `pow(align, 128)` was too binary -> every receiver pixel
       * either fired at full brightness or got nothing, producing speckled fireflies. The
       * smoothstep gives partial credit across a ~10 degree band so adjacent pixels'
       * contributions vary smoothly. Caustic outline is wider but visibly less noisy. */
      const float caustic_peak = photons_intensity *
                                 smoothstep(0.985, 0.999, align);
      return throughput * caustic_peak;
    }
    const float hit_distance = rayQueryGetIntersectionTEXT(rq, true);
    const uint user_id = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    const uint hit_primitive_id = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    const vec2 hit_barycentric = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    const HardwareMaterialProxy proxy = material_proxies[user_id];
    const uint proxy_flags = uint(proxy.ior_closure_type.w);
    if ((proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) != 0u) {
      current_tmin = hit_distance + 1.0e-4;
      if (current_tmin >= current_tmax) {
        return ray_was_bent ? throughput : vec3(0.0);
      }
      continue;
    }
    const uint closure_type = uint(proxy.ior_closure_type.z);
    const float alpha = proxy.packed_thickness.y;
    const float refraction_roughness = proxy.transmission_color_roughness.w;
    /* Nuru: caustic-bendable only for low-roughness REFRACTION (clear glass / refractive
     * BSDF). Reflection / metal is intentionally NOT handled here: the per-pixel bent-shadow
     * approach is geometrically sparse for mirrors and produces noisy fireflies that don't
     * converge cleanly. Metal caustics are best done as a separate forward photon-mapping
     * stage and that is left for a future change; for now metal is opaque in the caustic
     * trace (same as DIAMOND 19 behavior: metal hard-shadows correctly in `tint_only`). */
    const float caustic_roughness_threshold = 0.15;
    const bool is_caustic_refraction = (closure_type == HWRT_CLOSURE_REFRACTION) &&
                                       (refraction_roughness < caustic_roughness_threshold);
    const bool is_alpha_blend = ((proxy_flags & HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT) != 0u);
    if (is_caustic_refraction) {
      throughput *= proxy.transmission_color_roughness.rgb;
    }
    else if (is_alpha_blend) {
      throughput *= vec3(clamp(1.0 - alpha, 0.0, 1.0));
    }
    else {
      /* Metal (any reflection closure), rough glass, diffuse, or any other surface
       * terminates the bent caustic ray. The tinted shadow path is independent. */
      return vec3(0.0);
    }
    const float max_through = max(throughput.r, max(throughput.g, throughput.b));
    if (max_through < 1.0e-3) {
      return vec3(0.0);
    }
    if (is_caustic_refraction) {
      /* Snell refraction via smooth normal. `eta = n_from / n_to`. */
      const TriangleNormalRange normal_range = triangle_normal_ranges[user_id];
      const uint primitive_offset = normal_range.offset + hit_primitive_id;
      const vec3 flat_N = triangle_normals[primitive_offset].xyz;
      const uint smooth_offset = primitive_offset * 3u;
      const vec3 bary = barycentric_expand(hit_barycentric);
      vec3 smooth_N_raw =
          triangle_smooth_normals[smooth_offset + 0u].xyz * bary.x +
          triangle_smooth_normals[smooth_offset + 1u].xyz * bary.y +
          triangle_smooth_normals[smooth_offset + 2u].xyz * bary.z;
      const float smooth_len_sq = dot(smooth_N_raw, smooth_N_raw);
      const vec3 N_smooth = (smooth_len_sq > 1.0e-10) ? (smooth_N_raw * inversesqrt(smooth_len_sq))
                                                      : flat_N;
      const float cos_i_signed = dot(dir, N_smooth);
      const bool entering = (cos_i_signed < 0.0);
      const vec3 N_oriented = entering ? N_smooth : -N_smooth;
      const float cos_i = -dot(dir, N_oriented);
      const float ior = max(proxy.ior_closure_type.y, 1.0);
      const float eta = entering ? (1.0 / ior) : ior;
      const float sin2_t = eta * eta * (1.0 - cos_i * cos_i);
      if (sin2_t >= 1.0) {
        /* Total internal reflection - no caustic path through this hit. */
        return vec3(0.0);
      }
      const float cos_t = sqrt(1.0 - sin2_t);
      const vec3 refracted = normalize(eta * dir + (eta * cos_i - cos_t) * N_oriented);
      const vec3 hit_P = origin + dir * hit_distance;
      origin = hit_P;
      dir = refracted;
      current_tmin = 1.0e-3;
      current_tmax = is_directional ? 100000.0
                                    : max(length(light_pos_or_dir - origin), 1.0e-3);
      ray_was_bent = true;
    }
    else {
      current_tmin = hit_distance + 1.0e-4;
      if (current_tmin >= current_tmax) {
        if (!ray_was_bent) {
          return vec3(0.0);
        }
        const vec3 to_light = is_directional ? light_pos_or_dir
                                             : normalize(light_pos_or_dir - origin);
        const float align = clamp(dot(dir, to_light), 0.0, 1.0);
        const float caustic_peak = photons_intensity *
                                   smoothstep(0.985, 0.999, align);
        return throughput * caustic_peak;
      }
    }
  }
  return vec3(0.0);
}
/* Nuru: single shadow trace with opacity/transmission blend and Color Transmission tint.
 * transparency = 0 uses one any-hit opaque test. Otherwise one material-aware walk:
 * glass/alpha attenuates throughput, opaque surfaces block. With any hit along the ray the
 * opaque component is fully blocked, so mix(opaque, tinted, t) reduces to tinted * t. */
vec3 hardware_shadow_visibility(accelerationStructureEXT scene,
                                vec3 origin,
                                vec3 dir,
                                float t_min,
                                float t_max,
                                float transparency,
                                bool enable_caustics,
                                bool is_directional,
                                vec3 light_pos_or_dir,
                                float color_intensity,
                                float photons_intensity)
{
  if (g_ray_budget-- <= 0) {
    return vec3(1.0);
  }
  const float t = clamp(transparency, 0.0, 1.0);
  if (t <= 0.0) {
    return opaque_shadow_visibility(scene, origin, dir, t_min, t_max);
  }
  const int max_transparent_bounces = 8;
  vec3 throughput = vec3(1.0);
  float current_tmin = t_min;
  bool any_hit = false;
  const float vis_scale = min(t, 1.0);
  for (int b = 0; b < max_transparent_bounces; b++) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, scene, gl_RayFlagsOpaqueEXT, 0xFFu, origin, current_tmin, dir, t_max);
    int proceed_guard_rq = 4096;
    while (rayQueryProceedEXT(rq) && (proceed_guard_rq-- > 0)) {
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
      if (!any_hit) {
        return vec3(1.0);
      }
      const vec3 tinted = hardware_shadow_apply_color_transmission(throughput, color_intensity);
      vec3 visibility = tinted * vis_scale;
      if (enable_caustics && vis_scale > 0.0) {
        const vec3 caustic = transparent_shadow_caustic_only(
            scene, origin, dir, t_min, t_max,
            is_directional, light_pos_or_dir, photons_intensity);
        const float unoccluded = clamp(max(tinted.r, max(tinted.g, tinted.b)), 0.0, 1.0);
        visibility += caustic * clamp(1.0 - unoccluded, 0.0, 1.0) * vis_scale;
      }
      return visibility;
    }
    const float hit_distance = rayQueryGetIntersectionTEXT(rq, true);
    const uint user_id = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    const HardwareMaterialProxy proxy = material_proxies[user_id];
    const uint proxy_flags = uint(proxy.ior_closure_type.w);
    if ((proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) != 0u) {
      /* Nuru: thin glass passes shadows without attenuation (matching the transparency=0 opaque
       * walk). It must not flag `any_hit`, otherwise a pure thin-glass path gets multiplied by
       * `vis_scale` and the shadow jumps from fully lit (transparency 0) to nearly black at
       * transparency 0.001. */
      current_tmin = hit_distance + 1.0e-4;
      if (current_tmin >= t_max) {
        return any_hit ? hardware_shadow_apply_color_transmission(throughput, color_intensity) *
                             vis_scale :
                         vec3(1.0);
      }
      continue;
    }
    any_hit = true;
    const uint closure_type = uint(proxy.ior_closure_type.z);
    const float alpha = proxy.packed_thickness.y;
    const bool is_refraction = (closure_type == HWRT_CLOSURE_REFRACTION);
    const bool is_alpha_blend = ((proxy_flags & HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT) != 0u);
    if (is_refraction) {
      throughput *= proxy.transmission_color_roughness.rgb;
    }
    else if (is_alpha_blend) {
      throughput *= vec3(clamp(1.0 - alpha, 0.0, 1.0));
    }
    else {
      return vec3(0.0);
    }
    const float max_through = max(throughput.r, max(throughput.g, throughput.b));
    if (max_through < 1.0e-3) {
      return vec3(0.0);
    }
    current_tmin = hit_distance + 1.0e-4;
    if (current_tmin >= t_max) {
      const vec3 tinted = hardware_shadow_apply_color_transmission(throughput, color_intensity);
      vec3 visibility = tinted * vis_scale;
      if (enable_caustics && vis_scale > 0.0) {
        const vec3 caustic = transparent_shadow_caustic_only(
            scene, origin, dir, t_min, t_max,
            is_directional, light_pos_or_dir, photons_intensity);
        const float unoccluded = clamp(max(tinted.r, max(tinted.g, tinted.b)), 0.0, 1.0);
        visibility += caustic * clamp(1.0 - unoccluded, 0.0, 1.0) * vis_scale;
      }
      return visibility;
    }
  }
  return vec3(0.0);
}
void main()
{
  const uvec2 tid = gl_GlobalInvocationID.xy;
  if (tid.x >= uint(uniforms.resolution_layer_type.x) || tid.y >= uint(uniforms.resolution_layer_type.y)) {
    return;
  }
  const vec2 uv = (vec2(tid) + 0.5) / vec2(uniforms.resolution_layer_type.xy);
  const float depth = 1.0 - textureLod(depth_tx, uv, 0.0).r;
  if (!depth_is_valid(depth)) {
    imageStore(shadow_visibility_img, ivec3(ivec2(tid), int(uint(uniforms.resolution_layer_type.z))), vec4(1.0));
    return;
  }
  const vec3 P = point_screen_to_world(ivec2(tid), depth);
  vec3 center = uniforms.light_position_radius.xyz + uniforms.shadow_offset_scale.xyz;
  vec3 L = center - P;
  const float light_distance = length(L);
  if (!(light_distance > 1.0e-5)) {
    imageStore(shadow_visibility_img, ivec3(ivec2(tid), int(uint(uniforms.resolution_layer_type.z))), vec4(1.0));
    return;
  }
  L /= light_distance;
  vec3 N = vec3(0.0);
  if (!load_gbuffer_receiver_normal(ivec2(tid), gbuf_header_tx, gbuf_normal_tx, N)) {
    N = estimate_world_normal(ivec2(tid), depth, depth_tx);
  }
  if (dot(N, N) < 1.0e-10) {
    N = L;
  }
  const float normal_bias = max(4.0e-3, uniforms.normal_bias_pad.x);
  const float ray_tmin = max(5.0e-4, normal_bias * 0.25);
  const vec3 camera_position = uniforms.viewinv[3].xyz;
  const vec3 to_receiver = P - camera_position;
  const float receiver_distance = max(length(to_receiver), 1.0e-6);
  const vec3 view_dir = to_receiver / receiver_distance;
  const float receiver_slack = min(
      view_ray_receiver_slack(ivec2(tid), depth, P, view_dir, N, depth_tx),
      0.5 * receiver_distance);
  const vec3 P_corrected = P - view_dir * receiver_slack;
  const bool area_soft = is_area_light(uint(uniforms.resolution_layer_type.w)) && (max(uniforms.light_x_axis_size_x.w, uniforms.light_y_axis_size_y.w) * uniforms.shadow_offset_scale.w > 1.0e-6);
  const bool local_soft = (!is_area_light(uint(uniforms.resolution_layer_type.w))) && (uniforms.light_position_radius.w > 1.0e-6);
  const int sample_count = (area_soft || local_soft) ? max(int(uniforms.normal_bias_pad.y), 1) : 1;
  const bool enable_caustics = (uniforms.normal_bias_pad.z > 0.5);
  const float color_intensity = clamp(uniforms.normal_bias_pad.w, 0.0, 1.0);
  const float photons_intensity = max(uniforms.caustic_params.x, 0.0);
  const float transparent_shadows = uniforms.caustic_params.y;
  vec3 visibility = vec3(0.0);
  for (int sample_index = 0; sample_index < sample_count; sample_index++) {
    const vec3 target = sample_local_shadow_target(tid, sample_index, P);
    vec3 sample_L = target - P_corrected;
    const float sample_distance = length(sample_L);
    if (!(sample_distance > 1.0e-5)) {
      visibility += vec3(1.0);
      continue;
    }
    sample_L /= sample_distance;
    const float ray_tmax = max(ray_tmin, sample_distance);
    const vec3 origin = P_corrected + N * normal_bias;
    visibility += hardware_shadow_visibility(scene, origin, sample_L, ray_tmin, ray_tmax, transparent_shadows, enable_caustics, false, target, color_intensity, photons_intensity);
  }
  visibility /= float(sample_count);
  imageStore(shadow_visibility_img, ivec3(ivec2(tid), int(uint(uniforms.resolution_layer_type.z))), vec4(visibility, 1.0));
}
