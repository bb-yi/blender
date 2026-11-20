/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shader that down-sample depth buffer, creating a Hierarchical-Z buffer.
 * Saves max value of each 2x2 texel in the mipmap above the one we are
 * rendering to. Adapted from
 * http://rastergrid.com/blog/2010/10/hierarchical-z-map-based-occlusion-culling/
 *
 * Major simplification has been made since we pad the buffer to always be
 * bigger than input to avoid mipmapping misalignment.
 *
 * Start by copying the base level by quad loading the depth.
 * Then each thread compute it's local depth for level 1.
 * After that we use shared variables to do inter thread communication and
 * downsample to max level.
 */

#pragma once

#include "gpu_shader_compat.hh"

namespace eevee {

/* CurrentLayerHiZ. */
struct HiZ {
  [[sampler(HIZ_TEX_SLOT)]] sampler2D hiz_tx;
};

struct PreviousLayerHiZ {
  [[sampler(HIZ_PREVIOUS_LAYER_TEX_SLOT)]] sampler2D hiz_prev_tx;
};

struct PreviousLayerRadiance {
  [[sampler(RADIANCE_PREVIOUS_LAYER_TEX_SLOT)]] sampler2D previous_layer_radiance_tx;
};

}  // namespace eevee

/* Keep legacy material GLSL independent of BSL resource-table field encoding. */
int2 eevee_hiz_extent_get()
{
  [[resource_table]] const eevee::HiZ &hiz = resource_table_get(eevee::HiZ);
  return textureSize(hiz.hiz_tx, 0);
}

float eevee_hiz_sample(float2 uv)
{
  [[resource_table]] const eevee::HiZ &hiz = resource_table_get(eevee::HiZ);
  return textureLod(hiz.hiz_tx, uv, 0.0f).r;
}

float eevee_hiz_fetch(int2 pixel)
{
  [[resource_table]] const eevee::HiZ &hiz = resource_table_get(eevee::HiZ);
  return texelFetch(hiz.hiz_tx, pixel, 0).r;
}

int2 eevee_previous_layer_extent_get()
{
  [[resource_table]] const eevee::PreviousLayerRadiance &radiance = resource_table_get(
      eevee::PreviousLayerRadiance);
  return textureSize(radiance.previous_layer_radiance_tx, 0);
}

float eevee_previous_layer_depth_fetch(int2 pixel)
{
  [[resource_table]] const eevee::PreviousLayerHiZ &hiz = resource_table_get(
      eevee::PreviousLayerHiZ);
  return texelFetch(hiz.hiz_prev_tx, pixel, 0).r;
}

float4 eevee_previous_layer_radiance_fetch(int2 pixel)
{
  [[resource_table]] const eevee::PreviousLayerRadiance &radiance = resource_table_get(
      eevee::PreviousLayerRadiance);
  return texelFetch(radiance.previous_layer_radiance_tx, pixel, 0);
}
