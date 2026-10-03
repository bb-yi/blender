/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Nuru c603d408e9f3 scene-final specular resolve, adapted to 5.2 closure payloads.
 * NPR postpass/refraction-layer consumers use the per-layer merge instead. */
#include "infos/eevee_ray_shared_indirect_infos.hh"
COMPUTE_SHADER_CREATE_INFO(eevee_ray_scene_final_specular)

#include "draw_model.bsl.hh"
#include "eevee_colorspace_lib.bsl.hh"
#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_uniform.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, imageSize(combined_img)))) {
    return;
  }
  const gbuffer::Layers gbuf = gbuffer::read_layers(texel);
  if (gbuf.header.use_object_id()) {
    [[resource_table]] const draw::Infos &infos = resource_table_get(draw::Infos);
    if (world_environment_disabled_get(infos.get(gbuffer::read_object_id(texel)))) {
      return;
    }
  }
  const uint3 bins = gbuf.header.bin_index_per_layer();
  float3 out_indirect = float3(0.0f);
  float3 specular_light = float3(0.0f);
  bool any_enabled = false;
  for (int i = 0; i < 3; i++) [[unroll]] {
    if (i < gbuf.header.closure_len()) {
      ClosureUndetermined cl = gbuf.layer[i];
      bool enabled = (cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID &&
                      (feature_mask & (1 << 2)) != 0) ||
                     (cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID &&
                      (feature_mask & (1 << 3)) != 0);
      if (enabled) {
        any_enabled = true;
        float3 radiance = bins[i] == 0u ? texelFetch(specular_0_tx, texel, 0).rgb :
                          bins[i] == 1u ? texelFetch(specular_1_tx, texel, 0).rgb :
                                          texelFetch(specular_2_tx, texel, 0).rgb;
        radiance *= cl.npr.indirect_weight;
        /* Retain NPR's existing colored render-pass convention, including its single surface
         * tint in the light pass and the two transmission events in the combined image. */
        specular_light += radiance * cl.color;
        if (cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID &&
            gbuffer::read_thickness(gbuf.header, texel).value() != 0.0f)
        {
          cl.color *= cl.color;
        }
        out_indirect += radiance * cl.color;
      }
    }
  }
  if (use_shared_indirect) {
    out_indirect = any_enabled ? texelFetch(shared_specular_tx, texel, 0).rgb : float3(0.0f);
  }
  out_indirect = colorspace::brightness_clamp_max(out_indirect, uniform_buf.clamp.surface_indirect);
  out_indirect *= uniform_buf.clamp.indirect_scale;
  if (any(isnan(out_indirect)) || any(isinf(out_indirect))) {
    out_indirect = float3(0.0f);
  }
  float4 combined = imageLoadFast(combined_img, texel);
  combined.rgb += out_indirect;
  imageStoreFast(combined_img, texel, combined);

  const int pass_id = uniform_buf.render_pass.specular_light_id;
  if (pass_id >= 0 && any_enabled) {
    specular_light = colorspace::brightness_clamp_max(specular_light,
                                                       uniform_buf.clamp.surface_indirect);
    specular_light *= uniform_buf.clamp.indirect_scale;
    float4 value = imageLoadFast(rp_color_img, int3(texel, pass_id));
    value.rgb += specular_light;
    imageStoreFast(rp_color_img, int3(texel, pass_id), value);
  }
}
