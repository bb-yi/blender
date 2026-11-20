/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_ocio_infos.hh"

#include "gpu_shader_compositor_texture_utilities.glsl"

#include "gpu_shader_compositor_ocio_processor_lib.glsl"

COMPUTE_SHADER_CREATE_INFO(OCIO_Processor)

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float4 color = texture_load(input_tx, texel);
#ifdef OCIO_LUT
  float factor = clamp(texture_load(factor_tx, texel).x, 0.0f, 1.0f);
  if (factor > 0.0f && color.w != 0.0f) {
    float4 straight = float4(color.xyz() / color.w, 1.0f);
    float3 transformed = OCIOMain(straight).xyz() * color.w;
    color.xyz() = mix(color.xyz(), transformed, factor);
  }
#else
  color = OCIOMain(color);
  if (premultiply_output) {
    color.xyz() *= color.w;
  }
#endif
  imageStore(output_img, texel, color);
}
