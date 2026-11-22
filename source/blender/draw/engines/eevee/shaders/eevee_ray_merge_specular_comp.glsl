/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* The NPR layer combine consumes per-bin radiance, not just a final colored screen image.
 * Merge the disjoint Nuru phases before those consumers, retaining their existing semantics. */
#include "infos/eevee_ray_shared_indirect_infos.hh"
COMPUTE_SHADER_CREATE_INFO(eevee_ray_merge_specular)

#include "eevee_gbuffer_read_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, textureSize(gbuf_header_tx, 0).xy))) {
    return;
  }
  bool has_specular = false;
  for (int bin = 0; bin < 3; bin++) {
    ClosureUndetermined cl = gbuffer::read_bin(texel, bin);
    bool enabled = (cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID &&
                    (feature_mask & (1 << 2)) != 0) ||
                   (cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID &&
                    (feature_mask & (1 << 3)) != 0);
    if (!enabled) {
      continue;
    }
    has_specular = true;
    if (bin == 0) {
      imageStoreFast(indirect_0_img, texel, texelFetch(specular_0_tx, texel, 0));
    }
    else if (bin == 1) {
      imageStoreFast(indirect_1_img, texel, texelFetch(specular_1_tx, texel, 0));
    }
    else {
      imageStoreFast(indirect_2_img, texel, texelFetch(specular_2_tx, texel, 0));
    }
  }
  if (use_shared_indirect && has_specular) {
    float4 indirect = imageLoadFast(shared_indirect_img, texel);
    indirect.rgb += texelFetch(shared_specular_tx, texel, 0).rgb;
    imageStoreFast(shared_indirect_img, texel, indirect);
  }
}
