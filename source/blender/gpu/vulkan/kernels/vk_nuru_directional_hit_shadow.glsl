#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_control_flow_attributes : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

const uint HWRT_CLOSURE_REFRACTION = 12u;
const uint HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT = 1u << 2u;
const uint HWRT_PROXY_FLAG_THIN_GLASS = 1u << 7u;

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
  ivec4 resolution_layer;
  vec4 light_direction_bias;
  vec4 shadow_params;
  ivec4 world_sun_slot_pad;
  vec4 sampling_rand;
} uniforms;

layout(binding = 1) uniform accelerationStructureEXT scene;
layout(scalar, binding = 2) readonly buffer B2 { vec4 world_sunlight_direction[]; };
layout(scalar, binding = 3) readonly buffer B3 { uint tiles_coord_buf[]; };
layout(scalar, binding = 4) readonly buffer B4 { vec4 triangle_normals[]; };
layout(scalar, binding = 5) readonly buffer B5 { TriangleNormalRange triangle_normal_ranges[]; };
layout(scalar, binding = 6) readonly buffer B6 { HardwareMaterialProxy material_proxies[]; };
layout(scalar, binding = 7) readonly buffer B7 { vec4 triangle_smooth_normals[]; };

layout(binding = 16) uniform sampler2D hit_normal_img;
layout(binding = 17) uniform sampler2D hit_world_position_img;
layout(binding = 18) uniform usampler2D hit_identity_img;
layout(binding = 43) uniform writeonly image2DArray shadow_visibility_img;

uvec2 unpackUvec2x16(uint packed)
{
  return uvec2(packed & 0xFFFFu, packed >> 16u);
}

vec3 barycentric_expand(vec2 barycentric)
{
  return vec3(max(0.0f, 1.0f - barycentric.x - barycentric.y), barycentric.x, barycentric.y);
}

vec3 hit_shadow_receiver_normal(uvec2 tid, vec3 fallback_normal, usampler2D hit_identity_img)
{
  vec3 receiver_normal = normalize(fallback_normal);
  const uvec4 hit_identity = texelFetch(hit_identity_img, ivec2(tid), 0);
  const uint user_id = hit_identity.x;
  const uint primitive_id = hit_identity.y;
  const uint identity_flags = hit_identity.z;
  if (user_id == 0xFFFFFFFFu) {
    return receiver_normal;
  }
  const TriangleNormalRange normal_range = triangle_normal_ranges[user_id];
  if (primitive_id < normal_range.count) {
    receiver_normal = triangle_normals[normal_range.offset + primitive_id].xyz;
  }
  const float len_sq = dot(receiver_normal, receiver_normal);
  if (!(len_sq > 1.0e-10f)) {
    receiver_normal = normalize(fallback_normal);
  }
  else {
    receiver_normal *= inversesqrt(len_sq);
  }
  if ((identity_flags & 1u) == 0u) {
    receiver_normal = -receiver_normal;
  }
  return (dot(receiver_normal, fallback_normal) >= 0.0f) ? receiver_normal : -receiver_normal;
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

void make_orthonormal_basis(vec3 n, out vec3 right, out vec3 up)
{
  const vec3 helper = (abs(n.z) < 0.999f) ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);
  right = normalize(cross(helper, n));
  up = normalize(cross(n, right));
}

vec2 sample_directional_shadow_disk(uvec2 tid, int sample_index, int sample_count, int layer, vec4 sampling_rand)
{
  const int safe_sample_count = max(sample_count, 1);
  const vec2 rand = rand2_shadow(tid, sample_index, layer, sampling_rand);
  const float radius = sqrt((float(sample_index) + rand.x) / float(safe_sample_count));
  const float angle = 6.28318530718f * fract(rand.y + 0.61803398875f * float(sample_index));
  return vec2(cos(angle), sin(angle)) * radius;
}

vec3 directional_shadow_light_direction()
{
  if (uniforms.world_sun_slot_pad.x >= 0) {
    const vec4 packed_direction = world_sunlight_direction[uniforms.world_sun_slot_pad.x];
    if (packed_direction.w > 0.0f && !any(isnan(packed_direction.xyz)) && !any(isinf(packed_direction.xyz)) &&
        dot(packed_direction.xyz, packed_direction.xyz) > 1.0e-10f)
    {
      return normalize(packed_direction.xyz);
    }
  }
  return uniforms.light_direction_bias.xyz;
}

