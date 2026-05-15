/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * The light module manages light data buffers and light culling system.
 */

#include <algorithm>
#include <limits>

#include "draw_debug.hh"

#include "eevee_instance.hh"

#include "eevee_light.hh"

#include "DNA_light_types.h"
#include "DNA_node_types.h"
#include "DNA_sdna_type_ids.hh"

#include "BKE_light.h"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"

#include "BLI_set.hh"

#include "GPU_capabilities.hh"

namespace blender::eevee {

enum class LightShaderDependency {
  Passthrough,
  Uniform,
  PointDependent,
};

static const bNode *light_nodetree_eevee_light_shader_output_get(const bNodeTree *nodetree)
{
  if (nodetree == nullptr) {
    return nullptr;
  }
  const bNode *output = nullptr;
  for (const bNode &node : nodetree->nodes) {
    if (node.type_legacy != SH_NODE_EEVEE_LIGHT_SHADER_OUTPUT) {
      continue;
    }
    if (output == nullptr || ((node.flag & NODE_DO_OUTPUT) && !(output->flag & NODE_DO_OUTPUT))) {
      output = &node;
    }
  }
  return output;
}

static float light_nodetree_eevee_light_shader_range_scale_get(const bNodeTree *nodetree)
{
  const bNode *output = light_nodetree_eevee_light_shader_output_get(nodetree);
  if (output == nullptr) {
    return 1.0f;
  }
  return (output->custom3 > 0.0f && isfinite(output->custom3)) ? output->custom3 : 1.0f;
}

static const bNodeLink *light_nodetree_link_to_input_get(const bNodeTree &nodetree,
                                                         const bNodeSocket &input)
{
  const bNodeLink *direct_link = nullptr;
  for (const bNodeLink &link : nodetree.links) {
    if (link.tosock != &input) {
      continue;
    }
    if (direct_link != nullptr) {
      return nullptr;
    }
    direct_link = &link;
  }
  return direct_link;
}

static bool light_nodetree_output_input_is_default_info_link(const bNodeTree &nodetree,
                                                             const bNode &output,
                                                             const char *to_identifier,
                                                             const char *from_identifier,
                                                             const bNode *&r_info_node)
{
  const bNodeSocket *to_socket = bke::node_find_socket(output, SOCK_IN, to_identifier);
  if (to_socket == nullptr || (to_socket->flag & SOCK_UNAVAIL)) {
    return false;
  }

  const bNodeLink *link = light_nodetree_link_to_input_get(nodetree, *to_socket);
  if (link == nullptr || (link->flag & NODE_LINK_MUTED) || (link->flag & NODE_LINK_VALID) == 0 ||
      link->fromnode == nullptr || link->fromsock == nullptr)
  {
    return false;
  }

  const bNode &info = *link->fromnode;
  if (info.type_legacy != SH_NODE_EEVEE_LIGHT_SHADER_INFO || (info.flag & NODE_MUTED)) {
    return false;
  }
  if (r_info_node != nullptr && r_info_node != &info) {
    return false;
  }

  const bNodeSocket *from_socket = bke::node_find_socket(info, SOCK_OUT, from_identifier);
  if (from_socket == nullptr || (from_socket->flag & SOCK_UNAVAIL) ||
      link->fromsock != from_socket)
  {
    return false;
  }

  r_info_node = &info;
  return true;
}

static bool light_nodetree_eevee_light_shader_output_is_default_passthrough(
    const bNodeTree *nodetree)
{
  const bNode *output = light_nodetree_eevee_light_shader_output_get(nodetree);
  if (nodetree == nullptr || output == nullptr || (output->flag & NODE_MUTED)) {
    return false;
  }
  if (fabsf(light_nodetree_eevee_light_shader_range_scale_get(nodetree) - 1.0f) > 1e-6f) {
    return false;
  }

  const bNode *info_node = nullptr;
  return light_nodetree_output_input_is_default_info_link(
             *nodetree, *output, "Color", "Default Color", info_node) &&
         light_nodetree_output_input_is_default_info_link(
             *nodetree, *output, "Intensity", "Default Intensity", info_node) &&
         light_nodetree_output_input_is_default_info_link(
             *nodetree, *output, "Attenuation", "Default Attenuation", info_node);
}

static LightShaderDependency light_shader_dependency_join(const LightShaderDependency a,
                                                          const LightShaderDependency b)
{
  return (a == LightShaderDependency::PointDependent ||
          b == LightShaderDependency::PointDependent) ?
             LightShaderDependency::PointDependent :
             LightShaderDependency::Uniform;
}

static bool light_shader_node_type_is_point_dependent(const int type)
{
  return ELEM(type,
              SH_NODE_TEX_BRICK,
              SH_NODE_TEX_CHECKER,
              SH_NODE_TEX_GABOR,
              SH_NODE_TEX_GRADIENT,
              SH_NODE_TEX_HEXAGON,
              SH_NODE_TEX_IMAGE,
              SH_NODE_TEX_MAGIC,
              SH_NODE_TEX_NOISE,
              SH_NODE_TEX_VORONOI,
              SH_NODE_TEX_WAVE,
              SH_NODE_TEX_WHITE_NOISE);
}

static bool light_shader_node_type_is_uniform(const int type)
{
  switch (type) {
    case SH_NODE_RGB:
    case SH_NODE_VALUE:
    case SH_NODE_SCENE_TIME:
    case SH_NODE_BLACKBODY:
    case SH_NODE_BRIGHTCONTRAST:
    case SH_NODE_VALTORGB:
    case SH_NODE_GAMMA:
    case SH_NODE_HUE_SAT:
    case SH_NODE_INVERT:
    case SH_NODE_MIX:
    case SH_NODE_CURVE_RGB:
    case SH_NODE_WAVELENGTH:
    case SH_NODE_COMBINE_COLOR:
    case SH_NODE_SEPARATE_COLOR:
    case SH_NODE_RGBTOBW:
    case SH_NODE_COMBXYZ:
    case SH_NODE_SEPXYZ:
    case SH_NODE_MAP_RANGE:
    case SH_NODE_MAPPING:
    case SH_NODE_NORMAL:
    case SH_NODE_CURVE_VEC:
    case SH_NODE_VECTOR_MATH:
    case SH_NODE_VECTOR_ROTATE:
    case SH_NODE_VECT_TRANSFORM:
    case SH_NODE_CLAMP:
    case SH_NODE_CURVE_FLOAT:
    case SH_NODE_MATH:
      return true;
    default:
      return false;
  }
}

static LightShaderDependency light_shader_socket_dependency_get(const bNodeTree &nodetree,
                                                                const bNodeSocket &socket,
                                                                Set<const bNode *> &visited);

static LightShaderDependency light_shader_node_dependency_get(const bNodeTree &nodetree,
                                                              const bNode &node,
                                                              const bNodeSocket *from_socket,
                                                              Set<const bNode *> &visited)
{
  if (node.flag & NODE_MUTED) {
    return LightShaderDependency::PointDependent;
  }
  if (visited.contains(&node)) {
    return LightShaderDependency::Uniform;
  }
  visited.add(&node);

  if (node.type_legacy == SH_NODE_EEVEE_LIGHT_SHADER_INFO) {
    if (from_socket == nullptr) {
      return LightShaderDependency::PointDependent;
    }
    if (ELEM(StringRef(from_socket->identifier),
             StringRef("Distance"),
             StringRef("Light Space"),
             StringRef("Direction"),
             StringRef("Default Attenuation")))
    {
      return LightShaderDependency::PointDependent;
    }
    if (ELEM(StringRef(from_socket->identifier),
             StringRef("Default Color"),
             StringRef("Default Intensity"),
             StringRef("World Position"),
             StringRef("Rotation")))
    {
      return LightShaderDependency::Uniform;
    }
    return LightShaderDependency::PointDependent;
  }

  if (node.type_legacy == NODE_REROUTE) {
    const bNodeSocket *input_socket = static_cast<const bNodeSocket *>(node.inputs.first);
    return input_socket ? light_shader_socket_dependency_get(nodetree, *input_socket, visited) :
                          LightShaderDependency::Uniform;
  }

  if (light_shader_node_type_is_point_dependent(node.type_legacy)) {
    return LightShaderDependency::PointDependent;
  }
  if (!light_shader_node_type_is_uniform(node.type_legacy)) {
    return LightShaderDependency::PointDependent;
  }

  LightShaderDependency result = LightShaderDependency::Uniform;
  for (const bNodeSocket *input_socket = static_cast<const bNodeSocket *>(node.inputs.first);
       input_socket != nullptr;
       input_socket = input_socket->next)
  {
    result = light_shader_dependency_join(
        result, light_shader_socket_dependency_get(nodetree, *input_socket, visited));
    if (result == LightShaderDependency::PointDependent) {
      return result;
    }
  }
  return result;
}

static LightShaderDependency light_shader_socket_dependency_get(const bNodeTree &nodetree,
                                                                const bNodeSocket &socket,
                                                                Set<const bNode *> &visited)
{
  const bNodeLink *link = light_nodetree_link_to_input_get(nodetree, socket);
  if (link == nullptr) {
    return LightShaderDependency::Uniform;
  }
  if ((link->flag & NODE_LINK_MUTED) || (link->flag & NODE_LINK_VALID) == 0 ||
      link->fromnode == nullptr)
  {
    return LightShaderDependency::PointDependent;
  }
  return light_shader_node_dependency_get(nodetree, *link->fromnode, link->fromsock, visited);
}

static LightShaderDependency light_nodetree_eevee_light_shader_dependency_get(
    const bNodeTree *nodetree)
{
  const bNode *output = light_nodetree_eevee_light_shader_output_get(nodetree);
  if (nodetree == nullptr || output == nullptr || (output->flag & NODE_MUTED)) {
    return LightShaderDependency::Passthrough;
  }
  if (light_nodetree_eevee_light_shader_output_is_default_passthrough(nodetree)) {
    return LightShaderDependency::Passthrough;
  }
  if (fabsf(light_nodetree_eevee_light_shader_range_scale_get(nodetree) - 1.0f) > 1e-6f) {
    return LightShaderDependency::PointDependent;
  }

  LightShaderDependency result = LightShaderDependency::Uniform;
  for (const StringRefNull input_identifier : {"Color", "Intensity", "Attenuation"}) {
    const bNodeSocket *input_socket = bke::node_find_socket(*output, SOCK_IN, input_identifier);
    if (input_socket == nullptr || (input_socket->flag & SOCK_UNAVAIL)) {
      return LightShaderDependency::PointDependent;
    }
    Set<const bNode *> visited;
    result = light_shader_dependency_join(
        result, light_shader_socket_dependency_get(*nodetree, *input_socket, visited));
    if (result == LightShaderDependency::PointDependent) {
      return result;
    }
  }
  return result;
}

/* Convert by putting the least significant bits in the first component. */
static uint2 uint64_to_uint2(uint64_t data)
{
  return {uint(data), uint(data >> uint64_t(32))};
}

static void light_shader_index_buf_ensure_no_shader(LightShaderIndexBuf &index_buf, int len)
{
  index_buf.resize(len);
  for (const int i : IndexRange(len)) {
    index_buf[i] = -1;
  }
}

static void light_shader_index_buf_disable_point_dependent(LightShaderIndexBuf &index_buf, int len)
{
  index_buf.resize(len);
  for (const int i : IndexRange(len)) {
    if (index_buf[i] >= 0) {
      index_buf[i] = -1;
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name LightData
 * \{ */

static eLightType to_light_type(short blender_light_type,
                                short blender_area_type,
                                bool use_soft_falloff)
{
  switch (blender_light_type) {
    default:
    case LA_LOCAL:
      return use_soft_falloff ? LIGHT_OMNI_DISK : LIGHT_OMNI_SPHERE;
    case LA_SUN:
      return LIGHT_SUN;
    case LA_SPOT:
      return use_soft_falloff ? LIGHT_SPOT_DISK : LIGHT_SPOT_SPHERE;
    case LA_AREA:
      return ELEM(blender_area_type, LA_AREA_DISK, LA_AREA_ELLIPSE) ? LIGHT_ELLIPSE : LIGHT_RECT;
  }
}

static float4x4 light_object_to_world_normalized_get(float4x4 object_to_world, float3 &r_scale)
{
  using namespace blender::math;

  object_to_world.view<3, 3>() = normalize_and_get_size(object_to_world.view<3, 3>(), r_scale);

  /* Make sure we have consistent handedness (in case of negatively scaled Z axis). */
  float3 back = cross(float3(object_to_world.x_axis()), float3(object_to_world.y_axis()));
  if (dot(back, float3(object_to_world.z_axis())) < 0.0f) {
    negate_v3(object_to_world.y_axis());
  }

  return object_to_world;
}

static float4x4 light_object_to_world_normalized_get(float4x4 object_to_world)
{
  float3 scale;
  return light_object_to_world_normalized_get(object_to_world, scale);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Light Object
 * \{ */

void Light::sync(ShadowModule &shadows,
                 float4x4 object_to_world,
                 char visibility_flag,
                 const blender::Light *la,
                 const LightLinking *light_linking /* = nullptr */,
                 float light_shader_range_scale,
                 float threshold,
                 int lightgroup_id)
{
  using namespace blender::math;

  eLightType new_type = to_light_type(la->type, la->area_shape, la->mode & LA_USE_SOFT_FALLOFF);
  if (assign_if_different(this->type, new_type)) {
    shadow_discard_safe(shadows);
  }
  this->light_shader_range_scale = light_shader_range_scale;

  this->color = BKE_light_power(*la) * BKE_light_color(*la);
  if (la->mode & LA_UNNORMALIZED) {
    this->color *= BKE_light_area(*la, object_to_world);
  }

  float3 scale;
  object_to_world = light_object_to_world_normalized_get(object_to_world, scale);
  this->object_to_world = object_to_world;

  shape_parameters_set(la,
                       scale,
                       object_to_world.z_axis(),
                       light_shader_range_scale,
                       threshold,
                       shadows.get_data().use_jitter);

  const bool diffuse_visibility = (visibility_flag & OB_HIDE_DIFFUSE) == 0;
  const bool glossy_visibility = (visibility_flag & OB_HIDE_GLOSSY) == 0;
  const bool transmission_visibility = (visibility_flag & OB_HIDE_TRANSMISSION) == 0;
  const bool volume_visibility = (visibility_flag & OB_HIDE_VOLUME_SCATTER) == 0;

  float shape_power = shape_radiance_get();
  float point_power = point_radiance_get();
  this->power[LIGHT_DIFFUSE] = la->diff_fac * shape_power * diffuse_visibility;
  this->power[LIGHT_SPECULAR] = la->spec_fac * shape_power * glossy_visibility;
  this->power[LIGHT_TRANSMISSION] = la->transmission_fac * shape_power * transmission_visibility;
  this->power[LIGHT_VOLUME] = la->volume_fac * point_power * volume_visibility;

  this->lod_bias = shadows.global_lod_bias();
  this->lod_min = shadow_lod_min_get(la);
  this->filter_radius = la->shadow_filter_radius;
  this->shadow_jitter = (la->mode & LA_SHADOW_JITTER) != 0;
  this->lightgroup_id = max_ii(lightgroup_id, 0);

  this->shadow_map_scale = max_ff(la->shadow_map_scale, 0.0001f);

  if (la->mode & LA_SHADOW) {
    shadow_ensure(shadows);
  }
  else {
    shadow_discard_safe(shadows);
  }

  if (light_linking) {
    this->light_set_membership = uint64_to_uint2(light_linking->runtime.light_set_membership);
    this->shadow_set_membership = uint64_to_uint2(light_linking->runtime.shadow_set_membership);
  }
  else {
    /* Set all bits if light linking is not used. */
    this->light_set_membership = uint64_to_uint2(~uint64_t(0));
    this->shadow_set_membership = uint64_to_uint2(~uint64_t(0));
  }

  this->initialized = true;
}

float Light::shadow_lod_min_get(const blender::Light *la)
{
  /* Property is in mm. Convert to unit. */
  float max_res_unit = la->shadow_maximum_resolution;
  if (is_sun_light(this->type)) {
    return log2f(max_res_unit * SHADOW_MAP_MAX_RES) - 1.0f;
  }
  /* Store absolute mode as negative. */
  return (la->mode & LA_SHAD_RES_ABSOLUTE) ? -max_res_unit : max_res_unit;
}

void Light::shadow_discard_safe(ShadowModule &shadows)
{
  if (this->directional != nullptr) {
    shadows.directional_pool.destruct(*directional);
    this->directional = nullptr;
  }
  if (this->punctual != nullptr) {
    shadows.punctual_pool.destruct(*punctual);
    this->punctual = nullptr;
  }
}

void Light::shadow_ensure(ShadowModule &shadows)
{
  if (is_sun_light(this->type) && this->directional == nullptr) {
    this->directional = &shadows.directional_pool.construct(shadows);
  }
  else if (this->punctual == nullptr) {
    this->punctual = &shadows.punctual_pool.construct(shadows);
  }
}

float Light::attenuation_radius_get(const blender::Light *la,
                                    float light_threshold,
                                    float light_power)
{
  if (la->mode & LA_CUSTOM_ATTENUATION) {
    return la->att_dist;
  }
  /* Compute the distance (using the inverse square law)
   * at which the light power reaches the light_threshold. */
  /* TODO take area light scale into account. */
  return sqrtf(light_power / light_threshold);
}

void Light::shape_parameters_set(const blender::Light *la,
                                 const float3 &scale,
                                 const float3 &z_axis,
                                 const float light_shader_range_scale,
                                 const float threshold,
                                 const bool use_jitter)
{
  using namespace blender::math;

  /* Compute influence radius first. Can be amended by shape later. */
  if (is_local_light(this->type)) {
    LightLocalData &l_local = this->local();
    const float max_power = reduce_max(BKE_light_color(*la)) *
                            fabsf(BKE_light_power(*la) / 100.0f);
    const float surface_max_power = max(la->diff_fac, la->spec_fac) * max_power;
    const float volume_max_power = la->volume_fac * max_power;

    float influence_radius_surface = attenuation_radius_get(la, threshold, surface_max_power) *
                                     light_shader_range_scale;
    float influence_radius_volume = attenuation_radius_get(la, threshold, volume_max_power) *
                                    light_shader_range_scale;

    l_local.local.influence_radius_max = max(influence_radius_surface, influence_radius_volume);
    l_local.local.influence_radius_invsqr_surface = safe_rcp(square(influence_radius_surface));
    l_local.local.influence_radius_invsqr_volume = safe_rcp(square(influence_radius_volume));
    /* TODO(fclem): This is just duplicating a member for local lights. */
    this->clip_far = float_as_int(l_local.local.influence_radius_max);
    this->clip_near = float_as_int(l_local.local.influence_radius_max / 4000.0f);
  }

  float trace_scaling_fac = (use_jitter && (la->mode & LA_SHADOW_JITTER)) ?
                                la->shadow_jitter_overblur / 100.0f :
                                1.0f;

  if (is_sun_light(this->type)) {
    LightSunData &l_sun = this->sun();
    float sun_half_angle = min_ff(la->sun_angle, DEG2RADF(179.9f)) / 2.0f;
    /* Use non-clamped radius for soft shadows. Avoid having a minimum blur. */
    l_sun.shadow_angle = sun_half_angle * trace_scaling_fac;
    /* Clamp to a minimum to distinguish between point lights and area light shadow. */
    l_sun.shadow_angle = (sun_half_angle > 0.0f) ? max_ff(1e-8f, l_sun.shadow_angle) : 0.0f;
    /* Precompute this cosine on CPU to avoid differences in shadow tracing between platforms. */
    l_sun.shadow_angle_cos = cosf(l_sun.shadow_angle);
    /* Clamp to minimum value before float imprecision artifacts appear. */
    l_sun.shape_radius = clamp(tanf(sun_half_angle), 0.001f, 20.0f);
    /* Stable shading direction. */
    l_sun.direction = z_axis;
  }
  else if (is_area_light(this->type)) {
    LightAreaData &l_area = this->area();
    const bool is_irregular = ELEM(la->area_shape, LA_AREA_RECT, LA_AREA_ELLIPSE);
    l_area.size = float2(la->area_size, is_irregular ? la->area_sizey : la->area_size);
    /* Scale and clamp to minimum value before float imprecision artifacts appear. */
    l_area.size *= scale.xy() / 2.0f;
    l_area.shadow_scale = trace_scaling_fac;
    l_area.local.shadow_radius = length(l_area.size) * trace_scaling_fac;
    /* Set to default position. */
    l_area.local.shadow_position = float3(0.0f);
    /* Do not render lights that have no area. */
    if (l_area.size.x * l_area.size.y < 0.00001f) {
      /* Forces light to be culled. */
      l_area.local.influence_radius_max = 0.0f;
    }
    /* Clamp to minimum value before float imprecision artifacts appear. */
    l_area.size = max(float2(0.003f), l_area.size);
    /* For volume point lighting. */
    l_area.local.shape_radius = max(0.001f, length(l_area.size) / 2.0f);
  }
  else if (is_point_light(this->type)) {
    LightSpotData &l_spot = this->spot();
    LightLocalData &l_local = this->local();
    /* Spot size & blend */
    if (is_spot_light(this->type)) {
      const float spot_size = cosf(la->spotsize * 0.5f);
      const float spot_blend = (1.0f - spot_size) * la->spotblend;
      l_spot.spot_size_inv = scale.z / max(scale.xy(), float2(1e-8f));
      l_spot.spot_mul = 1.0f / max(1e-8f, spot_blend);
      l_spot.spot_bias = -spot_size * l_spot.spot_mul;
      l_spot.spot_tan = tanf(min(la->spotsize * 0.5f, float(M_PI_2 - 0.0001f)));
    }
    else {
      /* Point light could access it. Make sure to avoid Undefined Behavior.
       * In practice it is only ever used. */
      l_spot.spot_size_inv = float2(1.0f);
      l_spot.spot_mul = 0.0f;
      l_spot.spot_bias = 1.0f;
      l_spot.spot_tan = 0.0f;
    }
    /* Use unclamped radius for soft shadows. Avoid having a minimum blur. */
    l_local.local.shadow_radius = max(0.0f, la->radius) * trace_scaling_fac;
    /* Clamp to a minimum to distinguish between point lights and area light shadow. */
    l_local.local.shadow_radius = (la->radius > 0.0f) ?
                                      max_ff(1e-8f, local().local.shadow_radius) :
                                      0.0f;
    /* Set to default position. */
    l_local.local.shadow_position = float3(0.0f);
    l_local.local.shape_radius = la->radius;
    /* Clamp to minimum value before float imprecision artifacts appear. */
    l_local.local.shape_radius = max(0.001f, l_local.local.shape_radius);
  }
}

float Light::shape_radiance_get()
{
  using namespace blender::math;

  /* Make illumination power constant. */
  switch (this->type) {
    case LIGHT_RECT:
    case LIGHT_ELLIPSE: {
      /* Rectangle area. */
      float area = this->area().size.x * this->area().size.y * 4.0f;
      /* Scale for the lower area of the ellipse compared to the surrounding rectangle. */
      if (this->type == LIGHT_ELLIPSE) {
        area *= M_PI / 4.0f;
      }
      /* Convert radiant flux to radiance. */
      return float(M_1_PI) / area;
    }
    case LIGHT_OMNI_SPHERE:
    case LIGHT_OMNI_DISK:
    case LIGHT_SPOT_SPHERE:
    case LIGHT_SPOT_DISK: {
      /* Sphere area. */
      float area = float(4.0f * M_PI) * square(this->local().local.shape_radius);
      /* Convert radiant flux to radiance. */
      return 1.0f / (area * float(M_PI));
    }
    case LIGHT_SUN_ORTHO:
    case LIGHT_SUN: {
      float inv_sin_sq = 1.0f + 1.0f / square(this->sun().shape_radius);
      /* Convert irradiance to radiance. */
      return float(M_1_PI) * inv_sin_sq;
    }
  }
  BLI_assert_unreachable();
  return 0.0f;
}

float Light::point_radiance_get()
{
  /* Volume light is evaluated as point lights. */
  switch (this->type) {
    case LIGHT_RECT:
    case LIGHT_ELLIPSE: {
      /* This corrects for area light most representative point trick.
       * The fit was found by reducing the average error compared to cycles. */
      float area = this->area().size.x * this->area().size.y * 4.0f;
      float tmp = M_PI_2 / (M_PI_2 + sqrtf(area));
      /* Lerp between 1.0 and the limit (1 / pi). */
      float mrp_scaling = tmp + (1.0f - tmp) * M_1_PI;
      return float(M_1_PI) * mrp_scaling;
    }
    case LIGHT_OMNI_SPHERE:
    case LIGHT_OMNI_DISK:
    case LIGHT_SPOT_SPHERE:
    case LIGHT_SPOT_DISK: {
      /* Convert radiant flux to intensity. */
      /* Inverse of sphere solid angle. */
      return float(1.0 / (4.0 * M_PI));
    }
    case LIGHT_SUN_ORTHO:
    case LIGHT_SUN: {
      return 1.0f;
    }
  }
  BLI_assert_unreachable();
  return 0.0f;
}

void Light::debug_draw()
{
  drw_debug_sphere(transform_location(this->object_to_world),
                   this->local().local.influence_radius_max,
                   float4(0.8f, 0.3f, 0.0f, 1.0f));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name LightModule
 * \{ */

LightModule::LightModule(Instance &inst) : inst_(inst)
{
}

gpu::UniformBuf *LightModule::world_sunlight_ubo() const
{
  return inst_.world.sunlight;
}

LightModule::~LightModule()
{
  /* WATCH: Destructor order. Expect shadow module to be destructed later. */
  for (Light &light : light_map_.values()) {
    light.shadow_discard_safe(inst_.shadows);
  }
};

void LightModule::add_world_sun_light(const ObjectKey &key, bool use_diffuse, bool use_glossy)
{
  /* Create a placeholder light to be fed by the GPU after sunlight extraction.
   * Sunlight is disabled if power is zero. */
  blender::Light la = {};
  la.type = LA_SUN;
  /* Set on the GPU. */
  la.r = la.g = la.b = -1.0f; /* Tag as world sun light. */
  la.energy = 1.0f;
  la.sun_angle = inst_.world.sun_angle();
  la.shadow_filter_radius = inst_.world.sun_shadow_filter_radius();
  la.shadow_jitter_overblur = inst_.world.sun_shadow_jitter_overblur();
  la.shadow_maximum_resolution = inst_.world.sun_shadow_max_resolution();
  SET_FLAG_FROM_TEST(la.mode, inst_.world.use_sun_shadow(), LA_SHADOW);
  SET_FLAG_FROM_TEST(la.mode, inst_.world.use_sun_shadow_jitter(), LA_SHADOW_JITTER);
  int visibility_flag = 0;
  SET_FLAG_FROM_TEST(visibility_flag, !use_diffuse, OB_HIDE_DIFFUSE);
  SET_FLAG_FROM_TEST(visibility_flag, !use_glossy, OB_HIDE_GLOSSY);

  Light &light = light_map_.lookup_or_add_default(key);
  light.used = true;
  light.sync(inst_.shadows,
             float4x4::identity(),
             visibility_flag,
             &la,
             nullptr,
             1.0f,
             light_threshold_,
             0);

  sun_lights_len_ += 1;
}

void LightModule::begin_sync()
{
  if (assign_if_different(use_scene_lights_, inst_.use_scene_lights())) {
    if (inst_.is_viewport()) {
      /* Catch lookdev viewport properties updates. */
      inst_.sampling.reset();
    }
  }

  /* Disable sunlight if world has a volume shader as we consider the light cannot go through an
   * infinite opaque medium. */
  use_sun_lights_ = (inst_.world.has_volume_absorption() == false);

  /* In begin_sync so it can be animated. */
  if (assign_if_different(light_threshold_, max_ff(1e-16f, inst_.scene->eevee.light_threshold))) {
    /* All local lights need to be re-sync. */
    for (Light &light : light_map_.values()) {
      if (!ELEM(light.type, LIGHT_SUN, LIGHT_SUN_ORTHO)) {
        light.initialized = false;
      }
    }
  }

  sun_lights_len_ = 0;
  local_lights_len_ = 0;
  light_shader_materials_.clear();
  volume_light_shader_materials_.clear();
  surfel_light_shader_materials_.clear();
  uniform_light_shader_materials_.clear();
  front_light_shader_materials_.clear();
  light_shader_lights_.clear();
  front_light_shader_lights_.clear();
  volume_light_shader_lights_.clear();
  surfel_light_shader_lights_.clear();
  uniform_light_shader_lights_.clear();
  light_shader_valid_ = false;
  front_light_shader_valid_ = false;
  uniform_light_shader_valid_ = false;
  uniform_light_shader_evaluated_ = false;
  front_light_shader_missing_prepass_reported_ = false;
  front_light_shader_needed_ = false;
  has_time_dependent_light_shaders_ = false;

  if (use_sun_lights_ && inst_.world.sun_threshold() > 0.0f) {
    if (inst_.pipelines.world.use_lightpath_node()) {
      add_world_sun_light(world_sunlight_key_[WORLD_SUN_DIFFUSE], true, false);
      add_world_sun_light(world_sunlight_key_[WORLD_SUN_GLOSSY], false, true);
    }
    else {
      add_world_sun_light(world_sunlight_key_[WORLD_SUN_COMBINED], true, true);
    }
  }
}

void LightModule::sync_light(const Object *ob, ObjectHandle &handle)
{
  const blender::Light &la = DRW_object_get_data_for_drawing<const blender::Light>(*ob);
  if (use_scene_lights_ == false) {
    return;
  }

  if (use_sun_lights_ == false) {
    if (la.type == LA_SUN) {
      return;
    }
  }

  Light &light = light_map_.lookup_or_add_default(handle.object_key);
  light.used = true;
  const float light_shader_range_scale = light_nodetree_eevee_light_shader_range_scale_get(
      la.nodetree);
  if (handle.recalc != 0 || !light.initialized ||
      (is_local_light(light.type) && light.light_shader_range_scale != light_shader_range_scale))
  {
    light.initialized = true;
    light.sync(inst_.shadows,
               ob->object_to_world(),
               ob->visibility_flag,
               &la,
               ob->light_linking,
               light_shader_range_scale,
               light_threshold_,
               la.lightgroup_id);
  }
  else if (is_sun_light(light.type)) {
    /* Directional shadow sync stores view-dependent clipmap offsets in the matrix translation.
     * Restore the source object transform before exposing it to light shader nodes this frame. */
    light.object_to_world = light_object_to_world_normalized_get(ob->object_to_world());
    light.sun().direction = light_z_axis(light);
  }
  light.light_shader_index = -1;
  light.front_light_shader_index = -1;
  light.volume_light_shader_index = -1;
  light.surfel_light_shader_index = -1;
  light.uniform_light_shader_index = -1;
  auto register_light_shader = [&](const eLightShaderPipeline pipeline_type,
                                   int &r_light_shader_index,
                                   Vector<GPUMaterial *> &materials,
                                   Vector<LightData> &lights,
                                   const char *error_message) {
    GPUMaterial *gpumat = inst_.shaders.light_shader_get(
        const_cast<blender::Light *>(&la), la.nodetree, pipeline_type, false);
    if (gpumat != nullptr && GPU_material_status(gpumat) == GPU_MAT_SUCCESS &&
        GPU_material_has_light_shader_output(gpumat))
    {
      r_light_shader_index = materials.size();
      materials.append(gpumat);
      lights.append(static_cast<const LightData &>(light));
      has_time_dependent_light_shaders_ |= GPU_material_is_time_dependent(gpumat);
      inst_.manager->register_layer_attributes(gpumat);
      return;
    }
    inst_.info_append_i18n(error_message);
  };
  const LightShaderDependency light_shader_dependency =
      light_nodetree_eevee_light_shader_dependency_get(la.nodetree);
  if (light_shader_dependency == LightShaderDependency::Uniform) {
    register_light_shader(eLightShaderPipeline::Uniform,
                          light.uniform_light_shader_index,
                          uniform_light_shader_materials_,
                          uniform_light_shader_lights_,
                          "Error: Uniform custom light shader failed to compile.");
  }
  else if (light_shader_dependency == LightShaderDependency::PointDependent) {
    if (inst_.is_baking()) {
      register_light_shader(eLightShaderPipeline::Surfel,
                            light.surfel_light_shader_index,
                            surfel_light_shader_materials_,
                            surfel_light_shader_lights_,
                            "Error: Custom light shader failed to compile for volume probe "
                            "baking.");
    }
    else {
      register_light_shader(eLightShaderPipeline::Surface,
                            light.light_shader_index,
                            light_shader_materials_,
                            light_shader_lights_,
                            "Error: Custom light shader failed to compile for surface lighting.");
      register_light_shader(eLightShaderPipeline::Front,
                            light.front_light_shader_index,
                            front_light_shader_materials_,
                            front_light_shader_lights_,
                            "Error: Custom light shader failed to compile for front-layer "
                            "surface lighting.");
      register_light_shader(eLightShaderPipeline::Volume,
                            light.volume_light_shader_index,
                            volume_light_shader_materials_,
                            volume_light_shader_lights_,
                            "Error: Custom light shader failed to compile for volume lighting.");
    }
  }
  sun_lights_len_ += int(is_sun_light(light.type));
  local_lights_len_ += int(!is_sun_light(light.type));
}

void LightModule::end_sync()
{
  /** IMPORTANT: We cannot add new lights here since the shadow module already executed its
   * `end_sync`. Doing so ends up in very bad data access since the shadow data of the new light
   * will not exists on the GPU. */

  /* NOTE: We resize this buffer before removing deleted lights. */
  int lights_allocated = ceil_to_multiple_u(max_ii(light_map_.size(), 1), LIGHT_CHUNK);
  light_buf_.resize(lights_allocated);
  light_shader_index_buf_ensure_no_shader(light_shader_src_index_buf_, lights_allocated);
  light_shader_index_buf_ensure_no_shader(front_light_shader_src_index_buf_, lights_allocated);
  light_shader_index_buf_ensure_no_shader(volume_light_shader_src_index_buf_, lights_allocated);
  light_shader_index_buf_ensure_no_shader(surfel_light_shader_src_index_buf_, lights_allocated);

  /* Track light deletion. */
  /* Indices inside GPU data array. */
  int sun_lights_idx = 0;
  int local_lights_idx = sun_lights_len_;

  /* Fill GPU data with scene data. */
  auto it_end = light_map_.items().end();
  for (auto it = light_map_.items().begin(); it != it_end; ++it) {
    Light &light = (*it).value;
    /* Do not discard casters in baking mode. See WORKAROUND in `surfels_create`. */
    if (!light.used && !inst_.is_baking()) {
      light_map_.remove(it);
      continue;
    }

    int dst_idx = is_sun_light(light.type) ? sun_lights_idx++ : local_lights_idx++;
    /* Put all light data into global data SSBO. */
    light_buf_[dst_idx] = light;
    const int uniform_encoded_index = (light.uniform_light_shader_index >= 0) ?
                                          -light.uniform_light_shader_index - 2 :
                                          -1;
    light_shader_src_index_buf_[dst_idx] = (light.uniform_light_shader_index >= 0) ?
                                               uniform_encoded_index :
                                               light.light_shader_index;
    front_light_shader_src_index_buf_[dst_idx] = (light.uniform_light_shader_index >= 0) ?
                                                     uniform_encoded_index :
                                                     light.front_light_shader_index;
    volume_light_shader_src_index_buf_[dst_idx] = (light.uniform_light_shader_index >= 0) ?
                                                      uniform_encoded_index :
                                                      light.volume_light_shader_index;
    surfel_light_shader_src_index_buf_[dst_idx] = (light.uniform_light_shader_index >= 0) ?
                                                      uniform_encoded_index :
                                                      light.surfel_light_shader_index;

    /* Untag for next sync. */
    light.used = false;
  }
  const bool too_many_lights = sun_lights_len_ + local_lights_len_ > CULLING_MAX_ITEM;
  /* If exceeding the limit, just trim off the excess to avoid glitchy rendering. */
  if (too_many_lights) {
    sun_lights_len_ = min_ii(sun_lights_len_, CULLING_MAX_ITEM);
    local_lights_len_ = min_ii(local_lights_len_, CULLING_MAX_ITEM - sun_lights_len_);
    light_shader_materials_.clear();
    front_light_shader_materials_.clear();
    volume_light_shader_materials_.clear();
    surfel_light_shader_materials_.clear();
    uniform_light_shader_materials_.clear();
    light_shader_lights_.clear();
    front_light_shader_lights_.clear();
    volume_light_shader_lights_.clear();
    surfel_light_shader_lights_.clear();
    uniform_light_shader_lights_.clear();
    has_time_dependent_light_shaders_ = false;
    inst_.info_append_i18n("Error: Too many lights in the scene.");
  }
  lights_len_ = sun_lights_len_ + local_lights_len_;
  if (too_many_lights) {
    light_shader_index_buf_ensure_no_shader(light_shader_src_index_buf_, lights_len_);
    light_shader_index_buf_ensure_no_shader(front_light_shader_src_index_buf_, lights_len_);
    light_shader_index_buf_ensure_no_shader(volume_light_shader_src_index_buf_, lights_len_);
    light_shader_index_buf_ensure_no_shader(surfel_light_shader_src_index_buf_, lights_len_);
  }

  /* This scene data buffer is then immutable after this point. */
  light_buf_.push_update();
  light_shader_src_index_buf_.push_update();
  front_light_shader_src_index_buf_.push_update();
  volume_light_shader_src_index_buf_.push_update();
  surfel_light_shader_src_index_buf_.push_update();

  /* Resize to the actual number of lights after pruning. */
  lights_allocated = ceil_to_multiple_u(max_ii(lights_len_, 1), LIGHT_CHUNK);
  culling_key_buf_.resize(lights_allocated);
  culling_zdist_buf_.resize(lights_allocated);
  culling_light_buf_.resize(lights_allocated);
  light_shader_index_buf_ensure_no_shader(light_shader_index_buf_, lights_allocated);
  light_shader_index_buf_ensure_no_shader(front_light_shader_index_buf_, lights_allocated);
  light_shader_index_buf_ensure_no_shader(volume_light_shader_index_buf_, lights_allocated);
  light_shader_index_buf_ensure_no_shader(surfel_light_shader_index_buf_, lights_allocated);
  surfel_light_shader_buf_.resize(1);
  surfel_light_shader_buf_.clear_to_zero();
  uniform_light_shader_pass_sync();
  light_shader_pass_sync(inst_.render_extent_get());
  if (!front_light_shader_tx_.is_valid()) {
    constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT |
                                       GPU_TEXTURE_USAGE_SHADER_READ;
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    front_light_shader_tx_.ensure_2d_array(
        gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), 1, usage, white);
  }
  culling_extent_sync(inst_.render_extent_get());
}

void LightModule::sync_render_extent(const int2 render_extent)
{
  culling_extent_sync(render_extent);
}

void LightModule::culling_extent_sync(const int2 render_extent)
{
  int2 probe_extent = int2(inst_.sphere_probes.probe_render_extent());
  int2 max_extent = math::max(render_extent, probe_extent);
  uint word_per_tile = divide_ceil_u(max_ii(lights_len_, 1), 32);
  int2 tiles_extent;
  uint tile_size = 16;
  bool tile_size_valid = false;
  do {
    tile_size *= 2;
    tiles_extent = math::divide_ceil(max_extent, int2(tile_size));
    uint tile_count = tiles_extent.x * tiles_extent.y;
    if (tile_count > max_tile_count_threshold) {
      continue;
    }
    total_word_count_ = tile_count * word_per_tile;
    tile_size_valid = true;

  } while (total_word_count_ > max_word_count_threshold || !tile_size_valid);
  total_word_count_ = ceil_to_multiple_u(total_word_count_, 32);

  culling_data_buf_.tile_word_len = word_per_tile;
  culling_data_buf_.tile_size = tile_size;
  culling_data_buf_.tile_x_len = tiles_extent.x;
  culling_data_buf_.tile_y_len = tiles_extent.y;
  culling_data_buf_.items_count = lights_len_;
  culling_data_buf_.local_lights_len = local_lights_len_;
  culling_data_buf_.sun_lights_len = sun_lights_len_;

  culling_tile_buf_.resize(total_word_count_);

  culling_pass_sync();
  update_pass_sync();
  debug_pass_sync();
}

void LightModule::culling_pass_sync()
{
  uint safe_lights_len = max_ii(lights_len_, 1);
  uint culling_select_dispatch_size = divide_ceil_u(safe_lights_len, CULLING_SELECT_GROUP_SIZE);
  uint culling_sort_dispatch_size = divide_ceil_u(safe_lights_len, CULLING_SORT_GROUP_SIZE);
  uint culling_tile_dispatch_size = divide_ceil_u(total_word_count_, CULLING_TILE_GROUP_SIZE);

  /* NOTE: We reference the buffers that may be resized or updated later. */

  culling_ps_.init();
  {
    auto &sub = culling_ps_.sub("Select");
    sub.shader_set(inst_.shaders.static_shader_get(LIGHT_CULLING_SELECT));
    sub.bind_ubo("sunlight_buf", &inst_.world.sunlight);
    sub.bind_ssbo("light_cull_buf", &culling_data_buf_);
    sub.bind_ssbo("in_light_buf", light_buf_);
    sub.bind_ssbo("out_light_buf", culling_light_buf_);
    sub.bind_ssbo("out_zdist_buf", culling_zdist_buf_);
    sub.bind_ssbo("out_key_buf", culling_key_buf_);
    sub.bind_ssbo("in_light_shader_index_buf", light_shader_src_index_buf_);
    sub.bind_ssbo("out_light_shader_index_buf", light_shader_index_buf_);
    sub.bind_ssbo("in_front_light_shader_index_buf", front_light_shader_src_index_buf_);
    sub.bind_ssbo("out_front_light_shader_index_buf", front_light_shader_index_buf_);
    sub.bind_ssbo("in_volume_light_shader_index_buf", volume_light_shader_src_index_buf_);
    sub.bind_ssbo("out_volume_light_shader_index_buf", volume_light_shader_index_buf_);
    sub.bind_ssbo("in_surfel_light_shader_index_buf", surfel_light_shader_src_index_buf_);
    sub.bind_ssbo("out_surfel_light_shader_index_buf", surfel_light_shader_index_buf_);
    sub.dispatch(int3(culling_select_dispatch_size, 1, 1));
    sub.barrier(GPU_BARRIER_SHADER_STORAGE);
  }
  {
    auto &sub = culling_ps_.sub("Sort");
    sub.shader_set(inst_.shaders.static_shader_get(LIGHT_CULLING_SORT));
    sub.bind_ssbo("light_cull_buf", &culling_data_buf_);
    sub.bind_ssbo("in_light_buf", light_buf_);
    sub.bind_ssbo("out_light_buf", culling_light_buf_);
    sub.bind_ssbo("in_zdist_buf", culling_zdist_buf_);
    sub.bind_ssbo("in_key_buf", culling_key_buf_);
    sub.bind_ssbo("in_light_shader_index_buf", light_shader_src_index_buf_);
    sub.bind_ssbo("out_light_shader_index_buf", light_shader_index_buf_);
    sub.bind_ssbo("in_front_light_shader_index_buf", front_light_shader_src_index_buf_);
    sub.bind_ssbo("out_front_light_shader_index_buf", front_light_shader_index_buf_);
    sub.bind_ssbo("in_volume_light_shader_index_buf", volume_light_shader_src_index_buf_);
    sub.bind_ssbo("out_volume_light_shader_index_buf", volume_light_shader_index_buf_);
    sub.bind_ssbo("in_surfel_light_shader_index_buf", surfel_light_shader_src_index_buf_);
    sub.bind_ssbo("out_surfel_light_shader_index_buf", surfel_light_shader_index_buf_);
    sub.dispatch(int3(culling_sort_dispatch_size, 1, 1));
    sub.barrier(GPU_BARRIER_SHADER_STORAGE);
  }
  {
    auto &sub = culling_ps_.sub("Zbin");
    sub.shader_set(inst_.shaders.static_shader_get(LIGHT_CULLING_ZBIN));
    sub.bind_ssbo("light_cull_buf", &culling_data_buf_);
    sub.bind_ssbo("light_buf", culling_light_buf_);
    sub.bind_ssbo("out_zbin_buf", culling_zbin_buf_);
    sub.dispatch(int3(1, 1, 1));
    sub.barrier(GPU_BARRIER_SHADER_STORAGE);
  }
  {
    auto &sub = culling_ps_.sub("Tiles");
    sub.shader_set(inst_.shaders.static_shader_get(LIGHT_CULLING_TILE));
    sub.bind_ssbo("light_cull_buf", &culling_data_buf_);
    sub.bind_ssbo("light_buf", culling_light_buf_);
    sub.bind_ssbo("out_light_tile_buf", culling_tile_buf_);
    sub.dispatch(int3(culling_tile_dispatch_size, 1, 1));
    sub.barrier(GPU_BARRIER_SHADER_STORAGE);
  }
}

void LightModule::update_pass_sync()
{
  /* TODO(fclem): This dispatch for all light before culling. This could be made better by
   * only running on lights that survive culling using an indirect dispatch. */
  uint safe_lights_len = max_ii(lights_len_, 1);
  uint shadow_setup_dispatch_size = divide_ceil_u(safe_lights_len, CULLING_SELECT_GROUP_SIZE);

  auto &pass = update_ps_;
  pass.init();
  pass.shader_set(inst_.shaders.static_shader_get(LIGHT_SHADOW_SETUP));
  pass.bind_ssbo("light_buf", &culling_light_buf_);
  pass.bind_ssbo("light_cull_buf", &culling_data_buf_);
  pass.bind_ssbo("tilemaps_buf", &inst_.shadows.tilemap_pool.tilemaps_data);
  pass.bind_ssbo("tilemaps_clip_buf", &inst_.shadows.tilemap_pool.tilemaps_clip);
  pass.bind_resources(inst_.uniform_data);
  pass.bind_resources(inst_.sampling);
  pass.dispatch(int3(shadow_setup_dispatch_size, 1, 1));
  pass.barrier(GPU_BARRIER_SHADER_STORAGE);
}

void LightModule::light_shader_pass_sync(const int2 extent)
{
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ;
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  light_shader_valid_ = false;
  if (light_shader_materials_.is_empty()) {
    light_shader_tx_.ensure_2d_array(
        gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), 1, usage, white);
    light_shader_fbs_.clear();
    return;
  }

  const int layer_len = light_shader_materials_.size();
  if (layer_len > GPU_max_texture_layers()) {
    light_shader_tx_.ensure_2d_array(
        gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), 1, usage, white);
    light_shader_fbs_.clear();
    light_shader_index_buf_disable_point_dependent(light_shader_index_buf_,
                                                   max_ii(lights_len_, 1));
    light_shader_index_buf_.push_update();
    inst_.info_append_i18n("Error: Too many custom light shader surface layers.");
    return;
  }
  const int lights_allocated = ceil_to_multiple_u(max_ii(layer_len, 1), LIGHT_CHUNK);
  light_shader_light_buf_.resize(lights_allocated);
  for (const int layer : light_shader_lights_.index_range()) {
    light_shader_light_buf_[layer] = light_shader_lights_[layer];
  }
  light_shader_light_buf_.push_update();

  light_shader_tx_.ensure_2d_array(
      gpu::TextureFormat::SFLOAT_16_16_16_16, math::max(extent, int2(1)), layer_len, usage);
  light_shader_tx_.ensure_layer_views();

  while (light_shader_fbs_.size() < layer_len) {
    light_shader_fbs_.append(std::make_unique<Framebuffer>("LightShader.Framebuffer"));
  }
  while (light_shader_fbs_.size() > layer_len) {
    light_shader_fbs_.remove_last();
  }
  for (const int layer : light_shader_materials_.index_range()) {
    light_shader_fbs_[layer]->ensure(
        GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE_LAYER(light_shader_tx_.layer_view(layer), 0));
  }
  light_shader_valid_ = light_shader_tx_.is_valid();
}

void LightModule::front_light_shader_pass_sync(const int2 extent)
{
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ;
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  front_light_shader_valid_ = false;
  if (front_light_shader_materials_.is_empty()) {
    front_light_shader_tx_.ensure_2d_array(
        gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), 1, usage, white);
    front_light_shader_fbs_.clear();
    return;
  }

  const int layer_len = front_light_shader_materials_.size();
  if (layer_len > GPU_max_texture_layers()) {
    front_light_shader_tx_.ensure_2d_array(
        gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), 1, usage, white);
    front_light_shader_fbs_.clear();
    disable_point_dependent_front_light_shader_indices();
    inst_.info_append_i18n("Error: Too many custom light shader front-layer surface layers.");
    return;
  }

  const int lights_allocated = ceil_to_multiple_u(max_ii(layer_len, 1), LIGHT_CHUNK);
  front_light_shader_light_buf_.resize(lights_allocated);
  for (const int layer : front_light_shader_lights_.index_range()) {
    front_light_shader_light_buf_[layer] = front_light_shader_lights_[layer];
  }
  front_light_shader_light_buf_.push_update();

  front_light_shader_tx_.ensure_2d_array(
      gpu::TextureFormat::SFLOAT_16_16_16_16, math::max(extent, int2(1)), layer_len, usage);
  front_light_shader_tx_.ensure_layer_views();

  while (front_light_shader_fbs_.size() < layer_len) {
    front_light_shader_fbs_.append(std::make_unique<Framebuffer>("FrontLightShader.Framebuffer"));
  }
  while (front_light_shader_fbs_.size() > layer_len) {
    front_light_shader_fbs_.remove_last();
  }
  for (const int layer : front_light_shader_materials_.index_range()) {
    front_light_shader_fbs_[layer]->ensure(
        GPU_ATTACHMENT_NONE,
        GPU_ATTACHMENT_TEXTURE_LAYER(front_light_shader_tx_.layer_view(layer), 0));
  }
  front_light_shader_valid_ = front_light_shader_tx_.is_valid();
}

void LightModule::uniform_light_shader_pass_sync()
{
  uniform_light_shader_valid_ = false;
  uniform_light_shader_evaluated_ = false;
  const int result_len = max_ii(uniform_light_shader_materials_.size(), 1);
  uniform_light_shader_buf_.resize(result_len);
  uniform_light_shader_buf_.clear_to_zero();

  if (uniform_light_shader_materials_.is_empty()) {
    return;
  }

  const int lights_allocated = ceil_to_multiple_u(result_len, LIGHT_CHUNK);
  uniform_light_shader_light_buf_.resize(lights_allocated);
  for (const int layer : uniform_light_shader_lights_.index_range()) {
    uniform_light_shader_light_buf_[layer] = uniform_light_shader_lights_[layer];
  }
  uniform_light_shader_light_buf_.push_update();
  uniform_light_shader_valid_ = true;
}

void LightModule::disable_point_dependent_front_light_shader_indices()
{
  light_shader_index_buf_disable_point_dependent(front_light_shader_index_buf_,
                                                 max_ii(lights_len_, 1));
  front_light_shader_index_buf_.push_update();
}

void LightModule::volume_light_shader_pass_sync(const int3 grid_size)
{
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ |
                                     GPU_TEXTURE_USAGE_SHADER_WRITE;
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  volume_light_shader_valid_ = false;
  volume_light_shader_dummy_tx_.ensure_2d_array(
      gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), 1, usage, white);
  if (volume_light_shader_materials_.is_empty()) {
    volume_light_shader_tx_.free();
    return;
  }

  const int layer_len = volume_light_shader_materials_.size() * max_ii(grid_size.z, 1);
  if (layer_len > GPU_max_texture_layers()) {
    volume_light_shader_tx_.free();
    light_shader_index_buf_disable_point_dependent(volume_light_shader_index_buf_,
                                                   max_ii(lights_len_, 1));
    volume_light_shader_index_buf_.push_update();
    inst_.info_append_i18n("Error: Too many custom light shader volume layers.");
    return;
  }

  volume_light_shader_tx_.ensure_2d_array(gpu::TextureFormat::SFLOAT_16_16_16_16,
                                          math::max(int2(grid_size), int2(1)),
                                          layer_len,
                                          usage);
  const int lights_allocated = ceil_to_multiple_u(max_ii(volume_light_shader_materials_.size(), 1),
                                                  LIGHT_CHUNK);
  volume_light_shader_light_buf_.resize(lights_allocated);
  for (const int layer : volume_light_shader_lights_.index_range()) {
    volume_light_shader_light_buf_[layer] = volume_light_shader_lights_[layer];
  }
  volume_light_shader_light_buf_.push_update();
  volume_light_shader_valid_ = volume_light_shader_tx_.is_valid();
}

void LightModule::surfel_light_shader_pass_sync(uint surfel_len)
{
  surfel_light_shader_valid_ = false;
  if (surfel_light_shader_materials_.is_empty() || surfel_len == 0) {
    surfel_light_shader_buf_.resize(1);
    surfel_light_shader_buf_.clear_to_zero();
    light_shader_index_buf_disable_point_dependent(surfel_light_shader_index_buf_,
                                                   max_ii(lights_len_, 1));
    surfel_light_shader_index_buf_.push_update();
    return;
  }

  const uint64_t light_shader_count = uint64_t(surfel_light_shader_materials_.size());
  const uint64_t result_len = light_shader_count * uint64_t(surfel_len);
  const size_t required_mem = size_t(result_len) * sizeof(float4);
  size_t max_size = GPU_max_storage_buffer_size();
  if (GPU_mem_stats_supported()) {
    int total_mem_kb, free_mem_kb;
    GPU_mem_stats_get(&total_mem_kb, &free_mem_kb);
    const size_t reserved_mem_kb = 128 * 1024;
    size_t max_alloc = total_mem_kb > int(reserved_mem_kb) ?
                           (size_t(total_mem_kb) - reserved_mem_kb) * 1024 :
                           0;
    size_t max_free = size_t((size_t(free_mem_kb) * 1024) * 0.95f);
    max_size = std::min(max_size, std::max(size_t(0), std::min(max_alloc, max_free)));
  }
  if (result_len > uint64_t(std::numeric_limits<int64_t>::max()) || required_mem > max_size) {
    surfel_light_shader_buf_.resize(1);
    surfel_light_shader_buf_.clear_to_zero();
    light_shader_index_buf_disable_point_dependent(surfel_light_shader_index_buf_,
                                                   max_ii(lights_len_, 1));
    surfel_light_shader_index_buf_.push_update();
    inst_.info_append_i18n(
        "Error: Too many custom light shader surfel bake samples ({} / {} MBytes).",
        uint(required_mem / (1024 * 1024)),
        uint(max_size / (1024 * 1024)));
    return;
  }

  surfel_light_shader_buf_.resize(int64_t(result_len));
  surfel_light_shader_buf_.clear_to_zero();

  const int lights_allocated = ceil_to_multiple_u(max_ii(surfel_light_shader_materials_.size(), 1),
                                                  LIGHT_CHUNK);
  surfel_light_shader_light_buf_.resize(lights_allocated);
  for (const int layer : surfel_light_shader_lights_.index_range()) {
    surfel_light_shader_light_buf_[layer] = surfel_light_shader_lights_[layer];
  }
  surfel_light_shader_light_buf_.push_update();
  surfel_light_shader_valid_ = true;
}

void LightModule::eval_light_shaders(View &view, const int2 extent)
{
  if (light_shader_materials_.is_empty()) {
    return;
  }

  light_shader_pass_sync(extent);
  if (!light_shader_valid_) {
    return;
  }

  for (const int layer : light_shader_materials_.index_range()) {
    PassSimple pass = {"LightShader.Pass"};
    pass.state_set(DRW_STATE_WRITE_COLOR);
    pass.framebuffer_set(&*light_shader_fbs_[layer]);
    pass.clear_color(float4(1.0f));
    pass.material_set(*inst_.manager, light_shader_materials_[layer]);
    pass.push_constant("light_index", layer);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.gbuffer);
    pass.bind_resources(inst_.hiz_buffer.front);
    pass.bind_resources(inst_.lights);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_ssbo(LIGHT_BUF_SLOT, &light_shader_light_buf_);
    pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
    inst_.manager->submit(pass, view);
  }
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
}

void LightModule::eval_front_light_shaders(View &view, const int2 extent)
{
  if (!needs_front_light_shader()) {
    return;
  }

  const bool has_prepass_normal = inst_.render_buffers.prepass_normal_tx.is_valid();
  const int2 normal_extent = has_prepass_normal ?
                                 int2(inst_.render_buffers.prepass_normal_tx.width(),
                                      inst_.render_buffers.prepass_normal_tx.height()) :
                                 int2(0);
  if (!has_prepass_normal || normal_extent != math::max(extent, int2(1))) {
    constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT |
                                       GPU_TEXTURE_USAGE_SHADER_READ;
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    front_light_shader_valid_ = false;
    front_light_shader_tx_.ensure_2d_array(
        gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), 1, usage, white);
    front_light_shader_fbs_.clear();
    disable_point_dependent_front_light_shader_indices();
    if (!front_light_shader_missing_prepass_reported_) {
      inst_.info_append_i18n(
          "Error: Point-dependent custom light shader front-layer cache needs a full-size "
          "prepass normal buffer; falling back to default surface lighting for this view.");
      front_light_shader_missing_prepass_reported_ = true;
    }
    return;
  }

  front_light_shader_pass_sync(extent);
  if (!front_light_shader_valid_) {
    disable_point_dependent_front_light_shader_indices();
    return;
  }

  for (const int layer : front_light_shader_materials_.index_range()) {
    PassSimple pass = {"FrontLightShader.Pass"};
    pass.state_set(DRW_STATE_WRITE_COLOR);
    pass.framebuffer_set(&*front_light_shader_fbs_[layer]);
    pass.clear_color(float4(1.0f));
    pass.material_set(*inst_.manager, front_light_shader_materials_[layer]);
    pass.push_constant("light_index", layer);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.hiz_buffer.front);
    pass.bind_resources(inst_.lights);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_texture(PREPASS_NORMAL_TEX_SLOT, &inst_.render_buffers.prepass_normal_tx);
    pass.bind_ssbo(LIGHT_BUF_SLOT, &front_light_shader_light_buf_);
    pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
    inst_.manager->submit(pass, view);
  }
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_FETCH);
}

