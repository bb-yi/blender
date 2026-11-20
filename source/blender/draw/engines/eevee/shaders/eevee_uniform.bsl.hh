/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_uniform_shared.hh"

namespace eevee {

struct Uniform {
  [[uniform(UNIFORM_BUF_SLOT)]] const UniformData &uniform_buf;
  [[uniform(PIPELINE_BUF_SLOT)]] const PipelineInfoData &pipeline_buf;
  [[uniform(RAYTRACE_BUF_SLOT)]] const RayTraceData &raytrace_buf;
};

}  // namespace eevee

/* Expose a value to legacy GLSL without relying on BSL's resource-table storage layout. */
UniformData eevee_uniform_data_get()
{
  [[resource_table]] const eevee::Uniform &uni = resource_table_get(eevee::Uniform);
  return uni.uniform_buf;
}