vec3 sample_directional_shadow_direction(uvec2 tid, int sample_index)
{
  const vec3 light_direction = directional_shadow_light_direction();
  if (!(uniforms.shadow_params.x > 1.0e-6f)) {
    return light_direction;
  }
  vec3 right, up;
  make_orthonormal_basis(light_direction, right, up);
  const int sample_count = max(int(uniforms.shadow_params.y), 1);
  const vec2 disk = sample_directional_shadow_disk(
      tid, sample_index, sample_count, uniforms.resolution_layer.z, uniforms.sampling_rand) *
                    tan(uniforms.shadow_params.x);
  return normalize(light_direction + right * disk.x + up * disk.y);
}

/* Nuru: classic opaque HWRT shadow. Thin Glass is pass-through; any other triangle blocks. */
/* Per-thread ray budget guarding against an NVIDIA shader-compiler miscompile (595.71,
 * RTX 5090) that spins ray-query loops until the GPU channel dies (Xid 109). Far above any
 * legitimate workload; exhausted threads treat further rays as unoccluded. See
 * vk_nuru_trace_override.glsl for the validation history. */
int g_ray_budget = 65536;

vec3 opaque_shadow_visibility(accelerationStructureEXT scene_as,
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
    rayQueryInitializeEXT(rq, scene_as, gl_RayFlagsOpaqueEXT, 0xFFu, origin, current_tmin, dir, t_max);
    int proceed_guard_rq = 4096;
    while (rayQueryProceedEXT(rq) && (proceed_guard_rq-- > 0)) {
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
      return vec3(1.0f);
    }
    const HardwareMaterialProxy proxy =
        material_proxies[uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true))];
    const uint proxy_flags = uint(proxy.ior_closure_type.w);
    if ((proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) == 0u) {
      return vec3(0.0f);
    }
    current_tmin = rayQueryGetIntersectionTEXT(rq, true) + 1.0e-4f;
    if (current_tmin >= t_max) {
      return vec3(1.0f);
    }
  }
  return vec3(0.0f);
}

vec3 hardware_shadow_apply_color_transmission(vec3 tint, float color_intensity)
{
  const float luma = dot(tint, vec3(0.2126f, 0.7152f, 0.0722f));
  return mix(vec3(luma), tint, clamp(color_intensity, 0.0f, 1.0f));
}

/* Nuru: Snell-bent caustic trace. Returns ONLY the caustic contribution (tinted throughput
 * along the refracted path, modulated by a tight alignment cone against the light's
 * direction at the exit, scaled by Photons intensity). Returns vec3(0) if the ray never
 * actually bent (no refractive surface hit) so it contributes nothing to non-glass paths. */
