/* SPDX-FileCopyrightText: 2020-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_output_aov(float4 color, float value, float hash, float3 material_tag, Closure &dummy)
{
#ifdef GPU_FRAGMENT_SHADER
#  if defined(OBINFO_LIB) || defined(CREATE_INFO_draw_Infos)
  output_aov(int2(gl_FragCoord.xy),
             color,
             value,
             floatBitsToUint(hash),
             g_holdout,
             object_infos_get().flag,
             resource_id_get(), material_tag);
#  else
  output_aov(int2(gl_FragCoord.xy), color, value, floatBitsToUint(hash), 0.0f, 0u, 0u, material_tag);
#  endif
#endif
}
