/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Debug drawing passes for different system.
 * See eShadowDebug for more information.
 */
#pragma once

#include "draw_view.bsl.hh"
#include "eevee_debug_shared.hh"
#include "eevee_defines.hh"
#include "eevee_gbuffer_read.bsl.hh"
#include "eevee_hiz.bsl.hh"
#include "eevee_light_iter.bsl.hh"
#include "eevee_light_lib.bsl.hh"
#include "eevee_lightprobe_volume.bsl.hh"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_shadow.bsl.hh"
#include "eevee_shadow_shared.hh"
#include "eevee_shadow_tilemap_lib.bsl.hh"
#include "gpu_shader_debug_gradients_lib.glsl"
#include "gpu_shader_fullscreen_lib.glsl"
#include "gpu_shader_math_vector_compare_lib.glsl"

namespace eevee {

struct DebugVertOut {
  [[smooth]] float2 screen_uv;
};

[[vertex]] void debug_fullscreen_vert([[vertex_id]] const int vert_id,
                                      [[position]] float4 &out_position,
                                      [[out]] DebugVertOut &v_out)
{
  fullscreen_vertex(vert_id, out_position, v_out.screen_uv);
}

#define default_depth -1.0f

struct SearchDebugLightCtx {
  int debug_tilemap_index;
  uint light_index;

  void eval_directional([[resource_table]] LightRenderData & /*res*/, uint l_idx, LightData light)
  {
    if (light.tilemap_index == debug_tilemap_index) {
      light_index = l_idx;
    }
  }
  void eval_local([[resource_table]] LightRenderData & /*res*/, uint l_idx, LightData light)
  {
    if (light.tilemap_index == debug_tilemap_index) {
      light_index = l_idx;
    }
  }
};

template void light::foreach<SearchDebugLightCtx, LightRenderData>(const LightRenderData &,
                                                                   SearchDebugLightCtx &,
                                                                   LightRenderData &);

struct ShadowDebug {
  [[resource_table]] srt_t<LightRenderData> light_data;
  [[resource_table]] srt_t<ShadowRenderData> shadow_data;

  [[storage(5, read)]] ShadowTileMapData (&tilemaps_buf)[];
  [[storage(6, read)]] uint (&tiles_buf)[];
  [[push_constant]] int debug_mode;
  [[push_constant]] int debug_tilemap_index;
  [[push_constant]] float shadow_lod_opacity;

  ShadowSamplingTile shadow_tile_data_get(usampler2D tilemaps_tx, ShadowCoordinates coord) const
  {
    return shadow_tile_load(tilemaps_tx, coord.tilemap_tile, coord.tilemap_index);
  }

  float3 debug_random_color(int2 v) const
  {
    float r = interleaved_gradient_noise(float2(v), 0.0f, 0.0f);
    return hue_gradient(r);
  }

  float3 debug_random_color(int v) const
  {
    return debug_random_color(int2(v, 0));
  }

  void debug_tile_print([[maybe_unused]] ShadowTileData tile,
                        [[maybe_unused]] int4 tile_coord) const
  {
    /* This `printf` injection is based on string literal detection. Comment it out unless needed.
     */
    /* NOTE: using `#if 0` here causes a crash on exit for debug builds, stick to C++ comments. */
    // printf("Tile (%u, %u) in Tilemap %u: page(%u, %u, %u), cache_index %u",
    // tile_coord.x, tile_coord.y, tile_coord.z, tile.page.x, tile.page.y, tile.page.z,
    // tile.cache_index);
  }

  float3 debug_tile_state_color(ShadowTileData tile) const
  {
    if (tile.do_update && tile.is_used) {
      /* Updated. */
      return float3(0.5f, 1, 0);
    }
    if (tile.is_used) {
      /* Used but was cached. */
      return float3(0, 1, 0);
    }
    float3 col = float3(0);
    if (tile.is_cached) {
      col += float3(0.2f, 0, 0.5f);
      if (tile.do_update) {
        col += float3(0.8f, 0, 0);
      }
    }
    return col;
  }

  float3 debug_tile_lod(eLightType type, ShadowSamplingTile tile) const
  {
    if (!tile.is_valid) {
      return float3(1, 0, 0);
    }
    /* Uses data from another LOD. */
    return neon_gradient(float(tile.lod) / float((type == LIGHT_SUN) ?
                                                     SHADOW_TILEMAP_MAX_CLIPMAP_LOD :
                                                     SHADOW_TILEMAP_LOD));
  }

  int debug_shadow_effective_lod(LightData light,
                                 int tilemap_index,
                                 ShadowSamplingTile tile) const
  {
    if (is_sun_light(light.type)) {
      int tilemap_lod = light.sun().clipmap_lod_min + tilemap_index - light.tilemap_index;
      return tilemap_lod + int(tile.lod);
    }
    return int(tile.lod);
  }

  float3 debug_shadow_lod_palette(int lod) const
  {
    switch (clamp(lod, 0, 7)) {
      case 0:
        return float3(0.0f, 0.35f, 1.0f);
      case 1:
        return float3(0.0f, 0.8f, 1.0f);
      case 2:
        return float3(0.0f, 1.0f, 0.55f);
      case 3:
        return float3(0.45f, 1.0f, 0.0f);
      case 4:
        return float3(1.0f, 1.0f, 0.0f);
      case 5:
        return float3(1.0f, 0.65f, 0.0f);
      case 6:
        return float3(1.0f, 0.2f, 0.0f);
      default:
        return float3(0.65f, 0.0f, 0.0f);
    }
  }

