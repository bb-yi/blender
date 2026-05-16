/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * This pass load Gbuffer data and output a stencil of pixel to process for each
 * lighting complexity.
 */

#include "infos/eevee_deferred_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_deferred_tile_classify)

#include "eevee_gbuffer_lib.glsl"

void main()
{
  gbuffer::Header header = gbuffer::Header::from_data(in_gbuffer_header);
  int closure_count_bits = int(header.closure_len()) << 4;
  int is_transmission = 0;
  if (header.has_transmission()) {
    is_transmission = 1 << 6;
  }

#if defined(GPU_ARB_shader_stencil_export) || defined(GPU_METAL)
  gl_FragStencilRefARB = closure_count_bits | is_transmission;
#else
  /* Instead of setting the stencil at once, we do it (literally) bit by bit.
   * Discard fragments that do not have a number of closure whose bit-pattern
   * overlap the current stencil un-masked bit. */
  if ((current_bit & (closure_count_bits | is_transmission)) == 0) {
    gpu_discard_fragment();
    return;
  }
#endif
}