vec3 transparent_shadow_caustic_only(accelerationStructureEXT scene_as,
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
  vec3 throughput = vec3(1.0f);
  float current_tmin = t_min;
  float current_tmax = t_max;
  bool ray_was_bent = false;
  for (int b = 0; b < max_transparent_bounces; b++) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, scene_as, gl_RayFlagsOpaqueEXT, 0xFFu, origin, current_tmin, dir, current_tmax);
    int proceed_guard_rq = 4096;
    while (rayQueryProceedEXT(rq) && (proceed_guard_rq-- > 0)) {
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
      if (!ray_was_bent) {
        return vec3(0.0f);
      }
      const vec3 to_light = is_directional ? light_pos_or_dir
                                           : normalize(light_pos_or_dir - origin);
      const float align = clamp(dot(dir, to_light), 0.0f, 1.0f);
      /* Nuru: smooth cone falloff. `pow(align, 128)` was too binary -> every receiver pixel
       * either fired at full brightness or got nothing, producing speckled fireflies. The
       * smoothstep gives partial credit across a ~10 degree band so adjacent pixels'
       * contributions vary smoothly. Caustic outline is wider but visibly less noisy. */
      const float caustic_peak = photons_intensity *
                                 smoothstep(0.985f, 0.999f, align);
      return throughput * caustic_peak;
    }
    const float hit_distance = rayQueryGetIntersectionTEXT(rq, true);
    const uint hit_primitive_id = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    const vec2 hit_barycentric = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    const uint user_id = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    const HardwareMaterialProxy proxy = material_proxies[user_id];
    const uint proxy_flags = uint(proxy.ior_closure_type.w);
    if ((proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) != 0u) {
      current_tmin = hit_distance + 1.0e-4f;
      if (current_tmin >= current_tmax) {
        return ray_was_bent ? throughput : vec3(0.0f);
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
    const float caustic_roughness_threshold = 0.15f;
    const bool is_caustic_refraction = (closure_type == HWRT_CLOSURE_REFRACTION) &&
                                       (refraction_roughness < caustic_roughness_threshold);
    const bool is_alpha_blend = ((proxy_flags & HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT) != 0u);
    if (is_caustic_refraction) {
      throughput *= proxy.transmission_color_roughness.rgb;
    }
    else if (is_alpha_blend) {
      throughput *= vec3(clamp(1.0f - alpha, 0.0f, 1.0f));
    }
    else {
      /* Metal (any reflection closure), rough glass, diffuse, or any other surface
       * terminates the bent caustic ray. The tinted shadow path is independent. */
      return vec3(0.0f);
    }
    const float max_through = max(throughput.r, max(throughput.g, throughput.b));
    if (max_through < 1.0e-3f) {
      return vec3(0.0f);
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
      const vec3 N_smooth = (smooth_len_sq > 1.0e-10f) ? (smooth_N_raw * inversesqrt(smooth_len_sq))
                                                        : flat_N;
      const float cos_i_signed = dot(dir, N_smooth);
      const bool entering = (cos_i_signed < 0.0f);
      const vec3 N_oriented = entering ? N_smooth : -N_smooth;
      const float cos_i = -dot(dir, N_oriented);
      const float ior = max(proxy.ior_closure_type.y, 1.0f);
      const float eta = entering ? (1.0f / ior) : ior;
      const float sin2_t = eta * eta * (1.0f - cos_i * cos_i);
      if (sin2_t >= 1.0f) {
        /* Total internal reflection - no caustic path through this hit. */
        return vec3(0.0f);
      }
      const float cos_t = sqrt(1.0f - sin2_t);
      const vec3 refracted = normalize(eta * dir + (eta * cos_i - cos_t) * N_oriented);
      const vec3 hit_P = origin + dir * hit_distance;
      origin = hit_P;
      dir = refracted;
      current_tmin = 1.0e-3f;
      current_tmax = is_directional ? 100000.0f
                                    : max(length(light_pos_or_dir - origin), 1.0e-3f);
      ray_was_bent = true;
    }
    else {
      current_tmin = hit_distance + 1.0e-4f;
      if (current_tmin >= current_tmax) {
        if (!ray_was_bent) {
          return vec3(0.0f);
        }
        const vec3 to_light = is_directional ? light_pos_or_dir
                                             : normalize(light_pos_or_dir - origin);
        const float align = clamp(dot(dir, to_light), 0.0f, 1.0f);
        const float caustic_peak = photons_intensity *
                                   smoothstep(0.985f, 0.999f, align);
        return throughput * caustic_peak;
      }
    }
  }
  return vec3(0.0f);
}

/* Nuru: single shadow trace with opacity/transmission blend and Color Transmission tint.
 * transparency = 0 uses one any-hit opaque test. Otherwise one material-aware walk:
 * glass/alpha attenuates throughput, opaque surfaces block. With any hit along the ray the
 * opaque component is fully blocked, so mix(opaque, tinted, t) reduces to tinted * t. */
