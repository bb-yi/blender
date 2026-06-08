/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_surf_deferred_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_stencil_value_visualize)

float3 stencil_value_color(int value)
{
  float hue = fract((float(value) + 0.93f) * 0.61803398875f);
  float3 p = abs(fract(hue + float3(0.0f, 0.6666667f, 0.3333333f)) * 6.0f - 3.0f);
  float3 rgb = clamp(p - 1.0f, float3(0.0f), float3(1.0f));
  return mix(float3(0.18f), rgb, 0.82f);
}

int digit_row_mask(int digit, int row)
{
  switch (digit) {
    case 0:
      return (row == 0 || row == 4) ? 7 : 5;
    case 1:
      return (row == 0) ? 7 : ((row == 3) ? 6 : 2);
    case 2:
      return (row == 0 || row == 2 || row == 4) ? 7 : ((row == 1) ? 4 : 1);
    case 3:
      return (row == 0 || row == 2 || row == 4) ? 7 : 1;
    case 4:
      return (row == 0 || row == 1) ? 1 : ((row == 2) ? 7 : 5);
    case 5:
      return (row == 0 || row == 2 || row == 4) ? 7 : ((row == 1) ? 1 : 4);
    case 6:
      return (row == 0 || row == 2 || row == 4) ? 7 : ((row == 1) ? 5 : 4);
    case 7:
      return (row == 4) ? 7 : 1;
    case 8:
      return (row == 0 || row == 2 || row == 4) ? 7 : 5;
    case 9:
      return (row == 0 || row == 2 || row == 4) ? 7 : ((row == 3) ? 5 : 1);
    default:
      return 0;
  }
}

float digit_cell(int digit, int cell_x, int cell_y)
{
  if (cell_x < 0 || cell_x > 2 || cell_y < 0 || cell_y > 4) {
    return 0.0f;
  }
  int mask = digit_row_mask(digit, cell_y);
  return float((mask >> (2 - cell_x)) & 1);
}

float digit_sample(int digit, float2 pixel)
{
  const float scale = 3.0f;
  if (any(lessThan(pixel, float2(0.0f))) || pixel.x >= 3.0f * scale ||
      pixel.y >= 5.0f * scale)
  {
    return 0.0f;
  }
  return digit_cell(digit, int(pixel.x / scale), int(pixel.y / scale));
}

float stencil_number_sample(int value, float2 pixel)
{
  const float scale = 3.0f;
  const float digit_width = 3.0f * scale;
  const float digit_spacing = scale;
  int tens = value / 10;
  int ones = value - tens * 10;
  if (value >= 10) {
    if (pixel.x < digit_width) {
      return digit_sample(tens, pixel);
    }
    return digit_sample(ones, pixel - float2(digit_width + digit_spacing, 0.0f));
  }
  return digit_sample(ones, pixel);
}

float stencil_number_mask(int value, float2 pixel)
{
  float mask = stencil_number_sample(value, pixel);
  mask = max(mask, stencil_number_sample(value, pixel + float2(1.0f, 0.0f)));
  mask = max(mask, stencil_number_sample(value, pixel + float2(-1.0f, 0.0f)));
  mask = max(mask, stencil_number_sample(value, pixel + float2(0.0f, 1.0f)));
  mask = max(mask, stencil_number_sample(value, pixel + float2(0.0f, -1.0f)));
  return mask;
}

void main()
{
  float3 color = is_background ? float3(0.0f) : stencil_value_color(stencil_value);

  const float2 tile_size = float2(32.0f, 28.0f);
  const float scale = 3.0f;
  float label_width = stencil_value >= 10 ? 7.0f * scale : 3.0f * scale;
  float label_height = 5.0f * scale;
  float2 tile_coord = mod(gl_FragCoord.xy, tile_size);
  float2 label_coord = tile_coord - floor((tile_size - float2(label_width, label_height)) * 0.5f);

  float outline = stencil_number_mask(stencil_value, label_coord);
  float fill = stencil_number_sample(stencil_value, label_coord);
  float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
  float3 text_color = luminance > 0.45f ? float3(0.02f) : float3(1.0f);
  float3 outline_color = luminance > 0.45f ? float3(1.0f) : float3(0.02f);
  color = mix(color, outline_color, outline * 0.85f);
  color = mix(color, text_color, fill);

  gl_FragDepth = 0.0f;
  /* Combined alpha stores transmittance before Film converts it to final opacity. */
  out_color = float4(color, 0.0f);
}
