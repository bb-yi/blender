/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

bool glsl_light_is_zero(float3 value)
{
  return all(lessThanEqual(abs(value), float3(1e-8f)));
}

float glsl_light_shape_radiance(LightData light)
{
  if (light.type == LIGHT_RECT || light.type == LIGHT_ELLIPSE) {
    float area = light.area().size.x * light.area().size.y * 4.0f;
    if (light.type == LIGHT_ELLIPSE) {
      area *= M_PI / 4.0f;
    }
    return float(M_1_PI) / area;
  }

  if (light.type == LIGHT_OMNI_SPHERE || light.type == LIGHT_OMNI_DISK ||
      light.type == LIGHT_SPOT_SPHERE || light.type == LIGHT_SPOT_DISK)
  {
    float area = float(4.0f * M_PI) * square(light.local().local.shape_radius);
    return 1.0f / (area * float(M_PI));
  }

  if (is_sun_light(light.type)) {
    float inv_sin_sq = 1.0f + 1.0f / square(light.sun().shape_radius);
    return float(M_1_PI) * inv_sin_sq;
  }

  return 1.0f;
}

float glsl_light_point_radiance(LightData light)
{
  if (light.type == LIGHT_RECT || light.type == LIGHT_ELLIPSE) {
    float area = light.area().size.x * light.area().size.y * 4.0f;
    float tmp = M_PI_2 / (M_PI_2 + sqrt(area));
    float mrp_scaling = tmp + (1.0f - tmp) * M_1_PI;
    return float(M_1_PI) * mrp_scaling;
  }

  if (light.type == LIGHT_OMNI_SPHERE || light.type == LIGHT_OMNI_DISK ||
      light.type == LIGHT_SPOT_SPHERE || light.type == LIGHT_SPOT_DISK)
  {
    return float(1.0f / (4.0f * M_PI));
  }

  if (is_sun_light(light.type)) {
    return 1.0f;
  }

  return 1.0f;
}

float glsl_light_friendly_power(LightData light, LightingType type)
{
  float shape_power = glsl_light_shape_radiance(light);
  float point_power = glsl_light_point_radiance(light);
  if (shape_power <= 1e-16f) {
    return 0.0f;
  }
  return light_power_get(light, type) * (point_power / shape_power);
}

float3 glsl_light_resolve_normal(float3 normal_value)
{
  float normal_len_squared = dot(normal_value, normal_value);
  if (normal_len_squared > 1e-16f) {
    return normal_value * inversesqrt(normal_len_squared);
  }

  float geom_len_squared = dot(g_data.Ng, g_data.Ng);
  if (geom_len_squared > 1e-16f) {
    return g_data.Ng * inversesqrt(geom_len_squared);
  }

  return float3(0.0f, 0.0f, 1.0f);
}

#define GLSL_LIGHT_TYPE_INVALID 0
#define GLSL_LIGHT_TYPE_SUN 1
#define GLSL_LIGHT_TYPE_POINT 2
#define GLSL_LIGHT_TYPE_SPOT 3
#define GLSL_LIGHT_TYPE_AREA_RECT 4
#define GLSL_LIGHT_TYPE_AREA_ELLIPSE 5

struct GLSLLight {
  bool valid;
  uint index;
  int type;
  int lightgroup_id;
  float3 vector;
  float3 position;
  float3 direction;
  float distance;
  float3 diffuse_color;
  float3 specular_color;
  float attenuation;
};

GLSLLight glsl_light_default()
{
  GLSLLight light;
  light.valid = false;
  light.index = 0u;
  light.type = GLSL_LIGHT_TYPE_INVALID;
  light.lightgroup_id = 0;
  light.vector = float3(0.0f, 0.0f, 1.0f);
  light.position = float3(0.0f);
  light.direction = float3(0.0f);
  light.distance = 0.0f;
  light.diffuse_color = float3(0.0f);
  light.specular_color = float3(0.0f);
  light.attenuation = 0.0f;
  return light;
}

#if defined(GPU_FRAGMENT_SHADER) && defined(MAT_GLSL_LIGHT_ACCESS) && \
    (defined(MAT_DEFERRED) || defined(MAT_FORWARD))

