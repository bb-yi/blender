/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

#include "eevee_light_shared.hh"

namespace eevee {

struct LightRenderData {
  [[storage(LIGHT_CULL_BUF_SLOT, read)]] const LightCullingData &light_cull_buf;
  [[storage(LIGHT_BUF_SLOT, read)]] const LightData (&light_buf)[];
  [[storage(LIGHT_ZBIN_BUF_SLOT, read)]] const uint (&light_zbin_buf)[];
  [[storage(LIGHT_TILE_BUF_SLOT, read)]] const uint (&light_tile_buf)[];
  /* Metal emits scoped enum constants inside stage namespaces. */
  [[uniform(WORLD_SUNLIGHT_BUF_SLOT)]] const LightData (&sunlight_buf)[2];
};

struct LightShaderEvalData {
  [[sampler(LIGHT_SHADER_TEX_SLOT)]] sampler2DArray light_shader_tx;
  [[storage(LIGHT_SHADER_INDEX_BUF_SLOT, read)]] const int (&light_shader_index_buf)[];
  [[storage(LIGHT_SHADER_UNIFORM_BUF_SLOT, read)]] const float4 (&light_shader_uniform_buf)[];
};

struct LightShaderSurfelEvalData {
  [[storage(LIGHT_SHADER_SURFEL_INDEX_BUF_SLOT, read)]] const int (&surfel_light_shader_index_buf)[];
  [[storage(LIGHT_SHADER_SURFEL_BUF_SLOT, read)]] const float4 (&surfel_light_shader_buf)[];
  [[storage(LIGHT_SHADER_UNIFORM_BUF_SLOT, read)]] const float4 (&surfel_light_shader_uniform_buf)[];
};

}  // namespace eevee
