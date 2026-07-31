/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER

#  if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER) && defined(MAT_NPR_LIGHTING)

bool npr_is_zero(float3 value)
{
  return all(lessThanEqual(abs(value), float3(1e-8f)));
}

bool npr_light_linking_affects_receiver(uint2 light_set_membership, uchar receiver_light_set)
{
  return bitmask64_test(light_set_membership, receiver_light_set);
}

float npr_light_power_get(LightData light, LightingType type)
{
  /* Mask anything above 3. See LIGHT_TRANSLUCENT_WITH_THICKNESS. */
  return light.power[type & 3u];
}

bool foreach_light_setup(uint l_idx,
                         bool is_directional,
                         float3 N,
                         out float4 out_color,
                         out float3 out_vector,
                         out float out_distance,
                         out float out_attenuation,
                         out float out_shadow_mask)
{
  LightData light = light_buf[l_idx];
  if (npr_is_zero(light.color)) {
    return false;
  }

  ObjectInfos object_infos = drw_infos[drw_resource_id()];
  uchar receiver_light_set = receiver_light_set_get(object_infos);
  if (!npr_light_linking_affects_receiver(light.light_set_membership, receiver_light_set)) {
    return false;
  }

  LightVector lv = light_vector_get(light, is_directional, g_data.P);
  float attenuation = light_attenuation_volume(light, is_directional, lv);
  if (attenuation < LIGHT_ATTENUATION_THRESHOLD) {
    return false;
  }

  float3 V = drw_world_incident_vector(g_data.P);
  float4 ltc_mat = float4(1.0f, 0.0f, 0.0f, 1.0f);
  LightVertices light_shape_vertices = light_shape_corners(light, lv);
  float ltc = light_ltc(utility_tx, light, lv.L, V, lv, ltc_mat, light_shape_vertices);
  attenuation *= ltc * npr_light_power_get(light, LIGHT_DIFFUSE);
  if (attenuation < LIGHT_ATTENUATION_THRESHOLD) {
    return false;
  }

  float shadow_mask = 1.0f;
  if (light.tilemap_index != LIGHT_NO_SHADOW) {
    shadow_mask = eevee_shadow_eval(light,
                                    is_directional,
                                    false,
                                    false,
                                    0.0f,
                                    g_data.P,
                                    g_data.Ng,
                                    N,
                                    0.0f,
                                    0.0f,
                                    uniform_buf.shadow.ray_count,
                                    uniform_buf.shadow.step_count);
    shadow_mask *= dot(N, lv.L) > 0.0f ? 1.0f : 0.0f;
  }

  out_color = float4(light.color, 1.0f);
  out_vector = lv.L;
  out_distance = lv.dist;
  out_attenuation = attenuation;
  out_shadow_mask = shadow_mask;
  return true;
}

#define NPR_LIGHT_CHUNK_LOCAL 0u
#define NPR_LIGHT_CHUNK_DIRECTIONAL 1u
#define NPR_LIGHT_CHUNK_DONE 2u

struct NPRLightChunkIterator {
  uint phase;
  uint tile_word_offset;
  uint word_index;
  uint word_end;
  uint zbin_min;
  uint zbin_max;
  uint directional_index;
};

NPRLightChunkIterator npr_light_chunk_iterator_init(float2 pixel, float linear_view_z)
{
  NPRLightChunkIterator iterator;
  iterator.phase = NPR_LIGHT_CHUNK_LOCAL;
  iterator.tile_word_offset = 0u;
  iterator.word_index = 0u;
  iterator.word_end = 0u;
  iterator.zbin_min = 0u;
  iterator.zbin_max = 0u;
  iterator.directional_index = light_cull_buf.local_lights_len;

#ifdef LIGHT_ITER_FORCE_NO_CULLING
  if (light_cull_buf.visible_count == 0u) {
    iterator.phase = NPR_LIGHT_CHUNK_DIRECTIONAL;
  }
  else {
    iterator.word_end = (light_cull_buf.visible_count - 1u) >> 5u;
  }
#else
  uint2 tile_co = uint2(pixel / light_cull_buf.tile_size);
  iterator.tile_word_offset =
      (tile_co.x + tile_co.y * light_cull_buf.tile_x_len) * light_cull_buf.tile_word_len;

  int zbin_index = eevee::light::culling_z_to_zbin(
      light_cull_buf.zbin_scale, light_cull_buf.zbin_bias, linear_view_z);
  zbin_index = clamp(zbin_index, 0, CULLING_ZBIN_COUNT - 1);
  uint zbin_data = light_zbin_buf[zbin_index];
  iterator.zbin_min = zbin_data & 0xFFFFu;
  iterator.zbin_max = zbin_data >> 16u;

#  ifdef GPU_METAL
  iterator.zbin_min = simd_broadcast_first(simd_min(iterator.zbin_min));
  iterator.zbin_max = simd_broadcast_first(simd_max(iterator.zbin_max));
#  endif

  iterator.word_index = iterator.zbin_min >> 5u;
  iterator.word_end = iterator.zbin_max >> 5u;
#endif

  return iterator;
}