bool glsl_light_index_matches_locality(uint light_index, bool is_local)
{
  bool expected_is_local = light_index < light_cull_buf.local_lights_len;
  return expected_is_local == is_local;
}

bool glsl_light_lookup(uint light_index,
                       bool is_local,
                       out LightData light,
                       out LightVector light_vector,
                       out bool is_directional)
{
  if ((light_index >= light_cull_buf.items_count) ||
      !glsl_light_index_matches_locality(light_index, is_local))
  {
    return false;
  }

  light = light_buf[light_index];
  if (glsl_light_is_zero(light.color)) {
    return false;
  }

  ObjectInfos object_infos = drw_infos[drw_resource_id()];
  uchar receiver_light_set = receiver_light_set_get(object_infos);
  if (!light_linking_affects_receiver(light.light_set_membership, receiver_light_set)) {
    return false;
  }

  is_directional = !is_local;
  light_vector = light_vector_get(light, is_directional, g_data.P);
  return true;
}

bool glsl_light_loop_accept(uint light_index, bool is_local)
{
  LightData light;
  LightVector light_vector;
  bool is_directional;
  if (!glsl_light_lookup(light_index, is_local, light, light_vector, is_directional)) {
    return false;
  }

  float diffuse_power = light_power_get(light, LIGHT_DIFFUSE);
  float specular_power = light_power_get(light, LIGHT_SPECULAR);
  return max(diffuse_power, specular_power) >= LIGHT_ATTENUATION_THRESHOLD;
}

struct GLSLLightCursor {
  uint tile_word_offset;
  uint min_index;
  uint max_index;
  uint word_min;
  uint word_max;
  uint word_idx;
  uint word;
  uint light_index;
  uint state;
  bool initialized;
};

GLSLLightCursor glsl_light_cursor_state;
bool glsl_light_cached_valid = false;
int glsl_light_cached_ordinal = -1;
uint glsl_light_cached_index = 0u;
bool glsl_light_cached_is_local = false;
bool glsl_light_cached_lookup_valid = false;
LightData glsl_light_cached_light;
LightVector glsl_light_cached_light_vector;
bool glsl_light_cached_lookup_is_directional = false;
int glsl_light_cached_count = -1;

bool glsl_light_lookup_cached(uint light_index,
                              bool is_local,
                              out LightData light,
                              out LightVector light_vector,
                              out bool is_directional)
{
  if (glsl_light_cached_lookup_valid && glsl_light_cached_index == light_index &&
      glsl_light_cached_is_local == is_local)
  {
    light = glsl_light_cached_light;
    light_vector = glsl_light_cached_light_vector;
    is_directional = glsl_light_cached_lookup_is_directional;
    return true;
  }

  bool ok = glsl_light_lookup(light_index, is_local, light, light_vector, is_directional);
  if (ok) {
    glsl_light_cached_lookup_valid = true;
    glsl_light_cached_index = light_index;
    glsl_light_cached_is_local = is_local;
    glsl_light_cached_light = light;
    glsl_light_cached_light_vector = light_vector;
    glsl_light_cached_lookup_is_directional = is_directional;
  }
  return ok;
}

void glsl_light_cursor_begin()
{
  uint2 tile_co = uint2(gl_FragCoord.xy / light_cull_buf.tile_size);
  glsl_light_cursor_state.tile_word_offset = (tile_co.x + tile_co.y * light_cull_buf.tile_x_len) *
                                             light_cull_buf.tile_word_len;

  int zbin_index = culling_z_to_zbin(
      light_cull_buf.zbin_scale, light_cull_buf.zbin_bias, drw_point_world_to_view(g_data.P).z);
  zbin_index = clamp(zbin_index, 0, CULLING_ZBIN_COUNT - 1);
  uint zbin_data = light_zbin_buf[zbin_index];
  uint min_index = zbin_data & 0xFFFFu;
  uint max_index = zbin_data >> 16u;

  glsl_light_cursor_state.word_min = min_index >> 5u;
  glsl_light_cursor_state.word_max = max_index >> 5u;
  glsl_light_cursor_state.min_index = min_index;
  glsl_light_cursor_state.max_index = max_index;
  glsl_light_cursor_state.word_idx = glsl_light_cursor_state.word_min;
  glsl_light_cursor_state.word = 0u;
  glsl_light_cursor_state.light_index = (light_cull_buf.local_lights_len == 0u) ? 0xFFFFFFFFu :
                                                                                   (light_cull_buf.local_lights_len - 1u);
  glsl_light_cursor_state.state = 1u;
  glsl_light_cursor_state.initialized = true;
}

