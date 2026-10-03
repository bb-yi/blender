#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_control_flow_attributes : enable
#extension GL_EXT_shader_image_load_formatted : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

const int GBUF_NONE = 0;
const int GBUF_DIFFUSE = 1;
const int GBUF_REFLECTION = 2;
const int GBUF_REFLECTION_COLORLESS = 3;
const int GBUF_REFRACTION = 8;
const int GBUF_REFRACTION_COLORLESS = 9;
const int GBUF_SUBSURFACE = 11;
const int GBUFFER_HEADER_BITS_PER_BIN = 4;
const uint FEATURE_HARDWARE_GI = 1u << 0u;
const uint FEATURE_HARDWARE_REFLECTIONS = 1u << 2u;
const uint FEATURE_HARDWARE_REFRACTIONS = 1u << 3u;
const uint HWRT_CLOSURE_DIFFUSE = 1u;
const uint HWRT_CLOSURE_REFLECTION = 7u;
const uint HWRT_CLOSURE_REFRACTION = 12u;
const uint HWRT_PROXY_FLAG_DIELECTRIC_REFLECTION = 1u << 0u;
const uint HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL = 1u << 1u;
const uint HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT = 1u << 2u;
const uint HWRT_PROXY_FLAG_PRINCIPLED_TRANSMISSION_LAYER = 1u << 3u;
const uint HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL = 1u << 4u;
const uint HWRT_PROXY_FLAG_METALLIC_BSDF_SCENE_FINAL = 1u << 6u;
const uint HWRT_PROXY_FLAG_THIN_GLASS = 1u << 7u;
const uint HWRT_HIT_IDENTITY_PRINCIPLED_LAYERED_SCENE_FINAL = 1u << 5u;
const uint HWRT_HIT_IDENTITY_METALLIC_BSDF_SCENE_FINAL = 1u << 6u;
const uint LIGHT_SUN = 0u;
const uint LIGHT_SUN_ORTHO = 1u;
const uint LIGHT_OMNI_SPHERE = 10u;
const uint LIGHT_OMNI_DISK = 11u;
const uint LIGHT_SPOT_SPHERE = 12u;
const uint LIGHT_SPOT_DISK = 13u;
const uint LIGHT_RECT = 20u;
const uint LIGHT_ELLIPSE = 21u;

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
struct EmissiveLightRecord {
  vec4 center_radius;
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
struct ThicknessData {
  float value;
  bool sphere_mode;
};

layout(scalar, binding = 0) uniform NuruUniforms {
  mat4 viewinv;
  mat4 wininv;
  ivec2 full_resolution;
  int resolution_scale;
  int resolution_scale_denominator;
  int closure_index;
  uint feature_mask;
  int hardware_trace_phase;
  int reflection_bounces;
  int refraction_bounces;
  int _pad0;
  ivec2 resolution_bias;
  float clamp_indirect;
  vec4 world_probe_atlas_coord;
  ivec4 use_environment_pad; /* x: specular/refraction world probe, y: emissive count,
                              * z: GI sample count, w: diffuse GI world probe. */
  ivec4 light_count_pad;
  vec4 sampling_rand;
} uniforms;

layout(binding = 1) uniform accelerationStructureEXT scene;

layout(scalar, binding = 2) readonly buffer B2 { vec4 emissive_radiance[]; };
layout(scalar, binding = 3) readonly buffer B3 { vec4 diffuse_albedo[]; };
layout(scalar, binding = 4) readonly buffer B4 { HardwareMaterialProxy material_proxy[]; };
layout(scalar, binding = 5) readonly buffer B5 { vec4 triangle_normals[]; };
layout(scalar, binding = 6) readonly buffer B6 { TriangleNormalRange triangle_normal_ranges[]; };
layout(scalar, binding = 7) readonly buffer B7 { uint tiles_coord_buf[]; };
layout(scalar, binding = 8) readonly buffer B8 { vec4 triangle_smooth_normals[]; };
layout(scalar, binding = 9) readonly buffer B9 { vec4 triangle_local_positions[]; };
layout(scalar, binding = 10) readonly buffer B10 { EmissiveLightRecord emissive_lights[]; };
layout(scalar, binding = 11) readonly buffer B11 { FastGILightRecord trace_lights[]; };

layout(binding = 16) uniform sampler2D ray_data_tx;
layout(binding = 17) uniform sampler2D depth_tx;
layout(binding = 18) uniform usampler2DArray gbuf_header_tx;
layout(binding = 19) uniform sampler2DArray gbuf_normal_tx;
layout(binding = 20) uniform sampler2D screen_continuation_img;
layout(binding = 80) uniform sampler2DArray world_probe_tx;

layout(binding = 45) uniform image2D ray_time_img;
layout(binding = 46) uniform image2D ray_radiance_img;
layout(binding = 47) uniform writeonly image2D hit_albedo_img;
layout(binding = 48) uniform writeonly image2D hit_material_img;
layout(binding = 49) uniform writeonly image2D hit_normal_img;
layout(binding = 50) uniform writeonly image2D hit_position_img;
layout(binding = 51) uniform writeonly uimage2D hit_identity_img;
layout(binding = 52) uniform writeonly image2D hit_barycentric_img;
layout(binding = 53) uniform writeonly image2D hit_world_position_img;
layout(binding = 54) uniform writeonly image2D hit_throughput_img;
layout(binding = 55) uniform writeonly image2D layered_receiver_ray_time_img;
layout(binding = 56) uniform writeonly image2D layered_receiver_ray_radiance_img;
layout(binding = 57) uniform writeonly image2D layered_receiver_albedo_img;
layout(binding = 58) uniform writeonly image2D layered_receiver_material_img;
layout(binding = 59) uniform writeonly image2D layered_receiver_normal_img;
layout(binding = 60) uniform writeonly image2D layered_receiver_position_img;
layout(binding = 61) uniform writeonly uimage2D layered_receiver_identity_img;
layout(binding = 62) uniform writeonly image2D layered_receiver_barycentric_img;
layout(binding = 63) uniform writeonly image2D layered_receiver_world_position_img;
layout(binding = 64) uniform writeonly image2D layered_receiver_throughput_img;
layout(binding = 65) uniform writeonly image2D transmission_receiver_ray_time_img;
layout(binding = 66) uniform writeonly image2D transmission_receiver_ray_radiance_img;
layout(binding = 67) uniform writeonly image2D transmission_receiver_albedo_img;
layout(binding = 68) uniform writeonly image2D transmission_receiver_material_img;
layout(binding = 69) uniform writeonly image2D transmission_receiver_normal_img;
layout(binding = 70) uniform writeonly image2D transmission_receiver_position_img;
layout(binding = 71) uniform writeonly uimage2D transmission_receiver_identity_img;
layout(binding = 72) uniform writeonly image2D transmission_receiver_barycentric_img;
layout(binding = 73) uniform writeonly image2D transmission_receiver_world_position_img;
layout(binding = 74) uniform writeonly image2D transmission_receiver_throughput_img;
layout(binding = 75) uniform writeonly image2D hit_emission_replay_img;
layout(binding = 76) uniform writeonly image2D layered_receiver_emission_replay_img;
layout(binding = 77) uniform writeonly image2D transmission_receiver_emission_replay_img;

uvec2 unpackUvec2x16(uint packed)
{
  return uvec2(packed & 0xFFFFu, packed >> 16u);
}
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
vec3 point_screen_to_world(vec2 uv, float depth)
{
  return point_screen_to_world(uv, depth, uniforms.wininv, uniforms.viewinv);
}

/* See `vk_nuru_directional_shadow.glsl` `view_ray_receiver_slack`: the depth-reconstructed
 * launch position carries error along the view ray bounded by the in-pixel surface depth
 * change. At grazing seam pixels the launch point can land outside its own surface and the GI
 * ray escapes into lit space (the "glowing seam line" leak through the GI signal). Pulling the
 * origin toward the camera is occlusion-safe by construction (the camera directly sees the
 * receiver). Only coplanar neighbor deltas count (genuine grazing recession); silhouette depth
 * gaps must not pull the origin or they rim edges with light. */
float view_ray_origin_slack(vec2 uv, float depth, vec3 P, vec3 view_dir, vec3 receiver_N)
{
  const vec2 texel_size = 1.0 / vec2(uniforms.full_resolution);
  const float pitch = length(
      point_screen_to_world(clamp(uv + vec2(texel_size.x, 0.0), vec2(0.0), vec2(1.0)), depth) - P);
  float slack = 0.0;
  const vec2 offsets[4] = vec2[4](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
  for (int i = 0; i < 4; i++) {
    const vec2 neighbor_uv = clamp(uv + offsets[i] * texel_size, vec2(0.0), vec2(1.0));
    const float neighbor_depth = 1.0 - textureLod(depth_tx, neighbor_uv, 0.0).r;
    if (neighbor_depth <= 0.0 || neighbor_depth >= 1.0) {
      continue;
    }
    const vec3 offset = point_screen_to_world(neighbor_uv, neighbor_depth) - P;
    const float offset_len = length(offset);
    if (!(offset_len > 1.0e-9)) {
      continue;
    }
    const float along_view = dot(offset, view_dir);
    if (along_view < 0.0) {
      /* Nearer neighbor: always occlusion-safe to pull by (see directional shadow kernel). */
      slack = max(slack, -along_view);
      continue;
    }
    const bool coplanar = abs(dot(offset, receiver_N)) <= 0.25 * offset_len;
    const bool near_range = offset_len <= 8.0 * max(pitch, 1.0e-9);
    if (!coplanar && !near_range) {
      continue;
    }
    slack = max(slack, along_view);
  }
  return slack;
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
uint gbuffer_bin_to_layer(uint header, uint bin_id)
{
  const uint type0 = (header >> (GBUFFER_HEADER_BITS_PER_BIN * 0u)) & 15u;
  const uint type1 = (header >> (GBUFFER_HEADER_BITS_PER_BIN * 1u)) & 15u;
  switch (bin_id) {
    case 2u:
      return uint(type0 != 0u) + uint(type1 != 0u);
    case 1u:
      return uint(type0 != 0u);
    default:
      return 0u;
  }
}
uint gbuffer_tangent_space_id(uint header, uint layer_id)
{
  if (layer_id == 0u) {
    return 0u;
  }
  return 3u & (header >> ((12u - 2u) + layer_id * 2u));
}
bool load_gbuffer_surface_normal(ivec2 texel,
                                 uint header,
                                 uint closure_index,
                                 sampler2DArray gbuf_normal_tx,
                                 out vec3 r_N)
{
  r_N = vec3(0.0);
  if (header == 0u) {
    return false;
  }
  const uint layer_id = gbuffer_bin_to_layer(header, closure_index);
  const uint normal_id = gbuffer_tangent_space_id(header, layer_id);
  const vec2 packed_N = texelFetch(gbuf_normal_tx, ivec3(texel, int(normal_id)), 0).xy;
  r_N = normal_unpack(packed_N);
  return (!any(isnan(r_N)) && !any(isinf(r_N))) && (dot(r_N, r_N) > 1.0e-10);
}
ThicknessData thickness_unpack(float thickness_packed)
{
  ThicknessData thickness;
  float value = (thickness_packed > 0.5) ? (1.0 - thickness_packed) : thickness_packed;
  value = value / max(1.0 - 2.0 * value, 1.0e-8);
  thickness.value = value;
  thickness.sphere_mode = (thickness_packed <= 0.5);
  return thickness;
}
vec3 thickness_intersection_offset(ThicknessData thickness, vec3 N, vec3 L)
{
  const float cos_alpha = dot(L, -N);
  if (!(cos_alpha > 1.0e-5)) {
    return vec3(0.0);
  }
  if (thickness.sphere_mode) {
    return L * (cos_alpha * thickness.value);
  }
  return L * (thickness.value / cos_alpha);
}
float hwrt_specular_ray_epsilon(bool thin_refraction)
{
  /* Thin glass shells in asset space can be smaller than the historical 1e-3 launch bias after
   * object scaling. Keep the larger guard for mirror-only traces, but let refraction traverse
   * real exit faces instead of stepping over them. */
  return thin_refraction ? 1.0e-5 : 1.0e-3;
}
float hwrt_specular_ray_tmin(bool thin_refraction)
{
  return thin_refraction ? 1.0e-5 : 5.0e-4;
}
float hwrt_gi_ray_epsilon(float reference_distance)
{
  /* Nuru: GI rays need a scale-relative epsilon. Fixed world-unit offsets become huge when an
   * asset is scaled down and can skip sealed wall blockers at edges. Keep this far below typical
   * wall thickness while still large enough to avoid exact self-intersection. */
  return clamp(max(reference_distance, 1.0e-4) * 1.25e-6, 1.0e-8, 2.0e-5);
}
float hwrt_gi_self_hit_distance(float epsilon)
{
  return max(epsilon * 16.0, 1.0e-7);
}
vec2 direction_pack(vec3 dir)
{
  const float dir_len_sq = dot(dir, dir);
  if (!(dir_len_sq > 1.0e-10)) {
    return vec2(0.5, 0.5);
  }
  dir *= inversesqrt(dir_len_sq);
  dir /= max(abs(dir.x) + abs(dir.y) + abs(dir.z), 1.0e-8);
  vec2 packed = dir.xy;
  if (dir.z < 0.0) {
    const vec2 sign_dir = vec2((packed.x >= 0.0) ? 1.0 : -1.0,
                               (packed.y >= 0.0) ? 1.0 : -1.0);
    packed = (1.0 - abs(vec2(packed.y, packed.x))) * sign_dir;
  }
  return packed * 0.5 + 0.5;
}
float hash12(vec2 p)
{
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}
vec2 rand2_trace(uvec2 tid, int sample_index, int layer)
{
  const vec2 seed = vec2(uniforms.sampling_rand.x * 23.47 + uniforms.sampling_rand.z * 11.13,
                          uniforms.sampling_rand.y * 29.59 + uniforms.sampling_rand.w * 7.71);
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
vec3 sample_cylinder(vec2 rand)
{
  return vec3(rand.x, sample_circle(rand.y));
}
vec3 ggx_sample_vndf(vec3 rand, vec3 Vt, float alpha)
{
  const vec3 Vh = normalize(vec3(alpha * Vt.xy, Vt.z));
  const float cos_theta = mix(-Vh.z, 1.0, rand.x);
  const float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
  const vec3 Lh = vec3(sin_theta * rand.yz, cos_theta);
  const vec3 Hh = Vh + Lh;
  return normalize(vec3(alpha * Hh.xy, max(0.0, Hh.z)));
}
void make_orthonormal_basis(vec3 n, out vec3 right, out vec3 up)
{
  const vec3 helper = (abs(n.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
  right = normalize(cross(helper, n));
  up = normalize(cross(n, right));
}
vec3 sample_trace_diffuse_direction(uvec2 tid,
                                    int sample_index,
                                    int layer,
                                    vec3 N)
{
  vec3 right, up;
  make_orthonormal_basis(N, right, up);
  const vec2 disk = sample_disk(rand2_trace(tid, sample_index, layer));
  const float z = sqrt(max(0.0, 1.0 - dot(disk, disk)));
  return normalize(right * disk.x + up * disk.y + N * z);
}
vec3 sample_rough_specular_direction(uvec2 tid,
                                     int sample_index,
                                     int layer,
                                     vec3 ray_direction,
                                     vec3 surface_N,
                                     float roughness,
                                     bool refract_mode,
                                     float eta)
{
  const float alpha = roughness * roughness;
  vec3 sharp_dir = refract_mode ? refract(ray_direction, surface_N, eta) :
                                  reflect(ray_direction, surface_N);
  if (refract_mode && !(dot(sharp_dir, sharp_dir) > 1.0e-10)) {
    sharp_dir = reflect(ray_direction, surface_N);
  }
  if (!(alpha > 4.0e-4)) {
    return sharp_dir;
  }
  vec3 right, up;
  make_orthonormal_basis(surface_N, right, up);
  const vec3 V = -ray_direction;
  const vec3 Vt = vec3(dot(V, right), dot(V, up), dot(V, surface_N));
  if (!(Vt.z > 1.0e-5)) {
    return sharp_dir;
  }
  const vec3 Ht = ggx_sample_vndf(sample_cylinder(rand2_trace(tid, sample_index, layer)), Vt, alpha);
  const vec3 H = normalize(right * Ht.x + up * Ht.y + surface_N * Ht.z);
  vec3 sampled_dir = refract_mode ? refract(ray_direction, H, eta) : reflect(ray_direction, H);
  if (refract_mode && !(dot(sampled_dir, sampled_dir) > 1.0e-10)) {
    sampled_dir = reflect(ray_direction, H);
  }
  return sampled_dir;
}
float dielectric_fresnel_reflectance(vec3 ray_direction, vec3 surface_N, float ior)
{
  const float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
  const float cos_theta = clamp(dot(-ray_direction, surface_N), 0.0, 1.0);
  const float f = pow(1.0 - cos_theta, 5.0);
  return clamp(f0 + (1.0 - f0) * f, 0.0, 1.0);
}
float projected_sphere_disk_radius(float sphere_radius, float distance_to_sphere)
{
  return sphere_radius * inversesqrt(max(1.0e-8, 1.0 - (sphere_radius * sphere_radius) / max(distance_to_sphere * distance_to_sphere, 1.0e-8)));
}
bool fast_gi_is_sun(uint type)
{
  return type <= LIGHT_SUN_ORTHO;
}
bool fast_gi_is_spot(uint type)
{
  return type == LIGHT_SPOT_SPHERE || type == LIGHT_SPOT_DISK;
}
bool fast_gi_is_area(uint type)
{
  return type >= LIGHT_RECT;
}
bool fast_gi_is_sphere(uint type)
{
  return type == LIGHT_OMNI_SPHERE || type == LIGHT_SPOT_SPHERE;
}
vec3 fast_gi_transform_location(FastGILightRecord light)
{
  return vec3(light.object_to_world_x.w, light.object_to_world_y.w, light.object_to_world_z.w);
}
vec3 fast_gi_transform_z_axis(FastGILightRecord light)
{
  return vec3(light.object_to_world_x.z, light.object_to_world_y.z, light.object_to_world_z.z);
}
vec3 fast_gi_transform_direction_transposed(FastGILightRecord light, vec3 direction)
{
  return mat3(vec3(light.object_to_world_x.x, light.object_to_world_x.y, light.object_to_world_x.z),
              vec3(light.object_to_world_y.x, light.object_to_world_y.y, light.object_to_world_y.z),
              vec3(light.object_to_world_z.x, light.object_to_world_z.y, light.object_to_world_z.z)) * direction;
}
float fast_gi_light_influence_attenuation(float dist, float inv_sqr_influence)
{
  const float factor = dist * dist * inv_sqr_influence;
  const float fac = clamp(1.0 - factor * factor, 0.0, 1.0);
  return fac * fac;
}
float fast_gi_light_spot_attenuation(FastGILightRecord light, vec3 L)
{
  const vec3 lL = fast_gi_transform_direction_transposed(light, L);
  if (!(lL.z > 0.0)) {
    return 0.0;
  }
  const float inv_z = 1.0 / max(lL.z, 1.0e-6);
  const vec2 scaled = lL.xy * light.spot_size_inv.xy * inv_z;
  const float ellipse = inversesqrt(1.0 + dot(scaled, scaled));
  return smoothstep(0.0, 1.0, ellipse * light.attenuation_spot.z + light.attenuation_spot.w);
}
float fast_gi_light_surface_attenuation(FastGILightRecord light, uint type, vec3 L, float dist)
{
  if (fast_gi_is_sun(type)) {
    return 1.0;
  }
  float attenuation = fast_gi_is_spot(type) ? fast_gi_light_spot_attenuation(light, L) : 1.0;
  attenuation *= fast_gi_light_influence_attenuation(dist, light.attenuation_spot.y);
  if (fast_gi_is_area(type)) {
    attenuation *= float(dot(L, fast_gi_transform_z_axis(light)) > 0.0);
  }
  return attenuation;
}
float fast_gi_light_point_power(FastGILightRecord light, uint type, float dist, vec3 L)
{
  if (fast_gi_is_sun(type)) {
    return 1.0;
  }
  /* Nuru: Lambert small-source solid-angle form; the previous 1/d^2-only form left Eevee's
   * 1/r^2 shape normalization uncancelled (~100x over-contribution for a default point light).
   * See the matching fix in vk_nuru_fast_gi.glsl / the Metal kernels. */
  float radius = max(light.attenuation_spot.x, 1.0e-4);
  if (fast_gi_is_sphere(type) && dist > 1.0e-5) {
    radius = projected_sphere_disk_radius(radius, dist);
  }
  const float x = clamp(radius / max(dist, 1.0e-5), 0.0, 1.0);
  float power = 2.0 * (1.0 - sqrt(max(1.0 - x * x, 0.0)));
  if (fast_gi_is_area(type)) {
    power *= clamp(dot(fast_gi_transform_z_axis(light), L), 0.0, 1.0);
  }
  return power;
}
vec2 octahedral_uv_from_direction(vec3 co)
{
  co /= max(dot(vec3(1.0), abs(co)), 1.0e-8);
  if (co.z < 0.0) {
    const vec2 sign_xy = vec2((co.x >= 0.0) ? 1.0 : -1.0,
                              (co.y >= 0.0) ? 1.0 : -1.0);
    co.xy = (1.0 - abs(co.yx)) * sign_xy;
  }
  return co.xy * 0.5 + 0.5;
}
vec3 sample_trace_world_radiance(sampler2DArray world_probe_tx,
                                 vec3 direction,
                                 bool use_environment)
{
  if (!use_environment) {
    return vec3(0.0);
  }
  const vec4 atlas_coord = uniforms.world_probe_atlas_coord;
  if (!(atlas_coord.z > 0.0) || !(atlas_coord.w >= 0.0)) {
    return vec3(0.0);
  }
  const vec3 sample_dir = normalize(direction);
  const vec2 octahedral_uv = octahedral_uv_from_direction(sample_dir);
  const float mip_0_res = max(atlas_coord.z * 4096.0, 1.0);
  const vec2 local_uv = octahedral_uv * ((mip_0_res - 2.0) / mip_0_res) + 0.5 / mip_0_res;
  const vec2 atlas_uv = local_uv * atlas_coord.z + atlas_coord.xy;
  return textureLod(world_probe_tx, vec3(atlas_uv, float(uint(max(int(atlas_coord.w), 0)))), 0.0).xyz;
}
vec4 sample_trace_emissive_direction(uvec2 tid,
                                     int sample_index,
                                     vec3 P)
{
  const int light_count = max(uniforms.use_environment_pad.y, 0);
  if (light_count <= 0) {
    return vec4(0.0);
  }
  const vec2 select_rand = rand2_trace(tid, sample_index, 101 + sample_index * 17);
  const int light_index = min(int(select_rand.x * float(light_count)), light_count - 1);
  const vec4 light = emissive_lights[light_index].center_radius;
  vec3 L = light.xyz - P;
  const float distance_to_light = length(L);
  if (!(distance_to_light > 1.0e-5)) {
    return vec4(0.0);
  }
  L /= distance_to_light;
  const float aperture = min(light.w / distance_to_light, 0.95);
  const float cos_theta_max = sqrt(max(1.0 - aperture * aperture, 0.0));
  const float cone_solid_angle = max(6.28318530718 * (1.0 - cos_theta_max), 1.0e-4);
  vec3 right, up;
  make_orthonormal_basis(L, right, up);
  const vec2 rand = rand2_trace(tid, sample_index, 173 + light_index * 23);
  const float cos_theta = mix(1.0, cos_theta_max, rand.x);
  const float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
  const float phi = 6.28318530718 * rand.y;
  const vec3 dir = normalize(L * cos_theta + right * (cos(phi) * sin_theta) + up * (sin(phi) * sin_theta));
  const float pdf = (1.0 / float(light_count)) / cone_solid_angle;
  return vec4(dir, pdf);
}
vec3 fast_gi_hit_normal(uint user_id,
                        uint primitive_id,
                        vec3 sample_dir)
{
  vec3 hit_normal = -sample_dir;
  const TriangleNormalRange normal_range = triangle_normal_ranges[user_id];
  if (primitive_id < normal_range.count) {
    hit_normal = triangle_normals[normal_range.offset + primitive_id].xyz;
  }
  const float len_sq = dot(hit_normal, hit_normal);
  if (!(len_sq > 1.0e-10)) {
    return -sample_dir;
  }
  hit_normal *= inversesqrt(len_sq);
  return (dot(hit_normal, sample_dir) < 0.0) ? hit_normal : -hit_normal;
}
/* Expansion of the MSL `intersector<triangle_data, instancing, max_levels<2>>` closest-hit
 * pattern (assume_geometry_type(triangle), force_opacity(opaque), closest hit). */
struct HitResult {
  bool hit;
  float dist;
  uint user_instance_id;
  uint primitive_id;
  vec2 triangle_barycentric_coord;
};
/* Per-thread ray budget. The legitimate worst case is a few hundred traversals per thread
 * (3 bounces x GI dome x light samples), so 64k can never alter results. It exists because the
 * NVIDIA shader compiler (595.71, RTX 5090) miscompiles this kernel without it: reflection-phase
 * dispatches spin until the GPU channel dies (Xid 109 CTX SWITCH TIMEOUT). The budget's
 * side-effecting counter perturbs codegen enough to avoid the bad transform, and verified
 * experiments show the wedge is gone even with a budget too large to ever trigger - i.e. this
 * guards against the compiler bug, not against real workloads. Keep until a driver update is
 * verified to compile this kernel correctly. */
int g_ray_budget = 65536;

HitResult trace_scene_closest(accelerationStructureEXT tlas,
                              vec3 origin,
                              vec3 direction,
                              float t_min,
                              float t_max)
{
  if (g_ray_budget-- <= 0) {
    HitResult exhausted;
    exhausted.hit = false;
    exhausted.dist = 0.0;
    exhausted.user_instance_id = 0u;
    exhausted.primitive_id = 0u;
    exhausted.triangle_barycentric_coord = vec2(0.0);
    return exhausted;
  }
  /* Non-finite or degenerate rays can wedge NVIDIA's traversal (Xid 109 channel timeout).
   * Metal's intersector returns a miss for them; match that contract before touching the BVH. */
  if (any(isnan(origin)) || any(isinf(origin)) || any(isnan(direction)) ||
      any(isinf(direction)) || dot(direction, direction) < 1.0e-12f || !(t_min <= t_max))
  {
    HitResult invalid;
    invalid.hit = false;
    invalid.dist = 0.0;
    invalid.user_instance_id = 0u;
    invalid.primitive_id = 0u;
    invalid.triangle_barycentric_coord = vec2(0.0);
    return invalid;
  }
  rayQueryEXT rq;
  rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, origin, t_min, direction, t_max);
  int proceed_guard_rq = 4096;
  while (rayQueryProceedEXT(rq) && (proceed_guard_rq-- > 0)) {
  }
  HitResult result;
  result.hit = (rayQueryGetIntersectionTypeEXT(rq, true) ==
                gl_RayQueryCommittedIntersectionTriangleEXT);
  if (result.hit) {
    result.dist = rayQueryGetIntersectionTEXT(rq, true);
    result.user_instance_id = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    result.primitive_id = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    result.triangle_barycentric_coord = rayQueryGetIntersectionBarycentricsEXT(rq, true);
  }
  else {
    result.dist = 0.0;
    result.user_instance_id = 0u;
    result.primitive_id = 0u;
    result.triangle_barycentric_coord = vec2(0.0);
  }
  return result;
}
vec3 sample_trace_direct_light(uvec2 tid,
                               int sample_index,
                               int sample_count,
                               vec3 P,
                               vec3 N,
                               uint source_user_id,
                               uint source_primitive_id,
                               bool trace_visibility,
                               bool include_locals,
                               accelerationStructureEXT scene)
{
  /* Nuru: direct translation of the Metal `sample_trace_direct_light` (the June 11 all-zero
   * Vulkan stub is retired for kernel parity with the Metal reference: mirror/GI-visible
   * diffuse surfaces were missing the in-kernel sun NEE that the Metal build applies at every
   * diffuse-gather hit). The transport contract is unchanged and calibrated on Metal:
   * - the primary gather is a DISJOINT split: hit lighting evaluates LOCAL lights analytically
   *   at recorded GI hits; this in-kernel NEE owns SUNS for the dome rays (adding locals here
   *   double-counts their bounce),
   * - `include_locals` (light-tree descent) exists only for the receiver-GI kernels, which are
   *   not ported to Vulkan yet; no Vulkan call site passes true. */
  const int light_count = max(uniforms.light_count_pad.x, 0);
  const int light_sample_count = min(max(uniforms.light_count_pad.y, 0), sample_count);
  if (light_count <= 0 || light_sample_count <= 0 || sample_index >= light_sample_count) {
    return vec3(0.0);
  }
  if (include_locals) {
    /* Receiver-GI light-tree descent: kernels not ported on Vulkan; unreachable today. */
    return vec3(0.0);
  }
  const int light_index = min(int(rand2_trace(tid,
                                              sample_index,
                                              211 + sample_index * 19).x * float(light_count)),
                              light_count - 1);
  const float select_weight = float(light_count);
  if (light_index < 0) {
    return vec3(0.0);
  }
  const FastGILightRecord light = trace_lights[light_index];
  const uint type = uint(light.direction_type.w + 0.5);
  if (!fast_gi_is_sun(type)) {
    /* Disjoint-split contract (see above): suns only on this path. */
    return vec3(0.0);
  }
  vec3 L = vec3(0.0, 0.0, 1.0);
  float light_distance = 100000.0;
  L = normalize(-light.direction_type.xyz);
  const float attenuation = fast_gi_light_surface_attenuation(light, type, L, light_distance);
  const float facing = clamp(dot(N, L), 0.0, 1.0);
  if (!(attenuation > 1.0e-6) || !(facing > 1.0e-4)) {
    return vec3(0.0);
  }
  if (trace_visibility) {
    const float visibility_origin_epsilon = hwrt_gi_ray_epsilon(light_distance);
    const float visibility_ray_tmin = visibility_origin_epsilon;
    const float visibility_self_hit_tmax = hwrt_gi_self_hit_distance(visibility_origin_epsilon);
    const vec3 origin = P + N * visibility_origin_epsilon;
    const float ray_tmax = 100000.0;
    HitResult occlusion = trace_scene_closest(scene, origin, L, visibility_ray_tmin, ray_tmax);
    if (occlusion.hit && occlusion.dist <= visibility_self_hit_tmax &&
        occlusion.user_instance_id == source_user_id &&
        occlusion.primitive_id == source_primitive_id)
    {
      /* Nuru: visibility to lights must be as tight as the diffuse gather itself. Ignore only
       * the exact source-triangle self hit; do not use a distance-scaled bias that can skip
       * sealed wall blockers. */
      const vec3 retry_origin = origin + L * (occlusion.dist + visibility_ray_tmin);
      occlusion = trace_scene_closest(scene, retry_origin, L, visibility_ray_tmin, ray_tmax);
    }
    if (occlusion.hit) {
      return vec3(0.0);
    }
  }
  const float direct_scale = float(sample_count) / float(light_sample_count);
  const float power = light.color_diffuse_power.w *
                      fast_gi_light_point_power(light, type, light_distance, L) * attenuation *
                      facing * select_weight * direct_scale;
  return light.color_diffuse_power.xyz * power;
}
void main()
{
  const uvec2 tile_coord = unpackUvec2x16(tiles_coord_buf[gl_WorkGroupID.x]);
  const uvec2 tid = gl_LocalInvocationID.xy + tile_coord * 8u;
  if (tid.x >= uint(textureSize(ray_data_tx, 0).x) || tid.y >= uint(textureSize(ray_data_tx, 0).y)) {
    return;
  }
  const vec4 packed_ray = texelFetch(ray_data_tx, ivec2(tid), 0);
  const float preserved_screen_time = imageLoad(ray_time_img, ivec2(tid)).x;
  const vec4 preserved_radiance = imageLoad(ray_radiance_img, ivec2(tid));
  const vec4 screen_continuation = texelFetch(screen_continuation_img, ivec2(tid), 0);
  imageStore(layered_receiver_ray_time_img, ivec2(tid), vec4(0.0));
  imageStore(layered_receiver_ray_radiance_img, ivec2(tid), vec4(0.0));
  imageStore(layered_receiver_albedo_img, ivec2(tid), vec4(0.0));
  imageStore(layered_receiver_material_img, ivec2(tid), vec4(0.0));
  imageStore(layered_receiver_normal_img, ivec2(tid), vec4(0.0));
  imageStore(layered_receiver_position_img, ivec2(tid), vec4(0.0));
  imageStore(layered_receiver_world_position_img, ivec2(tid), vec4(0.0));
  imageStore(layered_receiver_throughput_img, ivec2(tid), vec4(0.0));
  imageStore(layered_receiver_identity_img, ivec2(tid), uvec4(0u, 0u, 0u, 0xFFFFFFFFu));
  imageStore(layered_receiver_barycentric_img, ivec2(tid), vec4(0.0));
  imageStore(transmission_receiver_ray_time_img, ivec2(tid), vec4(0.0));
  imageStore(transmission_receiver_ray_radiance_img, ivec2(tid), vec4(0.0));
  imageStore(transmission_receiver_albedo_img, ivec2(tid), vec4(0.0));
  imageStore(transmission_receiver_material_img, ivec2(tid), vec4(0.0));
  imageStore(transmission_receiver_normal_img, ivec2(tid), vec4(0.0));
  imageStore(transmission_receiver_position_img, ivec2(tid), vec4(0.0));
  imageStore(transmission_receiver_world_position_img, ivec2(tid), vec4(0.0));
  imageStore(transmission_receiver_throughput_img, ivec2(tid), vec4(0.0));
  imageStore(transmission_receiver_identity_img, ivec2(tid), uvec4(0u, 0u, 0u, 0xFFFFFFFFu));
  imageStore(transmission_receiver_barycentric_img, ivec2(tid), vec4(0.0));
  if (packed_ray.w == 0.0) {
    imageStore(ray_time_img, ivec2(tid), vec4(-1.0, 0.0, 0.0, 0.0));
    imageStore(ray_radiance_img, ivec2(tid), preserved_radiance);
    imageStore(hit_albedo_img, ivec2(tid), vec4(0.0));
    imageStore(hit_material_img, ivec2(tid), vec4(0.0));
    imageStore(hit_normal_img, ivec2(tid), vec4(0.0));
    imageStore(hit_position_img, ivec2(tid), vec4(0.0));
    imageStore(hit_world_position_img, ivec2(tid), vec4(0.0));
    imageStore(hit_throughput_img, ivec2(tid), vec4(0.0));
    imageStore(hit_identity_img, ivec2(tid), uvec4(0u, 0u, 0u, 0xFFFFFFFFu));
    imageStore(hit_barycentric_img, ivec2(tid), vec4(0.0));
    return;
  }
  const int scale = max(uniforms.resolution_scale, 1);
  const int denominator = max(uniforms.resolution_scale_denominator, 1);
  const ivec2 cell_min = (ivec2(tid) * scale + ivec2(denominator - 1)) / denominator;
  const ivec2 cell_max = ((ivec2(tid) + ivec2(1)) * scale + ivec2(denominator - 1)) / denominator - ivec2(1);
  const ivec2 cell_extent = max(cell_max - cell_min + ivec2(1), ivec2(1));
  const ivec2 local_offset = min((max(uniforms.resolution_bias, ivec2(0)) * cell_extent) / scale, cell_extent - ivec2(1));
  const ivec2 texel_fullres = cell_min + local_offset;
  if (texel_fullres.x < 0 || texel_fullres.y < 0 || texel_fullres.x >= uniforms.full_resolution.x || texel_fullres.y >= uniforms.full_resolution.y) {
    imageStore(ray_time_img, ivec2(tid), vec4(-1.0, 0.0, 0.0, 0.0));
    imageStore(ray_radiance_img, ivec2(tid), preserved_radiance);
    imageStore(hit_albedo_img, ivec2(tid), vec4(0.0));
    imageStore(hit_material_img, ivec2(tid), vec4(0.0));
    imageStore(hit_normal_img, ivec2(tid), vec4(0.0));
    imageStore(hit_position_img, ivec2(tid), vec4(0.0));
    imageStore(hit_world_position_img, ivec2(tid), vec4(0.0));
    imageStore(hit_throughput_img, ivec2(tid), vec4(0.0));
    imageStore(hit_identity_img, ivec2(tid), uvec4(0u, 0u, 0u, 0xFFFFFFFFu));
    imageStore(hit_barycentric_img, ivec2(tid), vec4(0.0));
    return;
  }
  const uint gbuf_header = texelFetch(gbuf_header_tx, ivec3(texel_fullres, 0), 0).x;
  const uint gbuf_mode = (gbuf_header >> (uniforms.closure_index * GBUFFER_HEADER_BITS_PER_BIN)) & 15u;
  const bool supports_hardware_gi = ((uniforms.feature_mask & FEATURE_HARDWARE_GI) != 0u) && ((gbuf_mode == GBUF_DIFFUSE) || (gbuf_mode == GBUF_SUBSURFACE));
  const bool supports_hardware_reflection = ((uniforms.feature_mask & FEATURE_HARDWARE_REFLECTIONS) != 0u) && ((gbuf_mode == GBUF_REFLECTION) || (gbuf_mode == GBUF_REFLECTION_COLORLESS));
  const bool supports_hardware_refraction = ((uniforms.feature_mask & FEATURE_HARDWARE_REFRACTIONS) != 0u) && ((gbuf_mode == GBUF_REFRACTION) || (gbuf_mode == GBUF_REFRACTION_COLORLESS));
  const bool continuation_required = (supports_hardware_reflection && (uniforms.reflection_bounces > 1)) ||
                                   (supports_hardware_refraction && (uniforms.refraction_bounces > 1));
  const bool has_screen_continuation = screen_continuation.w > 0.0;
  const bool scene_final_specular_phase = (uniforms.hardware_trace_phase == 2);
  const bool preserved_screen_hit = !scene_final_specular_phase &&
                                    (supports_hardware_reflection || supports_hardware_refraction) &&
                                    (preserved_screen_time > 0.0) &&
                                    (preserved_screen_time < 10000.0);
  const bool use_preserved_screen_hit = preserved_screen_hit &&
                                        (!continuation_required || has_screen_continuation);
  if (!(supports_hardware_gi || supports_hardware_reflection || supports_hardware_refraction) || gbuf_mode == GBUF_NONE) {
    imageStore(ray_time_img, ivec2(tid), vec4(preserved_screen_time, 0.0, 0.0, 0.0));
    imageStore(ray_radiance_img, ivec2(tid), preserved_radiance);
    imageStore(hit_albedo_img, ivec2(tid), vec4(0.0));
    imageStore(hit_material_img, ivec2(tid), vec4(0.0));
    imageStore(hit_normal_img, ivec2(tid), vec4(0.0));
    imageStore(hit_position_img, ivec2(tid), vec4(0.0));
    imageStore(hit_world_position_img, ivec2(tid), vec4(0.0));
    imageStore(hit_throughput_img, ivec2(tid), vec4(0.0));
    /* Positive native/previous-phase ray time is not a hardware hit. Keep its radiance, but use
     * an invalid scene ID so sparse replay cannot shade scene entry zero over that result. */
    imageStore(hit_identity_img, ivec2(tid), uvec4(0xFFFFFFFFu, 0u, 0u, 0xFFFFFFFFu));
    imageStore(hit_barycentric_img, ivec2(tid), vec4(0.0));
    return;
  }
  const vec2 uv = (vec2(texel_fullres) + 0.5) / vec2(uniforms.full_resolution);
  const float depth = 1.0 - textureLod(depth_tx, uv, 0.0).r;
  if (depth <= 0.0 || depth >= 1.0) {
    imageStore(ray_time_img, ivec2(tid), vec4(-2.0, 0.0, 0.0, 0.0));
    imageStore(ray_radiance_img, ivec2(tid), preserved_radiance);
    imageStore(hit_albedo_img, ivec2(tid), vec4(0.0));
    imageStore(hit_material_img, ivec2(tid), vec4(0.0));
    imageStore(hit_normal_img, ivec2(tid), vec4(0.0));
    imageStore(hit_position_img, ivec2(tid), vec4(0.0));
    imageStore(hit_world_position_img, ivec2(tid), vec4(0.0));
    imageStore(hit_throughput_img, ivec2(tid), vec4(0.0));
    imageStore(hit_identity_img, ivec2(tid), uvec4(0u, 0u, 0u, 0xFFFFFFFFu));
    imageStore(hit_barycentric_img, ivec2(tid), vec4(0.0));
    return;
  }
  vec3 ray_direction = normalize(vec3(packed_ray.xyz));
  if (preserved_screen_hit && !continuation_required) {
    imageStore(ray_time_img, ivec2(tid), vec4(max(preserved_screen_time, 1.0e-4), 0.0, 0.0, 0.0));
    imageStore(ray_radiance_img, ivec2(tid), preserved_radiance);
    imageStore(hit_albedo_img, ivec2(tid), vec4(0.0));
    imageStore(hit_material_img, ivec2(tid), vec4(0.0));
    imageStore(hit_normal_img, ivec2(tid), vec4(0.0));
    imageStore(hit_position_img, ivec2(tid), vec4(0.0));
    imageStore(hit_world_position_img, ivec2(tid), vec4(0.0));
    imageStore(hit_throughput_img, ivec2(tid), vec4(0.0));
    imageStore(hit_identity_img, ivec2(tid), uvec4(0u, 0u, 0u, 0xFFFFFFFFu));
    imageStore(hit_barycentric_img, ivec2(tid), vec4(0.0));
    return;
  }
  vec3 ray_origin = point_screen_to_world(uv, depth);
  /* SAPPHIRE 12 seam-leak slack is a GI-launch-only correction (see its shadow/visibility kernel
   * siblings). Applying it to specular launches has no Metal counterpart: reflection/refraction
   * rays must start exactly on the surface. On curved refractive objects the per-pixel depth
   * gradient made the slack pull every launch toward the camera (centimetres near the limb), so
   * refracted rays entered the glass ahead of the real surface and clipped through to the
   * background in a flat band (the classroom glass-sphere "plate"/square artifact; absent on
   * Metal, which has no launch slack). */
  if (supports_hardware_gi) {
    vec3 launch_N;
    if (load_gbuffer_surface_normal(
            texel_fullres, gbuf_header, uint(uniforms.closure_index), gbuf_normal_tx, launch_N))
    {
      const vec3 camera_position = uniforms.viewinv[3].xyz;
      const vec3 to_receiver = ray_origin - camera_position;
      const float receiver_distance = max(length(to_receiver), 1.0e-6);
      const vec3 view_dir = to_receiver / receiver_distance;
      const float origin_slack = min(
          view_ray_origin_slack(uv, depth, ray_origin, view_dir, launch_N),
          0.5 * receiver_distance);
      ray_origin -= view_dir * origin_slack;
    }
  }
  int start_bounce = 0;
  if (use_preserved_screen_hit && continuation_required && has_screen_continuation) {
    ray_origin = screen_continuation.xyz;
    start_bounce = 1;
  }
  if (scene_final_specular_phase && !use_preserved_screen_hit && supports_hardware_reflection) {
    vec3 surface_N;
    if (load_gbuffer_surface_normal(
            texel_fullres, gbuf_header, uint(uniforms.closure_index), gbuf_normal_tx, surface_N))
    {
      /* Keep the late mirror/reflection launch epsilon small enough that enclosed receivers such
       * as nearby room walls are not skipped and replaced by the world/HDRI miss path. */
      ray_origin += surface_N * ((dot(surface_N, ray_direction) >= 0.0) ? 1.0e-3 : -1.0e-3);
    }
  }
  /* Full HWRT can already traverse the real back-face of the refractive object on the first
   * bounce. Do not analytically skip through thickness here or we will overrun nearby receivers
   * and distort the apparent IOR. */
  ray_origin += ray_direction * hwrt_specular_ray_epsilon(supports_hardware_refraction);
  int max_bounces = 1;
  if (supports_hardware_reflection) {
    max_bounces = max(uniforms.reflection_bounces, 1);
  }
  else if (supports_hardware_refraction) {
    max_bounces = max(uniforms.refraction_bounces, 1);
  }
  vec3 radiance = use_preserved_screen_hit ? preserved_radiance.xyz : vec3(0.0);
  vec3 throughput = vec3(1.0);
  vec4 final_emission_replay = vec4(0.0);
  vec4 preserved_emission_replay = vec4(0.0);
  vec4 transmission_emission_replay = vec4(0.0);
  float total_distance = (use_preserved_screen_hit && continuation_required && has_screen_continuation) ?
                             max(screen_continuation.w, 0.0) :
                             0.0;
  vec3 final_position = ray_origin;
  vec3 final_local_position = vec3(0.0);
  vec3 final_direction = ray_direction;
  float final_segment_distance = 0.0;
  vec3 final_normal = vec3(0.0);
  vec2 final_barycentric = vec2(0.0);
  vec3 carried_scene_final_throughput = vec3(1.0);
  bool apply_scene_final_throughput = false;
  vec3 preserved_output_throughput = vec3(1.0);
  uint final_user_id = 0u;
  uint final_primitive_id = 0u;
  uint final_front_facing = 1u;
  bool preserved_scene_final_reflective_hit = false;
  bool preserved_transparent_scene_final_hit = false;
  bool preserved_layered_scene_final_hit = false;
  vec3 preserved_position = vec3(0.0);
  vec3 preserved_local_position = vec3(0.0);
  vec3 preserved_direction = ray_direction;
  float preserved_total_distance = 0.0;
  float preserved_segment_distance = 0.0;
  vec3 preserved_normal = vec3(0.0);
  vec2 preserved_barycentric = vec2(0.0);
  uint preserved_user_id = 0u;
  uint preserved_primitive_id = 0u;
  uint preserved_front_facing = 1u;
  bool final_layered_principled_scene_final_hit = false;
  bool final_transparent_scene_final_hit = false;
  bool final_refracted_textured_receiver_hit = false;
  HardwareMaterialProxy preserved_proxy;
  preserved_proxy.reflection_color_roughness = vec4(0.0);
  preserved_proxy.transmission_color_roughness = vec4(0.0);
  preserved_proxy.ior_closure_type = vec4(0.0);
  preserved_proxy.packed_thickness = vec4(0.0);
  uint preserved_proxy_closure = 0u;
  bool layered_receiver_valid = false;
  vec3 layered_receiver_position = vec3(0.0);
  vec3 layered_receiver_local_position = vec3(0.0);
  vec3 layered_receiver_direction = ray_direction;
  float layered_receiver_total_distance = 0.0;
  float layered_receiver_segment_distance = 0.0;
  vec3 layered_receiver_normal = vec3(0.0);
  vec2 layered_receiver_barycentric = vec2(0.0);
  uint layered_receiver_user_id = 0u;
  uint layered_receiver_primitive_id = 0u;
  uint layered_receiver_front_facing = 1u;
  vec3 layered_receiver_carried_throughput = vec3(1.0);
  HardwareMaterialProxy layered_receiver_proxy;
  layered_receiver_proxy.reflection_color_roughness = vec4(0.0);
  layered_receiver_proxy.transmission_color_roughness = vec4(0.0);
  layered_receiver_proxy.ior_closure_type = vec4(0.0);
  layered_receiver_proxy.packed_thickness = vec4(0.0);
  uint layered_receiver_proxy_closure = 0u;
  vec3 layered_receiver_continued_radiance = vec3(0.0);
  bool transmission_receiver_valid = false;
  vec3 transmission_receiver_position = vec3(0.0);
  vec3 transmission_receiver_local_position = vec3(0.0);
  vec3 transmission_receiver_direction = ray_direction;
  float transmission_receiver_total_distance = 0.0;
  float transmission_receiver_segment_distance = 0.0;
  vec3 transmission_receiver_normal = vec3(0.0);
  vec2 transmission_receiver_barycentric = vec2(0.0);
  uint transmission_receiver_user_id = 0u;
  uint transmission_receiver_primitive_id = 0u;
  uint transmission_receiver_front_facing = 1u;
  vec3 transmission_receiver_carried_throughput = vec3(1.0);
  bool transmission_receiver_apply_throughput = false;
  bool transmission_receiver_direct_lit_reflective = false;
  bool transmission_receiver_lock_surface = false;
  HardwareMaterialProxy transmission_receiver_proxy;
  transmission_receiver_proxy.reflection_color_roughness = vec4(0.0);
  transmission_receiver_proxy.transmission_color_roughness = vec4(0.0);
  transmission_receiver_proxy.ior_closure_type = vec4(0.0);
  transmission_receiver_proxy.packed_thickness = vec4(0.0);
  uint transmission_receiver_proxy_closure = 0u;
  vec3 transmission_receiver_continued_radiance = vec3(0.0);
  const float ray_tmin = (scene_final_specular_phase && !use_preserved_screen_hit) ?
                         hwrt_specular_ray_tmin(supports_hardware_refraction) :
                         0.0;
  HardwareMaterialProxy final_proxy;
  final_proxy.reflection_color_roughness = vec4(0.0);
  final_proxy.transmission_color_roughness = vec4(0.0);
  final_proxy.ior_closure_type = vec4(0.0);
  final_proxy.packed_thickness = vec4(0.0);
  uint final_proxy_closure = 0u;
  int thin_glass_passthrough_count = 0;
  for (int bounce = start_bounce; bounce < max_bounces; bounce++) {
    HitResult intersection = trace_scene_closest(scene, ray_origin, ray_direction, ray_tmin, 10000.0);
    if (!intersection.hit) {
      const vec2 packed_direction = direction_pack(ray_direction);
      const bool has_specular_throughput =
          ((final_proxy_closure == HWRT_CLOSURE_REFLECTION) ||
           (final_proxy_closure == HWRT_CLOSURE_REFRACTION)) &&
          (dot(throughput, throughput) > 1.0e-10);
      const vec3 miss_proxy_color = (final_proxy_closure == HWRT_CLOSURE_REFLECTION) ?
                                     final_proxy.reflection_color_roughness.xyz :
                                     final_proxy.transmission_color_roughness.xyz;
      const vec3 miss_tint = has_specular_throughput ?
                                 clamp(throughput * miss_proxy_color,
                                       vec3(0.0),
                                       vec3(uniforms.clamp_indirect)) :
                                 vec3(0.0);
      const vec3 miss_origin = has_specular_throughput ? final_position : ray_origin;
      const vec3 miss_normal = has_specular_throughput ? final_normal : vec3(0.0);
      if (preserved_layered_scene_final_hit || preserved_scene_final_reflective_hit ||
          preserved_transparent_scene_final_hit) {
        break;
      }
      imageStore(ray_time_img, ivec2(tid), vec4(-3.0, 0.0, 0.0, 0.0));
      imageStore(ray_radiance_img, ivec2(tid), vec4(radiance, 0.0));
      imageStore(hit_albedo_img, ivec2(tid), vec4(miss_tint, 0.0));
      imageStore(hit_material_img, ivec2(tid), vec4(0.0, 0.0, float(final_proxy_closure), packed_direction.x));
      imageStore(hit_normal_img, ivec2(tid), vec4(miss_normal, packed_direction.y));
      imageStore(hit_position_img, ivec2(tid), vec4(miss_origin, total_distance));
      imageStore(hit_world_position_img, ivec2(tid), vec4(miss_origin, total_distance));
      imageStore(hit_throughput_img, ivec2(tid), vec4(0.0));
      imageStore(hit_identity_img, ivec2(tid), uvec4(0u, 0u, 0u, 0xFFFFFFFFu));
      imageStore(hit_barycentric_img, ivec2(tid), vec4(0.0));
      return;
    }
    const float hit_time = intersection.dist;
    total_distance += hit_time;
    final_position = ray_origin + ray_direction * hit_time;
    final_direction = ray_direction;
    final_segment_distance = hit_time;
    const uint user_id = intersection.user_instance_id;
    final_user_id = user_id;
    final_primitive_id = intersection.primitive_id;
    final_barycentric = intersection.triangle_barycentric_coord;
    final_emission_replay = vec4(throughput, use_preserved_screen_hit ? 0.0 : 1.0);
    if (!use_preserved_screen_hit) {
      radiance += throughput * min(emissive_radiance[user_id].xyz, vec3(uniforms.clamp_indirect));
    }
    final_proxy = material_proxy[user_id];
    vec3 raw_hit_normal = vec3(0.0);
    vec3 smooth_hit_normal = vec3(0.0);
    const TriangleNormalRange normal_range = triangle_normal_ranges[user_id];
    if (intersection.primitive_id < normal_range.count) {
      raw_hit_normal = triangle_normals[normal_range.offset + intersection.primitive_id].xyz;
      const uint smooth_offset = (normal_range.offset + intersection.primitive_id) * 3u;
      const vec3 bary = barycentric_expand(final_barycentric);
      final_local_position = triangle_local_positions[smooth_offset + 0u].xyz * bary.x +
                             triangle_local_positions[smooth_offset + 1u].xyz * bary.y +
                             triangle_local_positions[smooth_offset + 2u].xyz * bary.z;
      smooth_hit_normal = triangle_smooth_normals[smooth_offset + 0u].xyz * bary.x +
                          triangle_smooth_normals[smooth_offset + 1u].xyz * bary.y +
                          triangle_smooth_normals[smooth_offset + 2u].xyz * bary.z;
    }
    bool entering = true;
    vec3 hit_normal = smooth_hit_normal;
    if (!(dot(hit_normal, hit_normal) > 1.0e-10)) {
      hit_normal = raw_hit_normal;
    }
    if (!(dot(hit_normal, hit_normal) > 1.0e-10)) {
      final_normal = -ray_direction;
    }
    else {
      entering = dot(hit_normal, ray_direction) < 0.0;
      final_front_facing = entering ? 1u : 0u;
      final_normal = entering ? hit_normal : -hit_normal;
    }
    const uint proxy_closure = uint(final_proxy.ior_closure_type.z + 0.5);
    const uint proxy_flags = uint(final_proxy.ior_closure_type.w + 0.5);
    const float reflection_roughness = clamp(final_proxy.reflection_color_roughness.w, 0.0, 1.0);
    const float transmission_roughness = clamp(final_proxy.transmission_color_roughness.w, 0.0, 1.0);
    const float transparent_alpha = clamp(final_proxy.packed_thickness.y, 0.0, 1.0);
    const float refraction_ior = max(final_proxy.ior_closure_type.y, 1.0e-3);
    const float eta = entering ? (1.0 / refraction_ior) : refraction_ior;
    const bool resolving_preserved_scene_final_receiver =
        preserved_layered_scene_final_hit || preserved_scene_final_reflective_hit ||
        preserved_transparent_scene_final_hit;
    const bool supports_hardware_specular_receiver = supports_hardware_reflection ||
                                                     supports_hardware_refraction;
    const bool thin_glass_passthrough =
        ((proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) != 0u) &&
        (!scene_final_specular_phase || supports_hardware_gi || (bounce > start_bounce) ||
         resolving_preserved_scene_final_receiver);
    if (thin_glass_passthrough && (thin_glass_passthrough_count < 8)) {
      thin_glass_passthrough_count++;
      ray_origin = final_position + ray_direction * hwrt_specular_ray_epsilon(false);
      bounce -= 1;
      continue;
    }
    /* Alpha-cutout pass-through (Thin Glass contract for partial-coverage proxies): outside the
     * scene-final phase, an alpha-transparent hit must not be accepted and lit as a solid
     * surface. Recording it fed the cutout's support quad into diffuse-GI hit lighting as a
     * dark occluder (black blotches around spider webs / leaves on nearby walls, independent of
     * the HWRT shadow toggle). Attenuate by coverage and keep flying without consuming the
     * bounce, like the Thin Glass branch above. The scene-final phase keeps its dedicated
     * preserve_transparent_scene_final compositing below. */
    const bool alpha_cutout_passthrough =
        !scene_final_specular_phase &&
        ((proxy_flags & HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT) != 0u) &&
        (transparent_alpha < 1.0 - 1.0e-3);
    if (alpha_cutout_passthrough && (thin_glass_passthrough_count < 8)) {
      thin_glass_passthrough_count++;
      throughput *= max(vec3(1.0 - transparent_alpha), vec3(0.0));
      if (!(dot(throughput, throughput) > 1.0e-10)) {
        break;
      }
      ray_origin = final_position + ray_direction * hwrt_specular_ray_epsilon(false);
      bounce -= 1;
      continue;
    }
    const bool preserve_layered_principled_scene_final =
        scene_final_specular_phase && supports_hardware_specular_receiver &&
        !resolving_preserved_scene_final_receiver &&
        ((proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u) &&
        ((proxy_flags & HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT) == 0u);
    const bool preserve_textured_specular_scene_final =
        scene_final_specular_phase && supports_hardware_specular_receiver &&
        !preserved_layered_scene_final_hit && !preserved_scene_final_reflective_hit &&
        ((proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) &&
        ((proxy_flags & HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT) == 0u);
    const bool preserve_transparent_scene_final =
        scene_final_specular_phase && !preserved_transparent_scene_final_hit &&
        ((proxy_flags & HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT) != 0u) &&
        (transparent_alpha < 1.0 - 1.0e-3);
    const bool preserve_scene_final_transmission_layer =
        ((proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_TRANSMISSION_LAYER) != 0u) &&
        (preserve_layered_principled_scene_final || preserve_textured_specular_scene_final ||
         preserve_transparent_scene_final);
    const bool preserve_scene_final_transparent_layer = preserve_transparent_scene_final;
    const uint scene_final_proxy_carrier =
        (preserve_scene_final_transmission_layer || preserve_scene_final_transparent_layer) ?
                                                HWRT_CLOSURE_DIFFUSE :
                                                proxy_closure;
    uint resolved_proxy_closure = proxy_closure;
    if ((proxy_closure == HWRT_CLOSURE_REFRACTION) &&
        ((proxy_flags & HWRT_PROXY_FLAG_DIELECTRIC_REFLECTION) != 0u)) {
      const vec3 refracted = refract(ray_direction, final_normal, eta);
      const bool has_refraction = dot(refracted, refracted) > 1.0e-10;
      const float fresnel = dielectric_fresnel_reflectance(ray_direction, final_normal, refraction_ior);
      const float branch_rand = rand2_trace(
          tid,
          bounce + 1,
          uniforms.closure_index + int(HWRT_CLOSURE_REFLECTION + HWRT_CLOSURE_REFRACTION)).x;
      if (!has_refraction || (branch_rand < fresnel)) {
        resolved_proxy_closure = HWRT_CLOSURE_REFLECTION;
      }
    }
    if (preserve_textured_specular_scene_final &&
        ((proxy_flags & HWRT_PROXY_FLAG_DIELECTRIC_REFLECTION) != 0u)) {
      resolved_proxy_closure = HWRT_CLOSURE_REFLECTION;
    }
    const bool replay_textured_specular_receiver =
        scene_final_specular_phase && (bounce > start_bounce) &&
        (supports_hardware_reflection || supports_hardware_refraction) &&
        ((proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) &&
        ((proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) == 0u) &&
        ((proxy_flags & HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT) == 0u) &&
        ((resolved_proxy_closure == HWRT_CLOSURE_REFLECTION) ||
         (resolved_proxy_closure == HWRT_CLOSURE_REFRACTION));
    const bool preserved_material_scene_final =
        preserve_layered_principled_scene_final || preserve_textured_specular_scene_final;
    const bool layered_receiver_continuation = preserved_material_scene_final &&
                                             (scene_final_proxy_carrier == HWRT_CLOSURE_DIFFUSE) &&
                                             (reflection_roughness <= 1.0);
    const uint continuation_proxy_closure = layered_receiver_continuation ?
                                               HWRT_CLOSURE_REFLECTION :
                                               resolved_proxy_closure;
    const uint preserved_scene_final_proxy_closure = preserve_textured_specular_scene_final ?
                                                     resolved_proxy_closure :
                                                     scene_final_proxy_carrier;
    final_proxy_closure =
        (preserved_material_scene_final || preserve_scene_final_transmission_layer ||
         preserve_scene_final_transparent_layer) ?
            preserved_scene_final_proxy_closure :
            resolved_proxy_closure;
    if (preserved_material_scene_final) {
      preserved_layered_scene_final_hit = true;
      preserved_output_throughput = throughput;
      preserved_position = final_position;
      preserved_local_position = final_local_position;
      preserved_direction = final_direction;
      preserved_total_distance = total_distance;
      preserved_segment_distance = final_segment_distance;
      preserved_normal = final_normal;
      preserved_barycentric = final_barycentric;
      preserved_user_id = final_user_id;
      preserved_primitive_id = final_primitive_id;
      preserved_front_facing = final_front_facing;
      preserved_proxy = final_proxy;
      preserved_emission_replay = final_emission_replay;
      preserved_proxy_closure = final_proxy_closure;
      final_layered_principled_scene_final_hit = true;
    }
    if (preserve_transparent_scene_final) {
      preserved_transparent_scene_final_hit = true;
      preserved_position = final_position;
      preserved_local_position = final_local_position;
      preserved_direction = final_direction;
      preserved_total_distance = total_distance;
      preserved_segment_distance = final_segment_distance;
      preserved_normal = final_normal;
      preserved_barycentric = final_barycentric;
      preserved_user_id = final_user_id;
      preserved_primitive_id = final_primitive_id;
      preserved_front_facing = final_front_facing;
      preserved_proxy = final_proxy;
      preserved_emission_replay = final_emission_replay;
      preserved_proxy_closure = final_proxy_closure;
      preserved_output_throughput = throughput;
      final_transparent_scene_final_hit = true;
    }
    if (scene_final_specular_phase && supports_hardware_reflection && !preserved_material_scene_final && !preserve_transparent_scene_final && !preserve_scene_final_transmission_layer && !preserved_layered_scene_final_hit && !preserved_scene_final_reflective_hit &&
        (resolved_proxy_closure == HWRT_CLOSURE_REFLECTION)) {
      preserved_scene_final_reflective_hit = true;
      preserved_output_throughput = throughput;
      preserved_position = final_position;
      preserved_local_position = final_local_position;
      preserved_direction = final_direction;
      preserved_total_distance = total_distance;
      preserved_segment_distance = final_segment_distance;
      preserved_normal = final_normal;
      preserved_barycentric = final_barycentric;
      preserved_user_id = final_user_id;
      preserved_primitive_id = final_primitive_id;
      preserved_front_facing = final_front_facing;
      preserved_proxy = final_proxy;
      preserved_emission_replay = final_emission_replay;
      preserved_proxy_closure = final_proxy_closure;
    }
    /* The scene-final reflective early-out is only valid for the single-bounce shortcut. Once the
     * user requests deeper reflection continuation, do not clamp the late path back to the first
     * reflective secondary or nested glossy reflections disappear. */
    if (scene_final_specular_phase && supports_hardware_reflection && !continuation_required && !preserved_material_scene_final && !preserve_transparent_scene_final && !preserve_scene_final_transmission_layer &&
        (bounce == start_bounce) && (resolved_proxy_closure == HWRT_CLOSURE_REFLECTION)) {
      break;
    }
    const bool next_hit_is_specular_continuation =
        ((resolved_proxy_closure == HWRT_CLOSURE_REFLECTION) ||
         (resolved_proxy_closure == HWRT_CLOSURE_REFRACTION)) &&
        (bounce + 1 < max_bounces);
    if (preserved_scene_final_reflective_hit && next_hit_is_specular_continuation) {
      preserved_scene_final_reflective_hit = false;
    }
    if (resolving_preserved_scene_final_receiver && (bounce > start_bounce)) {
      layered_receiver_valid = true;
      layered_receiver_position = final_position;
      layered_receiver_local_position = final_local_position;
      layered_receiver_direction = final_direction;
      layered_receiver_total_distance = total_distance;
      layered_receiver_segment_distance = final_segment_distance;
      layered_receiver_normal = final_normal;
      layered_receiver_barycentric = final_barycentric;
      layered_receiver_user_id = final_user_id;
      layered_receiver_primitive_id = final_primitive_id;
      layered_receiver_front_facing = final_front_facing;
      layered_receiver_proxy = final_proxy;
      layered_receiver_proxy_closure = resolved_proxy_closure;
      layered_receiver_carried_throughput = throughput;
    }
    if (preserve_transparent_scene_final) {
      throughput *= max(vec3(1.0 - transparent_alpha), vec3(0.0));
      if (!(dot(throughput, throughput) > 1.0e-10)) {
        break;
      }
      ray_origin = final_position + ray_direction * hwrt_specular_ray_epsilon(supports_hardware_refraction);
      continue;
    }
    const bool can_continue = (bounce + 1 < max_bounces) &&
                              ((continuation_proxy_closure == HWRT_CLOSURE_REFLECTION) ||
                               (continuation_proxy_closure == HWRT_CLOSURE_REFRACTION));
    if (replay_textured_specular_receiver && !can_continue) {
      final_refracted_textured_receiver_hit = true;
    }
    if (!can_continue) {
      break;
    }
    vec3 next_direction = ray_direction;
    const bool skip_textured_proxy_throughput_tint =
        scene_final_specular_phase &&
        ((proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) &&
        ((proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) == 0u);
    if (continuation_proxy_closure == HWRT_CLOSURE_REFLECTION) {
      const vec3 reflection_tint = clamp(final_proxy.reflection_color_roughness.xyz,
                                         vec3(0.0),
                                         vec3(uniforms.clamp_indirect));
      if (!skip_textured_proxy_throughput_tint) {
        throughput *= reflection_tint;
      }
      if ((proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) != 0u) {
        const float thin_glass_ior = max(final_proxy.ior_closure_type.x, 1.0e-3);
        throughput *= dielectric_fresnel_reflectance(ray_direction, final_normal, thin_glass_ior);
      }
      next_direction = sample_rough_specular_direction(
          tid,
          bounce + 1,
          uniforms.closure_index + int(HWRT_CLOSURE_REFLECTION),
          ray_direction,
          final_normal,
          reflection_roughness,
          false,
          1.0);
    }
    else {
      if (!skip_textured_proxy_throughput_tint) {
        throughput *= clamp(final_proxy.transmission_color_roughness.xyz,
                             vec3(0.0),
                             vec3(uniforms.clamp_indirect));
      }
      next_direction = sample_rough_specular_direction(
          tid,
          bounce + 1,
          uniforms.closure_index + int(HWRT_CLOSURE_REFRACTION),
          ray_direction,
          final_normal,
          transmission_roughness,
          true,
          eta);
    }
    if (scene_final_specular_phase) {
      carried_scene_final_throughput = clamp(
          throughput, vec3(0.0), vec3(uniforms.clamp_indirect));
      apply_scene_final_throughput = true;
    }
    if (!(dot(next_direction, next_direction) > 1.0e-10)) {
      break;
    }
    ray_direction = normalize(next_direction);
    ray_origin = final_position + ray_direction *
                 hwrt_specular_ray_epsilon(continuation_proxy_closure == HWRT_CLOSURE_REFRACTION);
    if ((continuation_proxy_closure == HWRT_CLOSURE_REFRACTION) && entering) {
      const ThicknessData proxy_thickness = thickness_unpack(final_proxy.packed_thickness.x);
      if (proxy_thickness.value > 0.0) {
        const vec3 thickness_offset = thickness_intersection_offset(proxy_thickness, final_normal, ray_direction);
        const float thickness_distance = length(thickness_offset);
        if (thickness_distance > 1.0e-4) {
          HitResult thickness_intersection =
              trace_scene_closest(scene,
                                  ray_origin,
                                  ray_direction,
                                  hwrt_specular_ray_tmin(true),
                                  thickness_distance);
          if (!thickness_intersection.hit) {
            ray_origin += thickness_offset;
            total_distance += thickness_distance;
          }
        }
      }
    }
  }
  if (preserved_layered_scene_final_hit || preserved_transparent_scene_final_hit) {
    const uint preserved_proxy_flags = uint(preserved_proxy.ior_closure_type.w + 0.5);
    const bool preserved_has_transmission_layer =
        ((preserved_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_TRANSMISSION_LAYER) != 0u);
    if (preserved_has_transmission_layer) {
      const bool preserved_transmission_needs_real_exit_hit =
          ((preserved_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) ||
          ((preserved_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u);
      const float preserved_transmission_roughness =
          clamp(preserved_proxy.transmission_color_roughness.w, 0.0, 1.0);
      const float preserved_refraction_ior = max(preserved_proxy.ior_closure_type.y, 1.0e-3);
      const bool preserved_entering = (preserved_front_facing != 0u);
      const float preserved_eta = preserved_entering ? (1.0 / preserved_refraction_ior) :
                                                     preserved_refraction_ior;
      vec3 transmission_ray_direction = sample_rough_specular_direction(
          tid,
          start_bounce + 1,
          uniforms.closure_index + int(HWRT_CLOSURE_REFRACTION),
          preserved_direction,
          preserved_normal,
          preserved_transmission_roughness,
          true,
          preserved_eta);
      if (dot(transmission_ray_direction, transmission_ray_direction) > 1.0e-10) {
        vec3 transmission_ray_origin = preserved_position +
                                        normalize(transmission_ray_direction) *
                                            hwrt_specular_ray_epsilon(true);
        transmission_ray_direction = normalize(transmission_ray_direction);
        float transmission_total_distance = preserved_total_distance;
        vec3 transmission_throughput = vec3(1.0);
        if (preserved_entering && !preserved_transmission_needs_real_exit_hit) {
          const ThicknessData preserved_thickness = thickness_unpack(preserved_proxy.packed_thickness.x);
          if (preserved_thickness.value > 0.0) {
            const vec3 thickness_offset = thickness_intersection_offset(
                preserved_thickness, preserved_normal, transmission_ray_direction);
            const float thickness_distance = length(thickness_offset);
            if (thickness_distance > 1.0e-4) {
              HitResult thickness_intersection =
                  trace_scene_closest(scene,
                                      transmission_ray_origin,
                                      transmission_ray_direction,
                                      hwrt_specular_ray_tmin(true),
                                      thickness_distance);
              if (!thickness_intersection.hit) {
                transmission_ray_origin += thickness_offset;
                transmission_total_distance += thickness_distance;
              }
            }
          }
        }
        const int transmission_max_bounces = max(uniforms.refraction_bounces, 1);
        int transmission_thin_glass_passthrough_count = 0;
        for (int transmission_bounce = start_bounce + 1;
             transmission_bounce < transmission_max_bounces;
             transmission_bounce++) {
          HitResult transmission_intersection =
              trace_scene_closest(scene,
                                  transmission_ray_origin,
                                  transmission_ray_direction,
                                  hwrt_specular_ray_tmin(true),
                                  10000.0);
          if (!transmission_intersection.hit) {
            if (transmission_receiver_valid) {
              /* Dome-occlusion contract: the kernel has no access to the traced environment
               * visibility buffer, so it must never inject unmasked world radiance. An
               * unattenuated env add here floods sealed interiors with sky through every
               * refractive surface. World lighting for transmission receivers is owned by the
               * lighting pass, which attenuates by the cross-filtered dome visibility. */
              break;
            }
            transmission_receiver_valid = true;
            transmission_receiver_position = transmission_ray_origin;
            transmission_receiver_local_position = vec3(0.0);
            transmission_receiver_direction = transmission_ray_direction;
            transmission_receiver_total_distance = transmission_total_distance;
            transmission_receiver_segment_distance = 0.0;
            transmission_receiver_normal = -transmission_ray_direction;
            transmission_receiver_barycentric = vec2(0.0);
            transmission_receiver_user_id = 0u;
            transmission_receiver_primitive_id = 0u;
            transmission_receiver_front_facing = 1u;
            transmission_receiver_proxy = preserved_proxy;
            transmission_receiver_proxy_closure = HWRT_CLOSURE_REFRACTION;
            transmission_receiver_carried_throughput = transmission_throughput;
            transmission_receiver_apply_throughput = true;
            transmission_receiver_direct_lit_reflective = false;
            break;
          }
          const float transmission_hit_time = transmission_intersection.dist;
          transmission_total_distance += transmission_hit_time;
          const vec3 transmission_position = transmission_ray_origin +
                                              transmission_ray_direction * transmission_hit_time;
          const uint transmission_user_id = transmission_intersection.user_instance_id;
          const uint transmission_primitive_id = transmission_intersection.primitive_id;
          const vec2 transmission_bary = transmission_intersection.triangle_barycentric_coord;
          const vec4 transmission_hit_emission_replay = vec4(
              transmission_throughput, transmission_receiver_valid ? 1.0 : 0.0);
          if (transmission_receiver_valid) {
            transmission_receiver_continued_radiance += transmission_throughput *
                min(emissive_radiance[transmission_user_id].xyz, vec3(uniforms.clamp_indirect));
          }
          HardwareMaterialProxy transmission_proxy = material_proxy[transmission_user_id];
          vec3 transmission_raw_hit_normal = vec3(0.0);
          vec3 transmission_smooth_hit_normal = vec3(0.0);
          vec3 transmission_local_position = vec3(0.0);
          const TriangleNormalRange transmission_normal_range =
              triangle_normal_ranges[transmission_user_id];
          if (transmission_primitive_id < transmission_normal_range.count) {
            transmission_raw_hit_normal =
                triangle_normals[transmission_normal_range.offset + transmission_primitive_id].xyz;
            const uint transmission_smooth_offset =
                (transmission_normal_range.offset + transmission_primitive_id) * 3u;
            const vec3 transmission_bary3 = barycentric_expand(transmission_bary);
            transmission_local_position =
                triangle_local_positions[transmission_smooth_offset + 0u].xyz * transmission_bary3.x +
                triangle_local_positions[transmission_smooth_offset + 1u].xyz * transmission_bary3.y +
                triangle_local_positions[transmission_smooth_offset + 2u].xyz * transmission_bary3.z;
            transmission_smooth_hit_normal =
                triangle_smooth_normals[transmission_smooth_offset + 0u].xyz * transmission_bary3.x +
                triangle_smooth_normals[transmission_smooth_offset + 1u].xyz * transmission_bary3.y +
                triangle_smooth_normals[transmission_smooth_offset + 2u].xyz * transmission_bary3.z;
          }
          bool transmission_entering = true;
          vec3 transmission_hit_normal = transmission_smooth_hit_normal;
          if (!(dot(transmission_hit_normal, transmission_hit_normal) > 1.0e-10)) {
            transmission_hit_normal = transmission_raw_hit_normal;
          }
          uint transmission_front_facing = 1u;
          vec3 transmission_normal = -transmission_ray_direction;
          if (dot(transmission_hit_normal, transmission_hit_normal) > 1.0e-10) {
            transmission_entering = dot(transmission_hit_normal, transmission_ray_direction) < 0.0;
            transmission_front_facing = transmission_entering ? 1u : 0u;
            transmission_normal = transmission_entering ? transmission_hit_normal :
                                                        -transmission_hit_normal;
          }
          const uint transmission_proxy_closure = uint(transmission_proxy.ior_closure_type.z + 0.5);
          const uint transmission_proxy_flags = uint(transmission_proxy.ior_closure_type.w + 0.5);
          const float transmission_reflection_roughness =
              clamp(transmission_proxy.reflection_color_roughness.w, 0.0, 1.0);
          const float transmission_refraction_roughness =
              clamp(transmission_proxy.transmission_color_roughness.w, 0.0, 1.0);
          const float transmission_proxy_ior =
              max(transmission_proxy.ior_closure_type.y, 1.0e-3);
          const float transmission_eta = transmission_entering ? (1.0 / transmission_proxy_ior) :
                                                              transmission_proxy_ior;
          if (((transmission_proxy_flags & HWRT_PROXY_FLAG_THIN_GLASS) != 0u) &&
              (transmission_thin_glass_passthrough_count < 8)) {
            transmission_thin_glass_passthrough_count++;
            transmission_ray_origin = transmission_position + transmission_ray_direction *
                                      hwrt_specular_ray_epsilon(false);
            transmission_bounce -= 1;
            continue;
          }
          uint transmission_resolved_proxy_closure = transmission_proxy_closure;
          if ((transmission_proxy_closure == HWRT_CLOSURE_REFRACTION) &&
              ((transmission_proxy_flags & HWRT_PROXY_FLAG_DIELECTRIC_REFLECTION) != 0u)) {
            const vec3 refracted = refract(
                transmission_ray_direction, transmission_normal, transmission_eta);
            const bool has_refraction = dot(refracted, refracted) > 1.0e-10;
            const float fresnel = dielectric_fresnel_reflectance(
                transmission_ray_direction, transmission_normal, transmission_proxy_ior);
            const float branch_rand = rand2_trace(
                tid,
                transmission_bounce + 1,
                uniforms.closure_index + int(HWRT_CLOSURE_REFLECTION + HWRT_CLOSURE_REFRACTION)).x;
            if (!has_refraction || (branch_rand < fresnel)) {
              transmission_resolved_proxy_closure = HWRT_CLOSURE_REFLECTION;
            }
          }
          const bool transmission_replay_reflective_receiver =
              ((transmission_proxy_flags & HWRT_PROXY_FLAG_ALPHA_BLEND_TRANSPARENT) == 0u) &&
              (transmission_resolved_proxy_closure == HWRT_CLOSURE_REFLECTION);
          const bool transmission_replay_textured_reflective_receiver =
              transmission_replay_reflective_receiver &&
              (((transmission_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) ||
               ((transmission_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u));
          const bool transmission_replay_textured_refractive_receiver =
              !transmission_entering &&
              (transmission_resolved_proxy_closure == HWRT_CLOSURE_REFRACTION) &&
              (((transmission_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) ||
               ((transmission_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u));
          const bool transmission_can_continue =
              (transmission_bounce + 1 < transmission_max_bounces) &&
              ((transmission_resolved_proxy_closure == HWRT_CLOSURE_REFLECTION) ||
               (transmission_resolved_proxy_closure == HWRT_CLOSURE_REFRACTION));
          if ((transmission_replay_reflective_receiver ||
               transmission_replay_textured_refractive_receiver) &&
              !transmission_receiver_valid) {
            transmission_receiver_valid = true;
            transmission_receiver_position = transmission_position;
            transmission_receiver_local_position = transmission_local_position;
            transmission_receiver_direction = transmission_ray_direction;
            transmission_receiver_total_distance = transmission_total_distance;
            transmission_receiver_segment_distance = transmission_hit_time;
            transmission_receiver_normal = transmission_normal;
            transmission_receiver_barycentric = transmission_bary;
            transmission_receiver_user_id = transmission_user_id;
            transmission_receiver_primitive_id = transmission_primitive_id;
            transmission_receiver_front_facing = transmission_front_facing;
            transmission_receiver_proxy = transmission_proxy;
            transmission_emission_replay = transmission_hit_emission_replay;
            transmission_receiver_proxy_closure = transmission_resolved_proxy_closure;
            transmission_receiver_carried_throughput = transmission_throughput;
            transmission_receiver_direct_lit_reflective = transmission_replay_reflective_receiver;
            transmission_receiver_lock_surface = transmission_replay_textured_reflective_receiver ||
                                                 transmission_replay_textured_refractive_receiver;
          }
          if (!transmission_can_continue) {
            if (transmission_receiver_valid) {
              if (transmission_receiver_lock_surface) {
                break;
              }
              transmission_receiver_position = transmission_position;
              transmission_receiver_local_position = transmission_local_position;
              transmission_receiver_direction = transmission_ray_direction;
              transmission_receiver_total_distance = transmission_total_distance;
              transmission_receiver_segment_distance = transmission_hit_time;
              transmission_receiver_normal = transmission_normal;
              transmission_receiver_barycentric = transmission_bary;
              transmission_receiver_user_id = transmission_user_id;
              transmission_receiver_primitive_id = transmission_primitive_id;
              transmission_receiver_front_facing = transmission_front_facing;
              transmission_receiver_proxy = transmission_proxy;
              transmission_emission_replay = transmission_hit_emission_replay;
              transmission_receiver_proxy_closure = transmission_resolved_proxy_closure;
              transmission_receiver_carried_throughput = transmission_throughput;
              transmission_receiver_direct_lit_reflective = transmission_replay_reflective_receiver;
              break;
            }
            transmission_receiver_valid = true;
            transmission_receiver_position = transmission_position;
            transmission_receiver_local_position = transmission_local_position;
            transmission_receiver_direction = transmission_ray_direction;
            transmission_receiver_total_distance = transmission_total_distance;
            transmission_receiver_segment_distance = transmission_hit_time;
            transmission_receiver_normal = transmission_normal;
            transmission_receiver_barycentric = transmission_bary;
            transmission_receiver_user_id = transmission_user_id;
            transmission_receiver_primitive_id = transmission_primitive_id;
            transmission_receiver_front_facing = transmission_front_facing;
            transmission_receiver_proxy = transmission_proxy;
            transmission_emission_replay = transmission_hit_emission_replay;
            transmission_receiver_proxy_closure = transmission_resolved_proxy_closure;
            transmission_receiver_carried_throughput = transmission_throughput;
            transmission_receiver_direct_lit_reflective = transmission_replay_reflective_receiver;
            break;
          }
          vec3 transmission_next_direction = transmission_ray_direction;
          if (transmission_resolved_proxy_closure == HWRT_CLOSURE_REFLECTION) {
            if (!transmission_replay_reflective_receiver) {
              transmission_throughput *= clamp(
                  transmission_proxy.reflection_color_roughness.xyz,
                  vec3(0.0),
                  vec3(uniforms.clamp_indirect));
            }
            transmission_receiver_apply_throughput = true;
            transmission_next_direction = sample_rough_specular_direction(
                tid,
                transmission_bounce + 1,
                uniforms.closure_index + int(HWRT_CLOSURE_REFLECTION),
                transmission_ray_direction,
                transmission_normal,
                transmission_reflection_roughness,
                false,
                1.0);
          }
          else {
            transmission_throughput *= clamp(
                transmission_proxy.transmission_color_roughness.xyz,
                vec3(0.0),
                vec3(uniforms.clamp_indirect));
            transmission_receiver_apply_throughput = true;
            transmission_next_direction = sample_rough_specular_direction(
                tid,
                transmission_bounce + 1,
                uniforms.closure_index + int(HWRT_CLOSURE_REFRACTION),
                transmission_ray_direction,
                transmission_normal,
                transmission_refraction_roughness,
                true,
                transmission_eta);
          }
          if (!(dot(transmission_next_direction, transmission_next_direction) > 1.0e-10)) {
            break;
          }
          transmission_ray_direction = normalize(transmission_next_direction);
          transmission_ray_origin = transmission_position + transmission_ray_direction *
              hwrt_specular_ray_epsilon(transmission_resolved_proxy_closure == HWRT_CLOSURE_REFRACTION);
          const bool transmission_needs_real_exit_hit =
              ((transmission_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) ||
              ((transmission_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u);
          if ((transmission_resolved_proxy_closure == HWRT_CLOSURE_REFRACTION) &&
              transmission_entering && !transmission_needs_real_exit_hit) {
            const ThicknessData transmission_thickness = thickness_unpack(
                transmission_proxy.packed_thickness.x);
            if (transmission_thickness.value > 0.0) {
              const vec3 thickness_offset = thickness_intersection_offset(
                  transmission_thickness, transmission_normal, transmission_ray_direction);
              const float thickness_distance = length(thickness_offset);
              if (thickness_distance > 1.0e-4) {
                HitResult thickness_intersection =
                    trace_scene_closest(scene,
                                        transmission_ray_origin,
                                        transmission_ray_direction,
                                        hwrt_specular_ray_tmin(true),
                                        thickness_distance);
                if (!thickness_intersection.hit) {
                  transmission_ray_origin += thickness_offset;
                  transmission_total_distance += thickness_distance;
                }
              }
            }
          }
        }
      }
    }
  }
  if (preserved_layered_scene_final_hit || preserved_scene_final_reflective_hit ||
      preserved_transparent_scene_final_hit) {
    total_distance = preserved_total_distance;
    final_position = preserved_position;
    final_local_position = preserved_local_position;
    final_direction = preserved_direction;
    final_segment_distance = preserved_segment_distance;
    final_normal = preserved_normal;
    final_barycentric = preserved_barycentric;
    final_user_id = preserved_user_id;
    final_primitive_id = preserved_primitive_id;
    final_front_facing = preserved_front_facing;
    final_proxy = preserved_proxy;
    final_proxy_closure = preserved_proxy_closure;
    final_emission_replay = preserved_emission_replay;
    /* The restored receiver is shaded before its continuation. Do not tint its own BSDF
     * with the throughput accumulated AFTER leaving it (e.g. square a colored wall's
     * albedo merely by increasing the reflection bounce limit). Continuation payloads
     * retain their independently captured path weights. */
    throughput = preserved_output_throughput;
    carried_scene_final_throughput = preserved_output_throughput;
    apply_scene_final_throughput = true;
  }
  vec3 final_output_throughput = throughput;
  if (preserved_transparent_scene_final_hit) {
    final_output_throughput = preserved_output_throughput;
  }
  /* Keep this scene-final trace responsible for exporting the hit payload only.
   * Secondary GI receiver injection is removed from the active Nuru runtime. */
  const bool primary_diffuse_transport_receiver = supports_hardware_gi &&
                                                  !scene_final_specular_phase &&
                                                  (final_proxy_closure == HWRT_CLOSURE_DIFFUSE);
  if (primary_diffuse_transport_receiver) {
    const int diffuse_sample_count = max(uniforms.use_environment_pad.z, 1);
    const bool use_emissive_mixture = (uniforms.use_environment_pad.y > 0) &&
                                      (diffuse_sample_count > 1);
    const vec3 diffuse_gather_N = fast_gi_hit_normal(
        final_user_id,
        final_primitive_id,
        final_direction);
    const float diffuse_origin_epsilon = hwrt_gi_ray_epsilon(max(final_segment_distance, total_distance));
    const float diffuse_ray_tmin = diffuse_origin_epsilon;
    const float diffuse_self_hit_tmax = hwrt_gi_self_hit_distance(diffuse_origin_epsilon);
    const vec3 diffuse_origin = final_position + diffuse_gather_N * diffuse_origin_epsilon;
    vec3 incoming = vec3(0.0);
    for (int diffuse_sample = 0; diffuse_sample < diffuse_sample_count; diffuse_sample++) {
      const bool use_emissive_guiding = use_emissive_mixture &&
                                        ((diffuse_sample & 1) == 0);
      vec4 guided_sample = vec4(0.0);
      vec3 diffuse_dir;
      float diffuse_weight = 1.0;
      if (use_emissive_guiding) {
        guided_sample = sample_trace_emissive_direction(
            tid, diffuse_sample, diffuse_origin);
        diffuse_dir = guided_sample.xyz;
        const float cosine_pdf = clamp(dot(diffuse_gather_N, diffuse_dir), 0.0, 1.0) * 0.31830988618;
        const float mixture_pdf = max(0.5 * guided_sample.w + 0.5 * cosine_pdf, 1.0e-6);
        diffuse_weight = cosine_pdf / mixture_pdf;
      }
      else {
        diffuse_dir = sample_trace_diffuse_direction(
            tid, diffuse_sample, uniforms.closure_index + 37 + diffuse_sample * 13, diffuse_gather_N);
        diffuse_weight = use_emissive_mixture ? 2.0 : 1.0;
      }
      if (!(dot(diffuse_dir, diffuse_dir) > 1.0e-10) || diffuse_weight <= 0.0) {
        continue;
      }
      HitResult diffuse_intersection =
          trace_scene_closest(scene, diffuse_origin, diffuse_dir, diffuse_ray_tmin, 10000.0);
      if (diffuse_intersection.hit &&
          diffuse_intersection.dist <= diffuse_self_hit_tmax &&
          diffuse_intersection.user_instance_id == final_user_id &&
          diffuse_intersection.primitive_id == final_primitive_id)
      {
        /* Nuru: ignore only the exact source triangle self-hit. Do not hide nearby blockers with
         * a large launch bias; sealed wall shells must remain visible to the dome ray. */
        const vec3 retry_origin = diffuse_origin + diffuse_dir * (diffuse_intersection.dist +
                                                                   diffuse_ray_tmin);
        diffuse_intersection = trace_scene_closest(scene, retry_origin, diffuse_dir, diffuse_ray_tmin, 10000.0);
      }
      if (diffuse_intersection.hit) {
        const uint diffuse_user_id = diffuse_intersection.user_instance_id;
        incoming += min(max(emissive_radiance[diffuse_user_id].xyz, vec3(0.0)),
                        vec3(uniforms.clamp_indirect)) * diffuse_weight;
        const vec3 diffuse_hit_P = diffuse_origin + diffuse_dir * diffuse_intersection.dist;
        const vec3 diffuse_hit_N = fast_gi_hit_normal(
            diffuse_user_id,
            diffuse_intersection.primitive_id,
            diffuse_dir);
        incoming += sample_trace_direct_light(tid,
                                              diffuse_sample,
                                              diffuse_sample_count,
                                              diffuse_hit_P,
                                              diffuse_hit_N,
                                              diffuse_user_id,
                                              diffuse_intersection.primitive_id,
                                              true,
                                              false,
                                              scene) * diffuse_weight;
      }
      else if (!use_emissive_guiding) {
        incoming += min(sample_trace_world_radiance(
                            world_probe_tx, diffuse_dir, uniforms.use_environment_pad.w != 0),
                        vec3(uniforms.clamp_indirect)) * diffuse_weight;
      }
    }
    incoming /= float(diffuse_sample_count);
    /* Keep diffuse GI demodulated here; the full-resolution G-buffer color is applied later. */
    radiance += min(final_output_throughput * incoming,
                    vec3(uniforms.clamp_indirect));
  }
  const vec2 packed_direction = direction_pack(final_direction);
  imageStore(ray_time_img, ivec2(tid), vec4(max(total_distance, 1.0e-4), 0.0, 0.0, 0.0));
  imageStore(ray_radiance_img, ivec2(tid), vec4(radiance, 0.0));
  imageStore(hit_emission_replay_img, ivec2(tid), final_emission_replay);
  vec3 final_proxy_color = (final_proxy_closure == HWRT_CLOSURE_REFLECTION) ?
                                 final_proxy.reflection_color_roughness.xyz :
                             (final_proxy_closure == HWRT_CLOSURE_REFRACTION) ?
                                 final_proxy.transmission_color_roughness.xyz :
                                 diffuse_albedo[final_user_id].xyz;
  const uint final_proxy_flags = uint(final_proxy.ior_closure_type.w + 0.5);
  const bool final_textured_specular_scene_final_hit = final_layered_principled_scene_final_hit &&
      (final_proxy_closure == HWRT_CLOSURE_REFLECTION) &&
      ((final_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u);
  const bool final_metallic_bsdf_scene_final_hit = final_textured_specular_scene_final_hit &&
      ((final_proxy_flags & HWRT_PROXY_FLAG_METALLIC_BSDF_SCENE_FINAL) != 0u);
  if ((final_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) {
    final_proxy_color = vec3(1.0);
  }
  float final_proxy_roughness = (final_proxy_closure == HWRT_CLOSURE_REFLECTION) ?
                                    final_proxy.reflection_color_roughness.w :
                                    final_proxy.transmission_color_roughness.w;
  float final_proxy_ior = (final_proxy_closure == HWRT_CLOSURE_REFLECTION) ?
                              final_proxy.ior_closure_type.x :
                              final_proxy.ior_closure_type.y;
  imageStore(hit_albedo_img, ivec2(tid), vec4(clamp(final_output_throughput * final_proxy_color,
                                vec3(0.0),
                                vec3(uniforms.clamp_indirect)),
                              -1.0));
  imageStore(hit_material_img, ivec2(tid), vec4(final_proxy_roughness,
                                 final_proxy_ior,
                                 float(final_proxy_closure),
                                 packed_direction.x));
  imageStore(hit_normal_img, ivec2(tid), vec4(final_normal, packed_direction.y));
  imageStore(hit_position_img, ivec2(tid), vec4(final_local_position, total_distance));
  imageStore(hit_world_position_img, ivec2(tid), vec4(final_position, total_distance));
  imageStore(hit_throughput_img, ivec2(tid), vec4(apply_scene_final_throughput ? carried_scene_final_throughput :
                                       vec3(1.0),
                                   apply_scene_final_throughput ? 1.0 : 0.0));
  const uint identity_flags = final_front_facing |
                              ((final_layered_principled_scene_final_hit ||
                                preserved_scene_final_reflective_hit) ? 2u : 0u) |
                              (final_transparent_scene_final_hit ? 4u : 0u) |
                              ((final_refracted_textured_receiver_hit ||
                                final_textured_specular_scene_final_hit) ? 16u : 0u) |
                              (((final_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u) ?
                                   HWRT_HIT_IDENTITY_PRINCIPLED_LAYERED_SCENE_FINAL :
                                   0u) |
                              (final_metallic_bsdf_scene_final_hit ?
                                   HWRT_HIT_IDENTITY_METALLIC_BSDF_SCENE_FINAL :
                                   0u);
  imageStore(hit_identity_img, ivec2(tid), uvec4(final_user_id, final_primitive_id, identity_flags, 0xFFFFFFFFu));
  const float final_reflection_layer_coverage =
      ((final_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u) ?
          clamp(final_proxy.packed_thickness.z, 0.0, 1.0) :
          1.0;
  imageStore(hit_barycentric_img, ivec2(tid), vec4(final_barycentric.x,
                                  final_barycentric.y,
                                  final_segment_distance,
                                  final_reflection_layer_coverage));
  if (layered_receiver_valid) {
    const vec2 layered_receiver_packed_direction = direction_pack(layered_receiver_direction);
    const uint layered_receiver_proxy_flags = uint(layered_receiver_proxy.ior_closure_type.w + 0.5);
    vec3 layered_receiver_proxy_color =
        (layered_receiver_proxy_closure == HWRT_CLOSURE_REFLECTION) ?
            layered_receiver_proxy.reflection_color_roughness.xyz :
        (layered_receiver_proxy_closure == HWRT_CLOSURE_REFRACTION) ?
            layered_receiver_proxy.transmission_color_roughness.xyz :
            diffuse_albedo[layered_receiver_user_id].xyz;
    if ((layered_receiver_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) {
      layered_receiver_proxy_color = vec3(1.0);
    }
    const float layered_receiver_proxy_roughness =
        (layered_receiver_proxy_closure == HWRT_CLOSURE_REFLECTION) ?
            layered_receiver_proxy.reflection_color_roughness.w :
            layered_receiver_proxy.transmission_color_roughness.w;
    const float layered_receiver_proxy_ior =
        (layered_receiver_proxy_closure == HWRT_CLOSURE_REFLECTION) ?
            layered_receiver_proxy.ior_closure_type.x :
            layered_receiver_proxy.ior_closure_type.y;
    const float layered_receiver_reflection_layer_coverage =
        ((layered_receiver_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u) ?
            clamp(layered_receiver_proxy.packed_thickness.z, 0.0, 1.0) :
            1.0;
    imageStore(layered_receiver_ray_time_img, ivec2(tid),
        vec4(max(layered_receiver_total_distance, 1.0e-4), 0.0, 0.0, 0.0));
    imageStore(layered_receiver_ray_radiance_img, ivec2(tid), vec4(layered_receiver_continued_radiance, 0.0));
    /* This payload does not accumulate its own terminal proxy emission. */
    imageStore(layered_receiver_emission_replay_img, ivec2(tid), vec4(1.0, 1.0, 1.0, 0.0));
    imageStore(layered_receiver_albedo_img, ivec2(tid),
        vec4(clamp(layered_receiver_proxy_color,
                     vec3(0.0),
                     vec3(uniforms.clamp_indirect)),
               -1.0));
    imageStore(layered_receiver_material_img, ivec2(tid), vec4(layered_receiver_proxy_roughness,
                                               layered_receiver_proxy_ior,
                                               float(layered_receiver_proxy_closure),
                                               layered_receiver_packed_direction.x));
    imageStore(layered_receiver_normal_img, ivec2(tid), vec4(layered_receiver_normal, layered_receiver_packed_direction.y));
    imageStore(layered_receiver_position_img, ivec2(tid),
        vec4(layered_receiver_local_position, layered_receiver_total_distance));
    imageStore(layered_receiver_world_position_img, ivec2(tid),
        vec4(layered_receiver_position, layered_receiver_total_distance));
    const float layered_receiver_throughput_alpha =
        (preserved_transparent_scene_final_hit ||
         (scene_final_specular_phase && continuation_required)) ?
            1.0 :
            0.0;
    imageStore(layered_receiver_throughput_img, ivec2(tid),
        vec4(layered_receiver_carried_throughput, layered_receiver_throughput_alpha));
    const uint layered_receiver_identity_flags =
        layered_receiver_front_facing | 8u |
        (((layered_receiver_proxy_closure == HWRT_CLOSURE_REFLECTION) &&
          ((layered_receiver_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u)) ?
             16u :
             0u) |
        (((layered_receiver_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) ?
             2u :
             0u) |
        (((layered_receiver_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u) ?
             HWRT_HIT_IDENTITY_PRINCIPLED_LAYERED_SCENE_FINAL :
             0u);
    imageStore(layered_receiver_identity_img, ivec2(tid), uvec4(layered_receiver_user_id,
                                              layered_receiver_primitive_id,
                                              layered_receiver_identity_flags,
                                              0xFFFFFFFFu));
    imageStore(layered_receiver_barycentric_img, ivec2(tid),
        vec4(layered_receiver_barycentric.x,
               layered_receiver_barycentric.y,
               layered_receiver_segment_distance,
               layered_receiver_reflection_layer_coverage));
  }
  if (transmission_receiver_valid) {
    const vec2 transmission_receiver_packed_direction = direction_pack(
        transmission_receiver_direction);
    const uint transmission_receiver_proxy_flags = uint(
        transmission_receiver_proxy.ior_closure_type.w + 0.5);
    vec3 transmission_receiver_proxy_color =
        (transmission_receiver_proxy_closure == HWRT_CLOSURE_REFLECTION) ?
            transmission_receiver_proxy.reflection_color_roughness.xyz :
        (transmission_receiver_proxy_closure == HWRT_CLOSURE_REFRACTION) ?
            transmission_receiver_proxy.transmission_color_roughness.xyz :
            diffuse_albedo[transmission_receiver_user_id].xyz;
    if ((transmission_receiver_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) {
      transmission_receiver_proxy_color = vec3(1.0);
    }
    const float transmission_receiver_proxy_roughness =
        (transmission_receiver_proxy_closure == HWRT_CLOSURE_REFLECTION) ?
            transmission_receiver_proxy.reflection_color_roughness.w :
            transmission_receiver_proxy.transmission_color_roughness.w;
    const float transmission_receiver_proxy_ior =
        (transmission_receiver_proxy_closure == HWRT_CLOSURE_REFLECTION) ?
            transmission_receiver_proxy.ior_closure_type.x :
            transmission_receiver_proxy.ior_closure_type.y;
    const float transmission_receiver_reflection_layer_coverage =
        ((transmission_receiver_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u) ?
            clamp(transmission_receiver_proxy.packed_thickness.z, 0.0, 1.0) :
            1.0;
    imageStore(transmission_receiver_ray_time_img, ivec2(tid),
        vec4(max(transmission_receiver_total_distance, 1.0e-4), 0.0, 0.0, 0.0));
    imageStore(transmission_receiver_ray_radiance_img, ivec2(tid), vec4(transmission_receiver_continued_radiance, 0.0));
    imageStore(transmission_receiver_emission_replay_img, ivec2(tid), transmission_emission_replay);
    imageStore(transmission_receiver_albedo_img, ivec2(tid),
        vec4(clamp(transmission_receiver_proxy_color,
                     vec3(0.0),
                     vec3(uniforms.clamp_indirect)),
               -1.0));
    imageStore(transmission_receiver_material_img, ivec2(tid),
        vec4(transmission_receiver_proxy_roughness,
               transmission_receiver_proxy_ior,
               float(transmission_receiver_proxy_closure),
               transmission_receiver_packed_direction.x));
    imageStore(transmission_receiver_normal_img, ivec2(tid),
        vec4(transmission_receiver_normal, transmission_receiver_packed_direction.y));
    imageStore(transmission_receiver_position_img, ivec2(tid),
        vec4(transmission_receiver_local_position, transmission_receiver_total_distance));
    imageStore(transmission_receiver_world_position_img, ivec2(tid),
        vec4(transmission_receiver_position, transmission_receiver_total_distance));
    imageStore(transmission_receiver_throughput_img, ivec2(tid),
        vec4(transmission_receiver_carried_throughput,
               transmission_receiver_apply_throughput ? 1.0 : 0.0));
    const bool transmission_receiver_textured_replay =
        transmission_receiver_direct_lit_reflective ||
        ((transmission_receiver_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) ||
        ((transmission_receiver_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u);
    const uint transmission_receiver_identity_flags =
        transmission_receiver_front_facing | 8u |
        (transmission_receiver_textured_replay ? 16u : 0u) |
        (((transmission_receiver_proxy_flags & HWRT_PROXY_FLAG_TEXTURED_SPECULAR_SCENE_FINAL) != 0u) ?
             2u :
             0u) |
        (((transmission_receiver_proxy_flags & HWRT_PROXY_FLAG_PRINCIPLED_LAYERED_SCENE_FINAL) != 0u) ?
             HWRT_HIT_IDENTITY_PRINCIPLED_LAYERED_SCENE_FINAL :
             0u);
    imageStore(transmission_receiver_identity_img, ivec2(tid), uvec4(transmission_receiver_user_id,
                                                   transmission_receiver_primitive_id,
                                                   transmission_receiver_identity_flags,
                                                   0xFFFFFFFFu));
    imageStore(transmission_receiver_barycentric_img, ivec2(tid),
        vec4(transmission_receiver_barycentric.x,
               transmission_receiver_barycentric.y,
               transmission_receiver_segment_distance,
               transmission_receiver_reflection_layer_coverage));
  }
}