bool npr_light_chunk_iterator_next(NPRLightChunkIterator &iterator,
                                   out uint chunk_base,
                                   out uint chunk_word,
                                   out bool is_local)
{
  if (iterator.phase == NPR_LIGHT_CHUNK_LOCAL) {
    if (iterator.word_index <= iterator.word_end) {
      uint word_index = iterator.word_index++;
      chunk_base = word_index * 32u;

#ifdef LIGHT_ITER_FORCE_NO_CULLING
      uint local_count = min(32u, light_cull_buf.visible_count - chunk_base);
      chunk_word = eevee::light::bitfield_mask(local_count, 0u);
#else
      chunk_word = light_tile_buf[iterator.tile_word_offset + word_index];
      chunk_word &= eevee::light::zbin_mask(
          word_index, iterator.zbin_min, iterator.zbin_max);

#  ifdef GPU_METAL
      chunk_word = simd_broadcast_first(simd_or(chunk_word));
#  endif
#endif

      is_local = true;
      return true;
    }
    iterator.phase = NPR_LIGHT_CHUNK_DIRECTIONAL;
  }

  if (iterator.phase == NPR_LIGHT_CHUNK_DIRECTIONAL) {
    if (iterator.directional_index < light_cull_buf.items_count) {
      chunk_base = iterator.directional_index++;
      chunk_word = 1u;
      is_local = false;
      return true;
    }
    iterator.phase = NPR_LIGHT_CHUNK_DONE;
  }

  chunk_base = 0u;
  chunk_word = 0u;
  is_local = false;
  return false;
}

#    define FOREACH_LIGHT_BEGIN( \
        N, out_color, out_vector, out_distance, out_attenuation, out_shadow_mask) \
      { \
        NPRLightChunkIterator npr_light_iterator = npr_light_chunk_iterator_init( \
            gl_FragCoord.xy, drw_point_world_to_view(g_data.P).z); \
        uint npr_light_chunk_base; \
        uint npr_light_chunk_word; \
        bool is_local; \
        while (npr_light_chunk_iterator_next( \
            npr_light_iterator, npr_light_chunk_base, npr_light_chunk_word, is_local)) \
        { \
          int npr_light_bit_index; \
          while ((npr_light_bit_index = findLSB(npr_light_chunk_word)) != -1) { \
            npr_light_chunk_word &= ~(1u << uint(npr_light_bit_index)); \
            uint l_idx = npr_light_chunk_base + uint(npr_light_bit_index); \
      if (!foreach_light_setup(l_idx, \
                               !is_local, \
                               N, \
                               out_color, \
                               out_vector, \
                               out_distance, \
                               out_attenuation, \
                               out_shadow_mask)) \
      { \
        continue; \
      }

#    define FOREACH_LIGHT_END() \
  } \
  } \
  }

#  else

