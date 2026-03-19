/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

int render_texture_slot_find(const int uid)
{
  if (uid < 0) {
    return -1;
  }

  for (int i = 0; i < RENDER_TEXTURE_SLOT_MAX; i++) {
    if ((render_texture_buf[i].info.w & RENDER_TEXTURE_SLOT_VALID) != 0 &&
        render_texture_buf[i].info.x == uid)
    {
      return i;
    }
  }

  return -1;
}

int render_texture_output_type(const int slot)
{
  return (render_texture_buf[slot].info.w & int(RENDER_TEXTURE_SLOT_SOURCE_MASK)) >>
         int(RENDER_TEXTURE_SLOT_SOURCE_SHIFT);
}

int render_texture_storage_format(const int slot)
{
  return (render_texture_buf[slot].info.w & int(RENDER_TEXTURE_SLOT_FORMAT_MASK)) >>
         int(RENDER_TEXTURE_SLOT_FORMAT_SHIFT);
}

float2 render_texture_project_uv(const float4x4 viewproj, const float3 world_position)
{
  float4 projected = viewproj * float4(world_position, 1.0f);
  if (projected.w <= 0.0f) {
    return float2(-1.0f);
  }
  return (projected.xy / projected.w) * 0.5f + 0.5f;
}

float4 render_texture_sample_current(const int slot, float2 uv)
{
  switch (slot) {
    case 0:
      return texture(render_texture_color_tx_0, uv);
    case 1:
      return texture(render_texture_color_tx_1, uv);
    case 2:
      return texture(render_texture_color_tx_2, uv);
    case 3:
      return texture(render_texture_color_tx_3, uv);
    default:
      return float4(0.0f);
  }
}

float4 render_texture_sample_history(const int slot, float2 uv)
{
  switch (slot) {
    case 0:
      return texture(render_texture_color_history_tx_0, uv);
    case 1:
      return texture(render_texture_color_history_tx_1, uv);
    case 2:
      return texture(render_texture_color_history_tx_2, uv);
    case 3:
      return texture(render_texture_color_history_tx_3, uv);
    default:
      return float4(0.0f);
  }
}

float4 render_texture_as_opacity(float4 color)
{
  color.a = saturate(1.0f - color.a);
  return color;
}

bool render_texture_alpha_is_transmittance(const int output_type)
{
  return output_type == 0 || output_type == 1;
}

bool render_texture_is_single_channel(const int storage_format)
{
  return storage_format == 2 || storage_format == 3;
}

[[node]]
void node_render_texture_none(float3 vector,
                              float use_explicit_vector,
                              float render_texture_uid,
                              float4 &color,
                              float &alpha)
{
  color = float4(0.0f);
  alpha = 0.0f;
}

[[node]]
void node_render_texture(float3 vector,
                         float use_explicit_vector,
                         float render_texture_uid,
                         float4 &color,
                         float &alpha)
{
  int slot = render_texture_slot_find(int(render_texture_uid));
  if (slot == -1) {
    color = float4(0.0f);
    alpha = 0.0f;
    return;
  }

  int output_type = render_texture_output_type(slot);
  int storage_format = render_texture_storage_format(slot);

  float2 uv = (use_explicit_vector > 0.5f) ? vector.xy :
                                         render_texture_project_uv(render_texture_buf[slot].viewproj,
                                                                   g_data.P);

  if (any(lessThan(uv, float2(0.0f))) || any(greaterThan(uv, float2(1.0f)))) {
    color = float4(0.0f);
    alpha = 0.0f;
    return;
  }

  color = ((render_texture_buf[slot].info.w & RENDER_TEXTURE_SLOT_CAPTURING) != 0) ?
              render_texture_sample_history(slot, uv) :
              render_texture_sample_current(slot, uv);

  if (render_texture_is_single_channel(storage_format)) {
    color = float4(float3(color.r), 1.0f);
  }
  else if (render_texture_alpha_is_transmittance(output_type)) {
    color = render_texture_as_opacity(color);
  }

  alpha = color.a;
}