  float3 debug_shadow_lod_color(LightData light,
                                int tilemap_index,
                                ShadowSamplingTile tile) const
  {
    if (!tile.is_valid) {
      return float3(1.0f, 0.0f, 1.0f);
    }

    int lod = debug_shadow_effective_lod(light, tilemap_index, tile);
    return debug_shadow_lod_palette(lod);
  }

  ShadowCoordinates debug_coord_get(float3 P, LightData light) const
  {
    if (is_sun_light(light.type)) {
      float3 lP = light_world_to_local_direction(light, P);
      return shadow_directional_coordinates(light, lP);
    }

    float3 lP = light_world_to_local_point(light, P);
    int face_id = shadow_punctual_face_index_get(lP);
    lP = shadow_punctual_local_position_to_face_local(face_id, lP);
    return shadow_punctual_coordinates(light, lP, face_id);
  }

  ShadowSamplingTile debug_tile_get(float3 P, LightData light) const
  {
    [[resource_table]] const ShadowRenderData &srd = shadow_data;
    ShadowCoordinates coord = debug_coord_get(P, light);
    return shadow_tile_data_get(srd.shadow_tilemaps_tx, coord);
  }

  LightData debug_light_get()
  {
    [[resource_table]] LightRenderData &lrd = light_data;

    SearchDebugLightCtx ctx = {
        .debug_tilemap_index = this->debug_tilemap_index,
        .light_index = 0,
    };

    light::foreach(lrd, ctx, lrd);

    return lrd.light_buf[ctx.light_index];
  }
};

struct DualBlendFragOut {
  [[frag_color(0), index(0)]] float4 color_add;
  [[frag_color(0), index(1)]] float4 color_mul;
};

struct ShadowDebugOutput {
  bool valid;
  float4 color_add;
  float4 color_mul;
  float depth;
};

ShadowDebugOutput debug_shadow_color_output(float3 color)
{
  return {.valid = true,
          .color_add = float4(color, 0.0f),
          .color_mul = float4(0.0f),
          .depth = 1.0f};
}

bool debug_digit_glyph(int digit, int2 p)
{
  if (any(lessThan(p, int2(0))) || any(greaterThanEqual(p, int2(5, 7)))) {
    return false;
  }

  int mask = 0;
  switch (digit) {
    case 0:
      mask = 0x3f;
      break;
    case 1:
      mask = 0x06;
      break;
    case 2:
      mask = 0x5b;
      break;
    case 3:
      mask = 0x4f;
      break;
    case 4:
      mask = 0x66;
      break;
    case 5:
      mask = 0x6d;
      break;
    case 6:
      mask = 0x7d;
      break;
    case 7:
      mask = 0x07;
      break;
    case 8:
      mask = 0x7f;
      break;
    case 9:
      mask = 0x6f;
      break;
  }

  bool segment_a = (p.y == 6) && (p.x > 0) && (p.x < 4);
  bool segment_b = (p.x == 4) && (p.y > 3) && (p.y < 6);
  bool segment_c = (p.x == 4) && (p.y > 0) && (p.y < 3);
  bool segment_d = (p.y == 0) && (p.x > 0) && (p.x < 4);
  bool segment_e = (p.x == 0) && (p.y > 0) && (p.y < 3);
  bool segment_f = (p.x == 0) && (p.y > 3) && (p.y < 6);
  bool segment_g = (p.y == 3) && (p.x > 0) && (p.x < 4);
  return (((mask & 0x01) != 0) && segment_a) || (((mask & 0x02) != 0) && segment_b) ||
         (((mask & 0x04) != 0) && segment_c) || (((mask & 0x08) != 0) && segment_d) ||
         (((mask & 0x10) != 0) && segment_e) || (((mask & 0x20) != 0) && segment_f) ||
         (((mask & 0x40) != 0) && segment_g);
}

bool debug_letter_glyph(int letter, int2 p)
{
  if (any(lessThan(p, int2(0))) || any(greaterThanEqual(p, int2(5, 7)))) {
    return false;
  }

  switch (letter) {
    case 0: /* L */
      return (p.x == 0) || (p.y == 0);
    case 1: /* D */
      return (p.x == 0) || ((p.y == 0 || p.y == 6) && p.x < 4) ||
             (p.x == 4 && p.y > 0 && p.y < 6);
    case 2: /* N */
      return (p.x == 0) || (p.x == 4) || (p.x == (p.y * 4) / 6);
    case 3: /* F */
      return (p.x == 0) || (p.y == 6) || (p.y == 3 && p.x < 4);
    case 4: /* X */
      return (p.x == (p.y * 4) / 6) || (p.x == 4 - (p.y * 4) / 6);
    case 5: /* Y */
      return ((p.y >= 3) &&
              ((p.x == (p.y * 4) / 6) || (p.x == 4 - (p.y * 4) / 6))) ||
             ((p.y < 3) && p.x == 2);
    case 6: /* Z */
      return (p.y == 0) || (p.y == 6) || (p.x == (p.y * 4) / 6);
    case 7: /* O */
      return (p.x == 0) || (p.x == 4) || (p.y == 0) || (p.y == 6);
    case 8: /* E */
      return (p.x == 0) || (p.y == 0) || (p.y == 3) || (p.y == 6);
    case 9: /* P */
      return (p.x == 0) || (p.y == 6) || (p.y == 3) ||
             (p.x == 4 && p.y > 3 && p.y < 6);
    case 10: /* T */
      return (p.y == 6) || (p.x == 2);
    case 11: /* H */
      return (p.x == 0) || (p.x == 4) || (p.y == 3);
  }
  return false;
}

bool debug_sign_glyph(bool is_positive, int2 p)
{
  if (any(lessThan(p, int2(0))) || any(greaterThanEqual(p, int2(5, 7)))) {
    return false;
  }
  return (p.y == 3) || (is_positive && p.x == 2 && p.y > 0 && p.y < 6);
}

bool debug_direction_label(int face, int2 p, int width)
{
  int start = (width - 11) / 2;
  bool is_positive = (face == int(X_POS)) || (face == int(Y_POS)) || (face == int(Z_POS));
  int axis_letter = (face == int(X_POS) || face == int(X_NEG)) ? 4 :
                    (face == int(Y_POS) || face == int(Y_NEG)) ? 5 :
                                                                  6;
  return debug_sign_glyph(is_positive, p - int2(start, 2)) ||
         debug_letter_glyph(axis_letter, p - int2(start + 6, 2));
}

int debug_direction_face_from_slot(int slot)
{
  switch (slot) {
    case 0:
      return int(X_POS);
    case 1:
      return int(X_NEG);
    case 2:
      return int(Y_POS);
    case 3:
      return int(Y_NEG);
    case 4:
      return int(Z_POS);
    default:
      return int(Z_NEG);
  }
}

bool debug_sun_level_label(int level, int2 p, int width)
{
  int value = abs(level);
  bool has_sign = level < 0;
  bool has_tens = value >= 10;
  int character_count = 2 + int(has_sign) + int(has_tens);
  int cursor = (width - (character_count * 6 - 1)) / 2;

  bool glyph = debug_letter_glyph(0, p - int2(cursor, 2));
  cursor += 6;
  if (has_sign) {
    glyph = glyph || debug_sign_glyph(false, p - int2(cursor, 2));
    cursor += 6;
  }
  if (has_tens) {
    glyph = glyph || debug_digit_glyph((value / 10) % 10, p - int2(cursor, 2));
    cursor += 6;
  }
  return glyph || debug_digit_glyph(value % 10, p - int2(cursor, 2));
}

bool debug_number_label(int value, int2 p, int width)
{
  int digit_count = (value >= 1000) ? 4 : (value >= 100) ? 3 : (value >= 10) ? 2 : 1;
  int cursor = (width - (digit_count * 6 - 1)) / 2;
  int divisor = (digit_count == 4) ? 1000 : (digit_count == 3) ? 100 :
                                               (digit_count == 2) ? 10 :
                                                                    1;
  bool glyph = false;
  for (int i = 0; i < 4; i++) {
    if (i >= digit_count) {
      break;
    }
    glyph = glyph || debug_digit_glyph((value / divisor) % 10, p - int2(cursor, 0));
    cursor += 6;
    divisor = max(divisor / 10, 1);
  }
  return glyph;
}

bool debug_lod_legend_label(int lod, int2 p, int width)
{
  int start = (width - 11) / 2;
  return debug_letter_glyph(0, p - int2(start, 0)) ||
         debug_digit_glyph(lod, p - int2(start + 6, 0));
}

bool debug_row_label(bool is_depth_row, int2 p, int width)
{
  if (!is_depth_row) {
    int start = (width - 17) / 2;
    return debug_letter_glyph(0, p - int2(start, 0)) ||
           debug_letter_glyph(7, p - int2(start + 6, 0)) ||
           debug_letter_glyph(1, p - int2(start + 12, 0));
  }

  int start = (width - 29) / 2;
  return debug_letter_glyph(1, p - int2(start, 0)) ||
         debug_letter_glyph(8, p - int2(start + 6, 0)) ||
         debug_letter_glyph(9, p - int2(start + 12, 0)) ||
         debug_letter_glyph(10, p - int2(start + 18, 0)) ||
         debug_letter_glyph(11, p - int2(start + 24, 0));
}

ShadowDebugOutput debug_shadow_lod_legend([[resource_table]] const ShadowDebug &srt,
                                         int2 texel,
                                         int viewport_width)
{
  constexpr int legend_y = 2;
  constexpr int legend_height = 24;
  constexpr int swatch_width = 28;
  constexpr int swatch_count = 9;
  constexpr int depth_width = 96;
  constexpr int right_margin = 14;
  constexpr int legend_width = swatch_width * swatch_count + 8 + depth_width;
  int legend_x = max(viewport_width - right_margin - legend_width, 0);

  if (texel.y < legend_y || texel.y >= legend_y + legend_height) {
    return {false, float4(0.0f), float4(1.0f), 1.0f};
  }

  int local_x = texel.x - legend_x;
  if (local_x >= 0 && local_x < swatch_width * swatch_count) {
    int swatch = local_x / swatch_width;
    int2 p = int2(local_x % swatch_width, texel.y - legend_y);
    bool border = (p.x == 0) || (p.x == swatch_width - 1) || (p.y == 0) ||
                  (p.y == legend_height - 1);
    float3 color = (swatch == 8) ? float3(1.0f, 0.0f, 1.0f) :
                                   srt.debug_shadow_lod_palette(swatch);
    if (border) {
      color = float3(0.08f);
    }
    else {
      bool glyph = false;
      if (swatch == 8) {
        glyph = debug_letter_glyph(4, p - int2((swatch_width - 5) / 2, 8));
      }
      else {
        int resolution = SHADOW_MAP_MAX_RES >> swatch;
        glyph = debug_lod_legend_label(swatch, p - int2(0, 14), swatch_width) ||
                debug_number_label(resolution, p - int2(0, 3), swatch_width);
      }
      if (glyph) {
        bool use_light_text = (swatch <= 2) || (swatch >= 6);
        color = use_light_text ? float3(1.0f) : float3(0.0f);
      }
    }
    return debug_shadow_color_output(color);
  }

  int depth_x = legend_x + swatch_width * swatch_count + 8;
  int depth_local_x = texel.x - depth_x;
  if (depth_local_x >= 0 && depth_local_x < depth_width) {
    int2 p = int2(depth_local_x, texel.y - legend_y);
    bool border = (p.x == 0) || (p.x == depth_width - 1) || (p.y == 0) ||
                  (p.y == legend_height - 1);
    float value = float(depth_local_x) / float(depth_width - 1);
    float3 color = border ? float3(0.08f) : float3(value);
    if (!border) {
      if (debug_letter_glyph(2, p - int2(4, 8))) {
        color = float3(1.0f);
      }
      if (debug_letter_glyph(3, p - int2(depth_width - 9, 8))) {
        color = float3(0.0f);
      }
    }
    return debug_shadow_color_output(color);
  }

  return {false, float4(0.0f), float4(1.0f), 1.0f};
}

ShadowDebugOutput debug_shadow_lod_panels([[resource_table]] const ShadowDebug &srt,
                                         LightData light,
                                         int2 texel,
                                         int viewport_width)
{
  [[resource_table]] const ShadowRenderData &srd = srt.shadow_data;

  ShadowDebugOutput legend = debug_shadow_lod_legend(srt, texel, viewport_width);
  if (legend.valid) {
    return legend;
  }

  constexpr int label_width = 36;
  constexpr int right_margin = 14;
  constexpr int legend_height = 30;
  constexpr int label_height = 11;
  constexpr int row_gap = 2;
  int slot_count = is_sun_light(light.type) ?
                       light.sun().clipmap_lod_max - light.sun().clipmap_lod_min + 1 :
                       6;
  int panel_scale = clamp((viewport_width - label_width - right_margin) /
                              max(slot_count * SHADOW_TILEMAP_RES, 1),
                          1,
                          3);
  int panel_size = SHADOW_TILEMAP_RES * panel_scale;
  int panel_origin_x = max(viewport_width - right_margin - label_width - slot_count * panel_size,
                           0);
  int panel_x = panel_origin_x + label_width;
  int depth_y = legend_height;
  int lod_y = depth_y + panel_size + label_height + row_gap;

  bool is_depth_row = texel.y >= depth_y && texel.y < depth_y + panel_size + label_height;
  bool is_lod_row = texel.y >= lod_y && texel.y < lod_y + panel_size + label_height;
  if (!is_depth_row && !is_lod_row) {
    return {false, float4(0.0f), float4(1.0f), 1.0f};
  }

  int row_y = is_depth_row ? depth_y : lod_y;
  int local_y = texel.y - row_y;
  if (texel.x < panel_origin_x) {
    return {false, float4(0.0f), float4(1.0f), 1.0f};
  }
  if (texel.x >= panel_origin_x && texel.x < panel_x) {
    if (local_y < panel_size) {
      return {false, float4(0.0f), float4(1.0f), 1.0f};
    }
    bool glyph = debug_row_label(is_depth_row,
                                 int2(texel.x - panel_origin_x, local_y - panel_size - 2),
                                 label_width);
    return debug_shadow_color_output(glyph ? float3(1.0f) : float3(0.03f));
  }

  int slot_x = texel.x - panel_x;
  int slot = slot_x / panel_size;
  if (slot < 0 || slot >= slot_count) {
    return {false, float4(0.0f), float4(1.0f), 1.0f};
  }

  int2 panel_p = int2(slot_x % panel_size, local_y);
  int face = is_sun_light(light.type) ? slot : debug_direction_face_from_slot(slot);
  int active_count = is_sun_light(light.type) ? slot_count : light.local().tilemaps_count;
  bool slot_active = face < active_count;
  if (local_y >= panel_size) {
    float3 color = float3(0.03f);
    bool glyph = is_sun_light(light.type) ?
                     debug_sun_level_label(light.sun().clipmap_lod_min + slot,
                                           panel_p - int2(0, panel_size),
                                           panel_size) :
                     debug_direction_label(face, panel_p - int2(0, panel_size), panel_size);
    if (glyph) {
      color = slot_active ? float3(1.0f) : float3(0.35f);
    }
    return debug_shadow_color_output(color);
  }

  bool border = (panel_p.x == 0) || (panel_p.x == panel_size - 1) || (panel_p.y == 0) ||
                (panel_p.y == panel_size - 1);
  if (border) {
    return debug_shadow_color_output(float3(0.25f));
  }
  if (!slot_active) {
    return debug_shadow_color_output(float3(0.03f));
  }

  int tilemap_index = light.tilemap_index + face;
  if (!is_depth_row) {
    uint2 tile_co = uint2(panel_p / panel_scale);
    ShadowSamplingTile tile = shadow_tile_load(srd.shadow_tilemaps_tx, tile_co, tilemap_index);
    return debug_shadow_color_output(srt.debug_shadow_lod_color(light, tilemap_index, tile));
  }

  float2 tilemap_uv = (float2(panel_p) + 0.5f) / float(panel_size);
  ShadowCoordinates coord = shadow_coordinate_from_uvs(tilemap_index, tilemap_uv);
  ShadowSamplingTile tile = shadow_tile_load(
      srd.shadow_tilemaps_tx, coord.tilemap_tile, coord.tilemap_index);
  if (!tile.is_valid) {
    bool checker = (((panel_p.x / 6) + (panel_p.y / 6)) & 1) != 0;
    return debug_shadow_color_output(checker ? float3(0.18f) : float3(0.07f));
  }

  float shadow_depth = srd.read_depth(coord);
  float clip_near = orderedIntBitsToFloat(light.clip_near);
  float clip_far = orderedIntBitsToFloat(light.clip_far);
  float clip_range = max(clip_far - clip_near, 1e-8f);
  float depth_normalized = is_sun_light(light.type) ? shadow_depth / clip_range :
                                                      (shadow_depth - clip_near) / clip_range;
  return debug_shadow_color_output(float3(saturate(depth_normalized)));
}

ShadowDebugOutput debug_tilemaps([[resource_table]] const ShadowDebug &srt,
                                 float3 /*P*/,
                                 LightData light,
                                 int2 texel,
                                 bool do_debug_sample_tile)
{
  [[resource_table]] const ShadowRenderData &srd = srt.shadow_data;

  /** Control the scaling of the tile-map splat. */
  constexpr int debug_tile_size_px = 4;

  int2 px = texel / debug_tile_size_px;
  int tilemap = px.x / SHADOW_TILEMAP_RES;
  int tilemap_index = light.tilemap_index + tilemap;
  if ((px.y < SHADOW_TILEMAP_RES) && (tilemap_index <= light.tilemap_max_get())) {
    if (do_debug_sample_tile) {
      /* Debug values in the tilemap_tx. */
      uint2 tilemap_texel = shadow_tile_coord_in_atlas(uint2(px), tilemap_index);
      ShadowSamplingTile tile = shadow_sampling_tile_unpack(
          texelFetch(srd.shadow_tilemaps_tx, int2(tilemap_texel), 0).x);
      /* Leave 1 px border between tile-maps. */
      if (!any(equal(texel % (SHADOW_TILEMAP_RES * debug_tile_size_px), int2(0)))) {
        return {.valid = true,
                .color_add = float4(srt.debug_tile_lod(light.type, tile), 0.0f),
                .color_mul = float4(0.0f),
                .depth = 1.0f};
      }
    }
    else {
      /* Debug actual values in the tile-map buffer. */
      ShadowTileMapData tilemap = srt.tilemaps_buf[tilemap_index];
      int tile_index = shadow_tile_offset(
          uint2(px + SHADOW_TILEMAP_RES) % SHADOW_TILEMAP_RES, tilemap.tiles_index, 0);
      ShadowTileData tile = shadow_tile_unpack(srt.tiles_buf[tile_index]);
      /* Leave 1 px border between tile-maps. */
      if (!any(equal(texel % (SHADOW_TILEMAP_RES * debug_tile_size_px), int2(0)))) {
        return {.valid = true,
                .color_add = float4(srt.debug_tile_state_color(tile), 0.0f),
                .color_mul = float4(0.0f),
                .depth = 1.0f};
      }
    }
  }
  return {false, float4(0.0f), float4(1.0f), 1.0f};
}

ShadowDebugOutput debug_tile_state([[resource_table]] const ShadowDebug &srt,
                                   float3 P,
                                   LightData light)
{
  ShadowSamplingTile tile_samp = srt.debug_tile_get(P, light);
  ShadowCoordinates coord = srt.debug_coord_get(P, light);
  ShadowTileMapData tilemap = srt.tilemaps_buf[coord.tilemap_index];
  int tile_index = shadow_tile_offset(
      uint2(coord.tilemap_tile >> tile_samp.lod), tilemap.tiles_index, int(tile_samp.lod));
  ShadowTileData tile = shadow_tile_unpack(srt.tiles_buf[tile_index]);
  return {.valid = true,
          .color_add = float4(srt.debug_tile_state_color(tile), 0) * 0.5f,
          .color_mul = float4(0.5f),
          .depth = default_depth};
}

ShadowDebugOutput debug_atlas_values([[resource_table]] const ShadowDebug &srt,
                                     float3 P,
                                     LightData light)
{
  [[resource_table]] const ShadowRenderData &srd = srt.shadow_data;

  ShadowCoordinates coord = srt.debug_coord_get(P, light);
  float depth = srd.read_depth(coord, int(light.shadow_page_lod));
  return {
      .valid = true,
      .color_add = float4((depth == -1) ? float3(1.0f, 0.0f, 0.0f) : float3(1.0f / depth), 0.0f) *
                   0.5f,
      .color_mul = float4(0.5f),
      .depth = default_depth};
}

struct AtomicCostCtx {
  float3 P;
  float cost;