bool glsl_light_cursor_next(out uint r_light_index, out bool r_is_local)
{
  if (!glsl_light_cursor_state.initialized) {
    glsl_light_cursor_begin();
  }

  while (true) {
    if (glsl_light_cursor_state.state == 1u) {
      if (glsl_light_cursor_state.word_idx > glsl_light_cursor_state.word_max) {
        glsl_light_cursor_state.state = 3u;
      }
      else {
        glsl_light_cursor_state.word = light_tile_buf[glsl_light_cursor_state.tile_word_offset +
                                                      glsl_light_cursor_state.word_idx];
        glsl_light_cursor_state.word &= zbin_mask(glsl_light_cursor_state.word_idx,
                                                  glsl_light_cursor_state.min_index,
                                                  glsl_light_cursor_state.max_index);
        glsl_light_cursor_state.state = 2u;
      }
    }

    if (glsl_light_cursor_state.state == 2u) {
      int bit_index = findLSB(glsl_light_cursor_state.word);
      if (bit_index == -1) {
        glsl_light_cursor_state.word_idx++;
        glsl_light_cursor_state.state = 1u;
        continue;
      }
      glsl_light_cursor_state.word &= ~(1u << uint(bit_index));
      glsl_light_cursor_state.light_index = glsl_light_cursor_state.word_idx * 32u + uint(bit_index);
      r_light_index = glsl_light_cursor_state.light_index;
      r_is_local = true;
      return true;
    }

    if (glsl_light_cursor_state.state == 3u) {
      glsl_light_cursor_state.light_index = max(light_cull_buf.local_lights_len,
                                                glsl_light_cursor_state.light_index + 1u);
      if (glsl_light_cursor_state.light_index >= light_cull_buf.items_count) {
        return false;
      }
      r_light_index = glsl_light_cursor_state.light_index;
      r_is_local = false;
      return true;
    }
  }
}

bool glsl_light_find_ordinal(int light_ordinal, out uint r_light_index, out bool r_is_local)
{
  if (light_ordinal < 0) {
    return false;
  }

  if (glsl_light_cached_valid && glsl_light_cached_ordinal == light_ordinal) {
    r_light_index = glsl_light_cached_index;
    r_is_local = glsl_light_cached_is_local;
    return true;
  }

  int current = 0;
  if (!glsl_light_cached_valid || light_ordinal < glsl_light_cached_ordinal) {
    glsl_light_cursor_state.initialized = false;
    glsl_light_cached_valid = false;
    glsl_light_cached_lookup_valid = false;
  }
  else if (glsl_light_cached_ordinal >= 0) {
    current = glsl_light_cached_ordinal + 1;
  }

  while (glsl_light_cursor_next(r_light_index, r_is_local)) {
    LightData light;
    LightVector light_vector;
    bool is_directional;
    if (!glsl_light_lookup_cached(r_light_index, r_is_local, light, light_vector, is_directional))
    {
      continue;
    }
    float diffuse_power = light_power_get(light, LIGHT_DIFFUSE);
    float specular_power = light_power_get(light, LIGHT_SPECULAR);
    if (max(diffuse_power, specular_power) < LIGHT_ATTENUATION_THRESHOLD) {
      continue;
    }
    if (current == light_ordinal) {
      glsl_light_cached_valid = true;
      glsl_light_cached_ordinal = light_ordinal;
      glsl_light_cached_index = r_light_index;
      glsl_light_cached_is_local = r_is_local;
      return true;
    }
    current += 1;
  }
  return false;
}