void LightModule::eval_uniform_light_shaders(View &view)
{
  if (uniform_light_shader_materials_.is_empty() || !uniform_light_shader_valid_) {
    return;
  }

  if (uniform_light_shader_evaluated_) {
    return;
  }

  for (const int layer : uniform_light_shader_materials_.index_range()) {
    PassSimple pass = {"UniformLightShader.Pass"};
    pass.material_set(*inst_.manager, uniform_light_shader_materials_[layer]);
    pass.push_constant("light_index", layer);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.lights);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_ssbo(LIGHT_BUF_SLOT, &uniform_light_shader_light_buf_);
    pass.bind_ssbo(LIGHT_SHADER_UNIFORM_BUF_SLOT, &uniform_light_shader_buf_);
    pass.dispatch(int3(1, 1, 1));
    inst_.manager->submit(pass, view);
  }
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  uniform_light_shader_evaluated_ = true;
}

void LightModule::sync_volume_light_shaders(const int3 grid_size)
{
  volume_light_shader_pass_sync(grid_size);
}

void LightModule::eval_volume_light_shaders(View &view, const int3 grid_size)
{
  if (volume_light_shader_materials_.is_empty() || !volume_light_shader_valid_) {
    return;
  }
  if (!volume_light_shader_tx_.is_valid()) {
    return;
  }

  volume_light_shader_tx_.clear(float4(1.0f));

  for (const int layer : volume_light_shader_materials_.index_range()) {
    PassSimple pass = {"VolumeLightShader.Pass"};
    pass.material_set(*inst_.manager, volume_light_shader_materials_[layer]);
    pass.push_constant("light_index", layer);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.sampling);
    pass.bind_resources(inst_.lights);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_ssbo(LIGHT_BUF_SLOT, &volume_light_shader_light_buf_);
    pass.bind_image("out_light_shader_img", &volume_light_shader_tx_);
    pass.dispatch(math::divide_ceil(grid_size, int3(VOLUME_GROUP_SIZE)));
    inst_.manager->submit(pass, view);
  }
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
}