  float atomic_cost([[resource_table]] const ShadowDebug &srt, LightData light)
  {
    [[resource_table]] const ShadowRenderData &srd = srt.shadow_data;

    ShadowCoordinates coord = srt.debug_coord_get(P, light);
    uint u_cost = floatBitsToUint(srd.read_depth(coord, int(light.shadow_page_lod)));
    return float(u_cost - floatBitsToUint(FLT_MAX));
  }

  void eval_directional([[resource_table]] ShadowDebug &srt, uint /*l_idx*/, LightData light)
  {
    cost += atomic_cost(srt, light);
  }

  void eval_local([[resource_table]] ShadowDebug &srt, uint /*l_idx*/, LightData light)
  {
    cost += atomic_cost(srt, light);
  }
};

template void light::foreach<AtomicCostCtx, ShadowDebug>(const LightRenderData &,
                                                         AtomicCostCtx &,
                                                         ShadowDebug &);

ShadowDebugOutput debug_atomic_cost([[resource_table]] ShadowDebug &srt,
                                    float3 P,
                                    LightData /*light*/)
{
  AtomicCostCtx ctx = {.P = P, .cost = 0.0f};

  light::foreach(srt.light_data, ctx, srt);

  ctx.cost /= 60.0f;
  return {.valid = true,
          .color_add = float4(heatmap_gradient(ctx.cost), 0.0f),
          .color_mul = float4(0.0f),
          .depth = default_depth};
}

ShadowDebugOutput debug_random_tile_color([[resource_table]] const ShadowDebug &srt,
                                          float3 P,
                                          LightData light)
{
  ShadowSamplingTile tile = srt.debug_tile_get(P, light);
  return {.valid = true,
          .color_add = float4(srt.debug_random_color(int2(tile.page.xy)), 0) * 0.5f,
          .color_mul = float4(0.5f),
          .depth = default_depth};
}

ShadowDebugOutput debug_random_tilemap_color([[resource_table]] const ShadowDebug &srt,
                                             float3 P,
                                             LightData light)
{
  ShadowCoordinates coord = srt.debug_coord_get(P, light);
  return {.valid = true,
          .color_add = float4(srt.debug_random_color(int2(coord.tilemap_index)), 0) * 0.5f,
          .color_mul = float4(0.5f),
          .depth = default_depth};
}

ShadowDebugOutput debug_shadow_lod([[resource_table]] const ShadowDebug &srt,
                                   float3 P,
                                   LightData light)
{
  ShadowCoordinates coord = srt.debug_coord_get(P, light);
  ShadowSamplingTile tile = srt.debug_tile_get(P, light);
  if (!tile.is_valid) {
    float opacity = srt.shadow_lod_opacity;
    return {.valid = true,
            .color_add = float4(float3(1.0f, 0.0f, 1.0f) * opacity, 0.0f),
            .color_mul = float4(1.0f - opacity),
            .depth = default_depth};
  }
  float3 color = srt.debug_shadow_lod_color(light, coord.tilemap_index, tile);
  float opacity = srt.shadow_lod_opacity;
  return {.valid = true,
          .color_add = float4(color * opacity, 0.0f),
          .color_mul = float4(1.0f - opacity),
          .depth = default_depth};
}

[[fragment]]
void debug_shadow_frag([[resource_table]] ShadowDebug &srt,
                       [[resource_table]] const draw::View &views,
                       [[frag_coord]] const float4 frag_co,
                       [[frag_depth(greater)]] float &frag_depth,
                       [[resource_table]] const HiZ &hiz,
                       [[in]] const DebugVertOut v_out,
                       [[out]] DualBlendFragOut &frag_out)
{
  /* Default to no output. */
  frag_out.color_add = float4(0.0f);
  frag_out.color_mul = float4(1.0f);

  const ViewMatrices view = views.get(0);

  float depth = texelFetch(hiz.hiz_tx, int2(frag_co.xy), 0).r;
  float3 P = view.point_screen_to_world(float3(v_out.screen_uv, depth));

  const LightData light = srt.debug_light_get();

  const eDebugMode mode = eDebugMode(srt.debug_mode);
  bool do_debug_sample_tile = mode != DEBUG_SHADOW_TILEMAPS;

  ShadowDebugOutput result;
  result.valid = false;
  if (mode == DEBUG_SHADOW_LOD) {
    result = debug_shadow_lod_panels(srt, light, int2(frag_co.xy), textureSize(hiz.hiz_tx, 0).x);
  }
  else if (mode != DEBUG_SHADOW_ATOMIC_COST) {
    result = debug_tilemaps(srt, P, light, int2(frag_co.xy), do_debug_sample_tile);
  }

  if (!result.valid && depth != 1.0f) {
    switch (mode) {
      case DEBUG_SHADOW_TILEMAPS:
        result = debug_tile_state(srt, P, light);
        break;
      case DEBUG_SHADOW_VALUES:
        result = debug_atlas_values(srt, P, light);
        break;
      case DEBUG_SHADOW_TILE_RANDOM_COLOR:
        result = debug_random_tile_color(srt, P, light);
        break;
      case DEBUG_SHADOW_TILEMAP_RANDOM_COLOR:
        result = debug_random_tilemap_color(srt, P, light);
        break;
      case DEBUG_SHADOW_ATOMIC_COST:
        result = debug_atomic_cost(srt, P, light);
        break;
      case DEBUG_SHADOW_LOD:
        result = debug_shadow_lod(srt, P, light);
        break;
      default:
        break;
    }
  }

  frag_out.color_add = result.color_add;
  frag_out.color_mul = result.color_mul;
  /* Make it pass the depth test by default. */
  frag_depth = result.depth == default_depth ? 1.0f - depth + 1e-6f : result.depth;
}

}  // namespace eevee

