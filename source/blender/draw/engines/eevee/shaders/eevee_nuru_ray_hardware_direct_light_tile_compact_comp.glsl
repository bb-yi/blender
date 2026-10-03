/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Build the first-stage many-light direct RT work queue from Eevee's existing light-culling tiles.
 *
 * Each queued entry points back into the per-tile light bitmap and carries the bounded stochastic
 * sample budget chosen for that tile. Later passes can consume this queue without rescanning the
 * whole light-culling grid or inventing a second scene-wide candidate structure.
 */

#include "infos/eevee_nuru_direct_light_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_ray_hardware_direct_light_tile_compact)

#include "eevee_nuru_direct_light_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  const uint tile_idx = gl_GlobalInvocationID.x;
  const uint tile_count = light_cull_buf.tile_x_len * light_cull_buf.tile_y_len;

  if (tile_idx == 0u) {
    /* Match the existing indirect-dispatch initialization pattern. */
#if defined(GPU_INTEL) && defined(OS_WIN)
    atomicExchange(hardware_direct_light_dispatch_buf.num_groups_y, 1u);
    atomicExchange(hardware_direct_light_dispatch_buf.num_groups_z, 1u);
#else
    hardware_direct_light_dispatch_buf.num_groups_y = 1u;
    hardware_direct_light_dispatch_buf.num_groups_z = 1u;
#endif
  }

  if (tile_idx >= tile_count) {
    return;
  }

  const bool use_exact_local_lights =
      (raytrace_buf.hardware_direct_light.local_lights_len > 0u) &&
      (raytrace_buf.hardware_direct_light.local_lights_len <= 8u);
  const bool has_sun_candidates = raytrace_buf.hardware_direct_light.trace_sun_lights_separately;
  const uint tile_word_offset = tile_idx * light_cull_buf.tile_word_len;

  bool has_local_candidates = false;
  for (uint word_index = 0u; word_index < light_cull_buf.tile_word_len; word_index++) {
    has_local_candidates = has_local_candidates ||
                           (light_tile_buf[tile_word_offset + word_index] != 0u);
  }

  if (!(use_exact_local_lights || has_local_candidates || has_sun_candidates)) {
    return;
  }

  const uint tile_index = atomicAdd(hardware_direct_light_dispatch_buf.num_groups_x, 1u);
  /* Bounds proof: `hardware_direct_light_tile_capacity` is the exact host allocation size of
   * `hardware_direct_light_work_tiles_buf` (and the visibility samples buffer). The culling tile
   * grid can drift from the size captured at sync; never write past the host allocation. */
  if (tile_index >= uint(hardware_direct_light_tile_capacity)) {
    return;
  }
  const uint tile_size_px = max(raytrace_buf.hardware_direct_light.tile_size_px, 1u);
  const uint samples_per_point = max(
      raytrace_buf.hardware_direct_light.light_samples_per_shading_point, 1u);

  HardwareDirectLightWorkTile work_tile;
  work_tile.packed_tile_coord = packUvec2x16(
      uint2(tile_idx % light_cull_buf.tile_x_len, tile_idx / light_cull_buf.tile_x_len));
  work_tile.candidate_word_offset = tile_word_offset;
  work_tile.candidate_word_count = light_cull_buf.tile_word_len;
  work_tile.sample_budget = tile_size_px * tile_size_px * samples_per_point;
  hardware_direct_light_work_tiles_buf[tile_index] = work_tile;
}