void LightModule::eval_surfel_light_shaders(View &view,
                                            draw::StorageArrayBuffer<Surfel, 64> &surfels_buf,
                                            draw::StorageBuffer<CaptureInfoData> &capture_info_buf,
                                            uint surfel_len)
{
  surfel_light_shader_pass_sync(surfel_len);
  if (surfel_light_shader_materials_.is_empty() || !surfel_light_shader_valid_) {
    return;
  }

  for (const int layer : surfel_light_shader_materials_.index_range()) {
    PassSimple pass = {"SurfelLightShader.Pass"};
    pass.material_set(*inst_.manager, surfel_light_shader_materials_[layer]);
    pass.push_constant("light_index", layer);
    pass.bind_resources(inst_.uniform_data);
    pass.bind_resources(inst_.lights);
    pass.bind_texture(RBUFS_UTILITY_TEX_SLOT, inst_.pipelines.utility_tx);
    pass.bind_ssbo(SURFEL_BUF_SLOT, &surfels_buf);
    pass.bind_ssbo(CAPTURE_BUF_SLOT, &capture_info_buf);
    pass.bind_ssbo(LIGHT_BUF_SLOT, &surfel_light_shader_light_buf_);
    pass.bind_ssbo(LIGHT_SHADER_SURFEL_BUF_SLOT, &surfel_light_shader_buf_);
    const int dispatch_len = int(divide_ceil_u(surfel_len, uint(SURFEL_GROUP_SIZE)));
    pass.dispatch(int3(dispatch_len, 1, 1));
    inst_.manager->submit(pass, view);
  }
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
}