PipelineGraphic eevee_shadow_debug(eevee::debug_fullscreen_vert,
                                   eevee::debug_shadow_frag,
                                   eevee::ShadowRenderData{.shadow_random = false});

namespace eevee::debug::irradiance_grid {

struct VertOut {
  [[smooth]] float4 interp_color;
};

struct FragOut {
  [[frag_color(0)]] float4 out_color;
};

struct Resources {
  [[push_constant]] const float4x4 grid_mat;
  [[push_constant]] const int debug_mode;
  [[push_constant]] const float debug_value;

  [[sampler(0)]] sampler3D debug_data_tx;
};

[[vertex, clip_control]] void vert_main([[resource_table]] const Resources &srt,
                                        [[resource_table]] const draw::View &views,
                                        [[vertex_id]] const int vert_id,
                                        [[position]] float4 &out_position,
                                        [[point_size]] float &out_point_size,
                                        [[out]] VertOut &v_out)
{
  int3 grid_resolution = textureSize(srt.debug_data_tx, 0);
  int3 grid_sample;
  int sample_id = 0;
  if (srt.debug_mode == DEBUG_IRRADIANCE_CACHE_VALIDITY) {
    /* Points. */
    sample_id = vert_id;
  }
  else if (srt.debug_mode == DEBUG_IRRADIANCE_CACHE_VIRTUAL_OFFSET) {
    /* Lines. */
    sample_id = vert_id / 2;
  }

  grid_sample.x = (sample_id % grid_resolution.x);
  grid_sample.y = (sample_id / grid_resolution.x) % grid_resolution.y;
  grid_sample.z = (sample_id / (grid_resolution.x * grid_resolution.y));

  float3 P = eevee::lightprobe::volume::grid_sample_position(
      srt.grid_mat, grid_resolution, grid_sample);

  float4 debug_data = texelFetch(srt.debug_data_tx, grid_sample, 0);
  if (srt.debug_mode == DEBUG_IRRADIANCE_CACHE_VALIDITY) {
    v_out.interp_color = float4(1.0f - debug_data.r, debug_data.r, 0.0f, 0.0f);
    out_point_size = 3.0f;
    if (debug_data.r > srt.debug_value) {
      /* Only render points that are below threshold. */
      out_position = float4(0.0f);
      out_point_size = 0.0f;
      return;
    }
  }
  else if (srt.debug_mode == DEBUG_IRRADIANCE_CACHE_VIRTUAL_OFFSET) {
    if (is_zero(debug_data.xyz)) {
      /* Only render points that have offset. */
      out_position = float4(0.0f);
      out_point_size = 0.0f;
      return;
    }

    if ((vert_id & 1) == 1) {
      P += debug_data.xyz;
      v_out.interp_color = float4(1.0f, 0.0f, 0.0f, 0.0f);
    }
    else {
      v_out.interp_color = float4(0.0f, 0.0f, 1.0f, 0.0f);
    }
  }

  const ViewMatrices view = views.get(0);

  out_position = view.point_world_to_homogenous(P);
  out_position.z -= 2.5e-5f;
  out_point_size = 3.0f;
}

[[fragment]]
void frag_main([[in]] const VertOut &v_out, [[out]] FragOut &frag_out)
{
  frag_out.out_color = v_out.interp_color;
}

}  // namespace eevee::debug::irradiance_grid