#    if !defined(FOREACH_LIGHT_BEGIN)
#      define FOREACH_LIGHT_BEGIN( \
          N, out_color, out_vector, out_distance, out_attenuation, out_shadow_mask) \
        if (false) {
#    endif

#    if !defined(FOREACH_LIGHT_END)
#      define FOREACH_LIGHT_END() }
#    endif

#  endif

#else

/**
 * Dummy functions for gpu_shader_dependency.
 * Functions need parameters to be reflected, but zones use custom IO handling.
 */
[[node]]
void FOREACH_LIGHT_BEGIN(float dummy)
{
}

[[node]]
void FOREACH_LIGHT_END(float dummy)
{
}

#endif

[[node]]
void npr_image_sample_view(TextureHandle image, float3 offset, float4 &color, float &alpha)
{
#if (defined(NPR_SHADER) || defined(MAT_FILTER)) && defined(GPU_FRAGMENT_SHADER)
  color = TextureHandle_eval(image, offset.xy, false);
  alpha = color.a;
#  if defined(MAT_FILTER)
  if (TextureHandle_stores_transmittance_alpha(image)) {
    alpha = saturate(1.0f - alpha);
  }
  if (TextureHandle_is_scene_depth(image)) {
    alpha = color.r;
  }
#  else
  if (TextureHandle_is_scene_depth(image)) {
    alpha = color.r;
  }
#  endif
#else
  color = float4(0.0f);
  alpha = 0.0f;
#endif
}

[[node]]
void npr_image_sample_texel(TextureHandle image, float3 offset, float4 &color, float &alpha)
{
#if (defined(NPR_SHADER) || defined(MAT_FILTER)) && defined(GPU_FRAGMENT_SHADER)
  color = TextureHandle_eval(image, offset.xy, true);
  alpha = color.a;
#  if defined(MAT_FILTER)
  if (TextureHandle_stores_transmittance_alpha(image)) {
    alpha = saturate(1.0f - alpha);
  }
  if (TextureHandle_is_scene_depth(image)) {
    alpha = color.r;
  }
#  else
  if (TextureHandle_is_scene_depth(image)) {
    alpha = color.r;
  }
#  endif
#else
  color = float4(0.0f);
  alpha = 0.0f;
#endif
}

[[node]]
void npr_image_sample_uv(TextureHandle image, float3 uv, float4 &color, float &alpha)
{
#if (defined(NPR_SHADER) || defined(MAT_FILTER)) && defined(GPU_FRAGMENT_SHADER)
  color = TextureHandle_eval_uv(image, uv.xy);
  alpha = color.a;
#  if defined(MAT_FILTER)
  if (TextureHandle_stores_transmittance_alpha(image)) {
    alpha = saturate(1.0f - alpha);
  }
  if (TextureHandle_is_scene_depth(image)) {
    alpha = color.r;
  }
#  else
  if (TextureHandle_is_scene_depth(image)) {
    alpha = color.r;
  }
#  endif
#else
  color = float4(0.0f);
  alpha = 0.0f;
#endif
}

[[node]]
void npr_input(TextureHandle &combined_color,
               TextureHandle &diffuse_color,
               TextureHandle &diffuse_direct,
               TextureHandle &diffuse_indirect,
               TextureHandle &specular_color,
               TextureHandle &specular_direct,
               TextureHandle &specular_indirect,
               TextureHandle &position,
               TextureHandle &normal)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  npr_input_impl(combined_color,
                 diffuse_color,
                 diffuse_direct,
                 diffuse_indirect,
                 specular_color,
                 specular_direct,
                 specular_indirect,
                 position,
                 normal);
#else
  combined_color = TEXTURE_HANDLE_DEFAULT;
  diffuse_color = TEXTURE_HANDLE_DEFAULT;
  diffuse_direct = TEXTURE_HANDLE_DEFAULT;
  diffuse_indirect = TEXTURE_HANDLE_DEFAULT;
  specular_color = TEXTURE_HANDLE_DEFAULT;
  specular_direct = TEXTURE_HANDLE_DEFAULT;
  specular_indirect = TEXTURE_HANDLE_DEFAULT;
  position = TEXTURE_HANDLE_DEFAULT;
  normal = TEXTURE_HANDLE_DEFAULT;
#endif
}

[[node]]
void npr_output(float4 color, float4 &out_color)
{
  out_color = color;
}

[[node]]
void npr_output_float(float color, float4 &out_color)
{
  out_color = float4(float3(color), 1.0f);
}

[[node]]
void npr_output_vec3(float3 color, float4 &out_color)
{
  out_color = float4(color, 1.0f);
}

[[node]]
void npr_output_texture_handle(TextureHandle color, float4 &out_color)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  out_color = TextureHandle_eval(color, float2(0.0f), false);
#else
  out_color = float4(0.0f);
#endif
}

[[node]]
void npr_refraction(TextureHandle &combined_color, TextureHandle &position)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  npr_refraction_impl(combined_color, position);
#else
  combined_color = TEXTURE_HANDLE_DEFAULT;
  position = TEXTURE_HANDLE_DEFAULT;
#endif
}

[[node]]
void node_filter_graph_input(float index, TextureHandle &image)
{
#if defined(MAT_FILTER)
  image = TextureHandle(TEX_HANDLE_FILTER_GRAPH_INPUT, int(index));
#else
  image = TEXTURE_HANDLE_DEFAULT;
#endif
}

[[node]]
void node_input_aov(float hash, TextureHandle &color, TextureHandle &value)
{
#if (defined(NPR_SHADER) || defined(MAT_FILTER)) && defined(GPU_FRAGMENT_SHADER)
  input_aov_impl(floatBitsToUint(hash), color, value);
#else
  color = TEXTURE_HANDLE_DEFAULT;
  value = TEXTURE_HANDLE_DEFAULT;
#endif
}