void LightModule::debug_pass_sync()
{
  if (inst_.debug_mode == eDebugMode::DEBUG_LIGHT_CULLING) {
    debug_draw_ps_.init();
    debug_draw_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_CUSTOM);
    debug_draw_ps_.shader_set(inst_.shaders.static_shader_get(LIGHT_CULLING_DEBUG));
    debug_draw_ps_.bind_resources(inst_.uniform_data);
    debug_draw_ps_.bind_resources(inst_.hiz_buffer.front);
    debug_draw_ps_.bind_ssbo("light_buf", &culling_light_buf_);
    debug_draw_ps_.bind_ssbo("light_cull_buf", &culling_data_buf_);
    debug_draw_ps_.bind_ssbo("light_zbin_buf", &culling_zbin_buf_);
    debug_draw_ps_.bind_ssbo("light_tile_buf", &culling_tile_buf_);
    debug_draw_ps_.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
    debug_draw_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
  }
}

void LightModule::set_view(View &view, const int2 extent)
{
  float far_z = view.far_clip();
  float near_z = view.near_clip();

  culling_data_buf_.zbin_scale = -CULLING_ZBIN_COUNT / fabsf(far_z - near_z);
  culling_data_buf_.zbin_bias = -near_z * culling_data_buf_.zbin_scale;
  culling_data_buf_.tile_to_uv_fac = (culling_data_buf_.tile_size / float2(extent));
  culling_data_buf_.visible_count = 0;
  culling_data_buf_.view_is_flipped = view.is_inverted();
  culling_data_buf_.push_update();

  inst_.manager->submit(culling_ps_, view);
  inst_.manager->submit(update_ps_, view);
}

void LightModule::debug_draw(View &view, gpu::FrameBuffer *view_fb)
{
  if (inst_.debug_mode == eDebugMode::DEBUG_LIGHT_CULLING) {
    inst_.info_append("Debug Mode: Light Culling Validation");
    inst_.hiz_buffer.update();
    GPU_framebuffer_bind(view_fb);
    inst_.manager->submit(debug_draw_ps_, view);
  }
}

/** \} */

}  // namespace blender::eevee