int glsl_light_public_type(LightData light)
{
  if (is_sun_light(light.type)) {
    return GLSL_LIGHT_TYPE_SUN;
  }
  if (is_spot_light(light.type)) {
    return GLSL_LIGHT_TYPE_SPOT;
  }
  if (light.type == LIGHT_RECT) {
    return GLSL_LIGHT_TYPE_AREA_RECT;
  }
  if (light.type == LIGHT_ELLIPSE) {
    return GLSL_LIGHT_TYPE_AREA_ELLIPSE;
  }
  return GLSL_LIGHT_TYPE_POINT;
}

GLSLLight glsl_light_build(uint light_index, bool is_local, uint public_index)
{
  GLSLLight result = glsl_light_default();
  LightData light;
  LightVector light_vector;
  bool is_directional;
  if (!glsl_light_lookup_cached(light_index, is_local, light, light_vector, is_directional)) {
    return result;
  }

  result.valid = true;
  result.index = public_index;
  result.type = glsl_light_public_type(light);
  result.lightgroup_id = light.lightgroup_id;
  result.vector = light_vector.L;
  result.position = is_directional ? float3(0.0f) : light_position_get(light);
  if (is_directional) {
    result.direction = light.sun().direction;
  }
  else if (is_spot_light(light.type) || is_area_light(light.type)) {
    result.direction = light_z_axis(light);
  }
  else {
    result.direction = float3(0.0f);
  }
  result.distance = light_vector.dist;
  result.diffuse_color = light.color * glsl_light_friendly_power(light, LIGHT_DIFFUSE);
  result.specular_color = light.color * glsl_light_friendly_power(light, LIGHT_SPECULAR);
  result.attenuation = light_point_light(light, is_directional, light_vector) *
                       light_attenuation_surface(light, is_directional, light_vector);
  return result;
}

int glsl_light_count()
{
  if (glsl_light_cached_count >= 0) {
    return glsl_light_cached_count;
  }
  int count = 0;
  LIGHT_FOREACH_ALL_BEGIN(light_cull_buf,
                          light_zbin_buf,
                          light_tile_buf,
                          gl_FragCoord.xy,
                          drw_point_world_to_view(g_data.P).z,
                          light_index,
                          is_local)
    if (!glsl_light_loop_accept(light_index, is_local)) {
      continue;
    }
    count += 1;
  LIGHT_FOREACH_ALL_END()
  glsl_light_cached_count = count;
  return count;
}

GLSLLight glsl_light_get(int light_ordinal)
{
  uint light_index = 0u;
  bool is_local = false;
  if (!glsl_light_find_ordinal(light_ordinal, light_index, is_local)) {
    return glsl_light_default();
  }
  return glsl_light_build(light_index, is_local, uint(light_ordinal));
}

float glsl_light_shadow(int light_ordinal, float3 shading_normal)
{
  uint light_index = 0u;
  bool is_local = false;
  if (!glsl_light_find_ordinal(light_ordinal, light_index, is_local)) {
    return 0.0f;
  }

  LightData light;
  LightVector light_vector;
  bool is_directional;
  if (!glsl_light_lookup_cached(light_index, is_local, light, light_vector, is_directional)) {
    return 0.0f;
  }

  ObjectInfos object_infos = drw_infos[drw_resource_id()];
  float3 geometry_normal = glsl_light_resolve_normal(g_data.Ng);
  float3 resolved_shading_normal = glsl_light_resolve_normal(shading_normal);

  if (light.tilemap_index == LIGHT_NO_SHADOW) {
    return 1.0f;
  }

  return shadow_eval(light,
                     is_directional,
                     false,
                     false,
                     0.0f,
                     g_data.P,
                     geometry_normal,
                     resolved_shading_normal,
                     object_infos.shadow_terminator_normal_offset,
                     object_infos.shadow_terminator_geometry_offset,
                     uniform_buf.shadow.ray_count,
                     uniform_buf.shadow.step_count);
}

#else

int glsl_light_count()
{
  return 0;
}

GLSLLight glsl_light_get(int light_ordinal)
{
  UNUSED_VARS(light_ordinal);
  return glsl_light_default();
}

float glsl_light_shadow(int light_ordinal, float3 shading_normal)
{
  UNUSED_VARS(light_ordinal, shading_normal);
  return 0.0f;
}

#endif