PipelineGraphic eevee_debug_irradiance_grid(eevee::debug::irradiance_grid::vert_main,
                                            eevee::debug::irradiance_grid::frag_main);

namespace eevee::debug::surfels {

struct VertOut {
  [[smooth]] float3 P;
  [[flat]] int surfel_index;
};

struct FragOut {
  [[frag_color(0)]] float4 out_color;
};

struct Resources {
  [[storage(0, read)]] const Surfel (&surfels_buf)[];

  [[push_constant]] const float debug_surfel_radius;
  [[push_constant]] const int debug_mode;
};

[[vertex, clip_control]]
void vert_main([[resource_table]] const Resources &srt,
               [[resource_table]] const draw::View &views,
               [[vertex_id]] const int vert_id,
               [[instance_id]] const int inst_id,
               [[position]] float4 &out_position,
               [[out]] VertOut &v_out)
{
  v_out.surfel_index = inst_id;
  Surfel surfel = srt.surfels_buf[v_out.surfel_index];

#if 0 /* Debug surfel lists. TODO allow in release build with a dedicated shader. */
  if (vert_id == 0 && surfel.next > -1) {
    Surfel surfel_next = surfels_buf[surfel.next];
    float4 line_color = (surfel.prev == -1)      ? float4(1.0f, 1.0f, 0.0f, 1.0f) :
                      (surfel_next.next == -1) ? float4(0.0f, 1.0f, 1.0f, 1.0f) :
                                                 float4(0.0f, 1.0f, 0.0f, 1.0f);
    /* WORKAROUND: Avoid compilation error because this gets parsed before dead code removal. */
    drw_ debug_line(surfel_next.position, surfel.position, line_color);
  }
#endif

  float3 lP;

  switch (vert_id) {
    case 0:
      lP = float3(-1, 1, 0);
      break;
    case 1:
      lP = float3(-1, -1, 0);
      break;
    case 2:
      lP = float3(1, 1, 0);
      break;
    case 3:
      lP = float3(1, -1, 0);
      break;
  }

  float3x3 TBN = from_up_axis(surfel.normal);

  float4x4 model_matrix = float4x4(float4(TBN[0] * srt.debug_surfel_radius, 0),
                                   float4(TBN[1] * srt.debug_surfel_radius, 0),
                                   float4(TBN[2] * srt.debug_surfel_radius, 0),
                                   float4(surfel.position, 1));

  v_out.P = (model_matrix * float4(lP, 1)).xyz;

  const ViewMatrices view = views.get(0);

  out_position = reverse_z::transform(view.point_world_to_homogenous(v_out.P));
  out_position.z += 2.5e-5f;
}

float3 debug_random_color(int v)
{
  float r = interleaved_gradient_noise(float2(v, 0), 0.0f, 0.0f);
  return hue_gradient(r);
}

[[fragment]]
void frag_main([[resource_table]] const Resources &srt,
               [[front_facing]] const bool front_face,
               [[in]] const VertOut &v_out,
               [[out]] FragOut &frag_out)
{
  Surfel surfel = srt.surfels_buf[v_out.surfel_index];

  float4 radiance_vis = float4(0.0f);
  radiance_vis += front_face ? surfel.radiance_direct.front : surfel.radiance_direct.back;
  radiance_vis += front_face ? surfel.radiance_indirect[1].front :
                               surfel.radiance_indirect[1].back;

  switch (eDebugMode(srt.debug_mode)) {
    default:
    case DEBUG_IRRADIANCE_CACHE_SURFELS_NORMAL:
      frag_out.out_color = float4(pow(surfel.normal * 0.5f + 0.5f, float3(2.2f)), 0.0f);
      break;
    case DEBUG_IRRADIANCE_CACHE_SURFELS_CLUSTER:
      frag_out.out_color = float4(pow(debug_random_color(surfel.cluster_id), float3(2.2f)), 0.0f);
      break;
    case DEBUG_IRRADIANCE_CACHE_SURFELS_IRRADIANCE:
      frag_out.out_color = float4(radiance_vis.rgb, 0.0f);
      break;
    case DEBUG_IRRADIANCE_CACHE_SURFELS_VISIBILITY:
      frag_out.out_color = float4(radiance_vis.aaa, 0.0f);
      break;
  }

  /* Display surfels as circles. */
  if (distance(v_out.P, surfel.position) > srt.debug_surfel_radius) {
    gpu_discard_fragment();
    return;
  }
}

}  // namespace eevee::debug::surfels