vec3 hardware_shadow_visibility(accelerationStructureEXT scene_as,
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
  const float t = clamp(transparency, 0.0f, 1.0f);
  if (t <= 0.0f) {
    return opaque_shadow_visibility(scene_as, origin, dir, t_min, t_max);
  }
  const int max_transparent_bounces = 8;
  vec3 throughput = vec3(1.0f);
  float current_tmin = t_min;
  bool any_hit = false;
  const float vis_scale = min(t, 1.0f);
  for (int b = 0; b < max_transparent_bounces; b++) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, scene_as, gl_RayFlagsOpaqueEXT, 0xFFu, origin, current_tmin, dir, t_max);
    int proceed_guard_rq = 4096;
    while (rayQueryProceedEXT(rq) && (proceed_guard_rq-- > 0)) {
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
      if (!any_hit) {
        return vec3(1.0f);
      }
      const vec3 tinted = hardware_shadow_apply_color_transmission(throughput, color_intensity);
      vec3 visibility = tinted * vis_scale;
      if (enable_caustics && vis_scale > 0.0f) {
        const vec3 caustic = transparent_shadow_caustic_only(
            scene_as, origin, dir, t_min, t_max,
            is_directional, light_pos_or_dir, photons_intensity);
        const float unoccluded = clamp(max(tinted.r, max(tinted.g, tinted.b)), 0.0f, 1.0f);
        visibility += caustic * clamp(1.0f - unoccluded, 0.0f, 1.0f) * vis_scale;
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
      current_tmin = hit_distance + 1.0e-4f;
      if (current_tmin >= t_max) {
        return any_hit ? hardware_shadow_apply_color_transmission(throughput, color_intensity) *
                             vis_scale :
                         vec3(1.0f);
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
      throughput *= vec3(clamp(1.0f - alpha, 0.0f, 1.0f));
    }
    else {
      return vec3(0.0f);
    }
    const float max_through = max(throughput.r, max(throughput.g, throughput.b));
    if (max_through < 1.0e-3f) {
      return vec3(0.0f);
    }
    current_tmin = hit_distance + 1.0e-4f;
    if (current_tmin >= t_max) {
      const vec3 tinted = hardware_shadow_apply_color_transmission(throughput, color_intensity);
      vec3 visibility = tinted * vis_scale;
      if (enable_caustics && vis_scale > 0.0f) {
        const vec3 caustic = transparent_shadow_caustic_only(
            scene_as, origin, dir, t_min, t_max,
            is_directional, light_pos_or_dir, photons_intensity);
        const float unoccluded = clamp(max(tinted.r, max(tinted.g, tinted.b)), 0.0f, 1.0f);
        visibility += caustic * clamp(1.0f - unoccluded, 0.0f, 1.0f) * vis_scale;
      }
      return visibility;
    }
  }
  return vec3(0.0f);
}

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
  if (any(isnan(P)) || any(isinf(P)) || any(isnan(N)) || any(isinf(N)) || dot(N, N) < 1.0e-10f) {
    return;
  }
  N = normalize(N);
  const vec3 shadow_N = hit_shadow_receiver_normal(tid, N, hit_identity_img);
  /* Full-precision hit positions do not need the primary raster receiver's 5mm bias.
   * Bound float-position error instead, and do not step past neighboring triangles. */
  const float position_scale = max(max(abs(P.x), abs(P.y)), abs(P.z));
  const float normal_bias = max(position_scale * 1.0e-6f, 1.0e-7f);
  const float ray_tmin = 0.0f;
  const int sample_count = (uniforms.shadow_params.x > 1.0e-6f) ? max(int(uniforms.shadow_params.y), 1) : 1;
  const bool enable_caustics = (uniforms.world_sun_slot_pad.y != 0);
  const float color_intensity = clamp(uniforms.shadow_params.z, 0.0f, 1.0f);
  const float photons_intensity = max(uniforms.shadow_params.w, 0.0f);
  const float transparent_shadows = uintBitsToFloat(uint(uniforms.world_sun_slot_pad.w));
  vec3 visibility = vec3(0.0f);
  for (int sample_index = 0; sample_index < sample_count; sample_index++) {
    const vec3 sample_dir = sample_directional_shadow_direction(tid, sample_index);
    const vec3 origin = P + shadow_N * normal_bias;
    visibility += hardware_shadow_visibility(scene, origin, sample_dir, ray_tmin, 100000.0f, transparent_shadows, enable_caustics, true, sample_dir, color_intensity, photons_intensity);
  }
  visibility /= float(sample_count);
  imageStore(shadow_visibility_img,
             ivec3(ivec2(tid), int(uint(uniforms.resolution_layer.z))),
             vec4(visibility, 1.0f));
}
