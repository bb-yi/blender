/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void npr_image_sample_view(TextureHandle image, float3 offset, float4 &color)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  color = TextureHandle_eval(image, offset.xy, false);
#else
  color = float4(0.0f);
#endif
}

[[node]]
void npr_image_sample_texel(TextureHandle image, float3 offset, float4 &color)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  color = TextureHandle_eval(image, offset.xy, true);
#else
  color = float4(0.0f);
#endif
}

[[node]]
void npr_input(TextureHandle &combined_color,
               TextureHandle &diffuse_color,
               TextureHandle &diffuse_direct,
               TextureHandle &diffuse_indirect,
               TextureHandle &specular_color,
               TextureHandle &specular_direct,
               TextureHandle &specular_indirect,
               TextureHandle &position,
               TextureHandle &normal)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  npr_input_impl(combined_color,
                 diffuse_color,
                 diffuse_direct,
                 diffuse_indirect,
                 specular_color,
                 specular_direct,
                 specular_indirect,
                 position,
                 normal);
#else
  combined_color = TEXTURE_HANDLE_DEFAULT;
  diffuse_color = TEXTURE_HANDLE_DEFAULT;
  diffuse_direct = TEXTURE_HANDLE_DEFAULT;
  diffuse_indirect = TEXTURE_HANDLE_DEFAULT;
  specular_color = TEXTURE_HANDLE_DEFAULT;
  specular_direct = TEXTURE_HANDLE_DEFAULT;
  specular_indirect = TEXTURE_HANDLE_DEFAULT;
  position = TEXTURE_HANDLE_DEFAULT;
  normal = TEXTURE_HANDLE_DEFAULT;
#endif
}

[[node]]
void npr_output(float4 color, float4 &out_color)
{
  out_color = color;
}

[[node]]
void npr_refraction(TextureHandle &combined_color, TextureHandle &position)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  npr_refraction_impl(combined_color, position);
#else
  combined_color = TEXTURE_HANDLE_DEFAULT;
  position = TEXTURE_HANDLE_DEFAULT;
#endif
}

[[node]]
void node_input_aov(float hash, TextureHandle &color, TextureHandle &value)
{
#if defined(NPR_SHADER) && defined(GPU_FRAGMENT_SHADER)
  input_aov_impl(floatBitsToUint(hash), color, value);
#else
  color = TEXTURE_HANDLE_DEFAULT;
  value = TEXTURE_HANDLE_DEFAULT;
#endif
}