PipelineGraphic eevee_debug_surfels(eevee::debug::surfels::vert_main,
                                    eevee::debug::surfels::frag_main);

namespace eevee::debug::gbuffer {

struct Resources {
  [[push_constant]] const int debug_mode;
};

[[fragment]]
void frag_main([[resource_table]] Resources &srt,
               [[resource_table]] const draw::View &views,
               [[resource_table]] const ::gbuffer::Reader &reader,
               [[frag_coord]] const float4 frag_co,
               [[out]] DualBlendFragOut &frag_out)
{
  int2 texel = int2(frag_co.xy);

  const ::gbuffer::Layers gbuf = reader.read_layers(texel);

  if (gbuf.has_no_closure()) {
    gpu_discard_fragment();
    return;
  }

  const ViewMatrices view = views.get(0);

  float shade = saturate(view.normal_world_to_view(gbuf.surface_N()).z);

  ::gbuffer::Header header = reader.read_header(texel);
  uint4 closure_types = (uint4(header.raw()) >> uint4(0u, 4u, 8u, 12u)) & 15u;
  float storage_cost = reduce_add(float4(not(equal(closure_types, uint4(0u)))));

  float eval_cost = 0.0f;
  for (uchar i = 0; i < GBUFFER_LAYER_MAX; i++) {
    switch (gbuf.layer[i].type) {
      case CLOSURE_BSDF_DIFFUSE_ID:
      case CLOSURE_BSDF_TRANSLUCENT_ID:
      case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
      case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID:
        eval_cost += 1.0f;
        break;
      case CLOSURE_BSSRDF_BURLEY_ID:
      case CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID:
        eval_cost += 2.0f;
        break;
      case CLOSURE_NONE_ID:
        break;
    }
  }

  switch (eDebugMode(srt.debug_mode)) {
    default:
    case DEBUG_GBUFFER_STORAGE:
      frag_out.color_add = shade * float4(green_to_red_gradient(storage_cost / 4.0f), 0.0f);
      break;
    case DEBUG_GBUFFER_EVALUATION:
      frag_out.color_add = shade * float4(green_to_red_gradient(eval_cost / 4.0f), 0.0f);
      break;
  }

  frag_out.color_mul = float4(0.0f);
}

}  // namespace eevee::debug::gbuffer

PipelineGraphic eevee_debug_gbuffer(eevee::debug_fullscreen_vert,
                                    eevee::debug::gbuffer::frag_main);

namespace eevee::debug::hiz {

/**
 * Debug hiz down sampling pass.
 * Output red if above any max pixels, blue otherwise.
 */
[[fragment]]
void frag_main([[resource_table]] const HiZ &hiz,
               [[frag_coord]] const float4 frag_co,
               [[out]] DualBlendFragOut &frag_out)
{
  int2 texel = int2(frag_co.xy);

  float depth0 = texelFetch(hiz.hiz_tx, texel, 0).r;

  float4 color = float4(0.1f, 0.1f, 1.0f, 1.0f);
  for (int i = 1; i < HIZ_MIP_COUNT; i++) {
    int2 lvl_texel = texel / int2(uint2(1) << uint(i));
    lvl_texel = min(lvl_texel, textureSize(hiz.hiz_tx, i) - 1);
    if (texelFetch(hiz.hiz_tx, lvl_texel, i).r < depth0) {
      color = float4(1.0f, 0.1f, 0.1f, 1.0f);
      break;
    }
  }
  frag_out.color_add = float4(color.rgb, 0.0f) * 0.2f;
  frag_out.color_mul = color;
}

}  // namespace eevee::debug::hiz

PipelineGraphic eevee_hiz_debug(eevee::debug_fullscreen_vert, eevee::debug::hiz::frag_main);
