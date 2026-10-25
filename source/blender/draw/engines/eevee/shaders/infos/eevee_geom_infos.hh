/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "BLI_utildefines_variadic.h"

#  include "gpu_shader_compat.hh"

#  include "draw_object_infos_infos.hh"
#  include "draw_view_infos.hh"

#  include "eevee_common_infos.hh"
#  include "eevee_uniform_shared.hh"
#endif

#ifdef GLSL_CPP_STUBS
#  define CURVES_SHADER
#  define DRW_HAIR_INFO

#  define POINTCLOUD_SHADER
#  define DRW_POINTCLOUD_INFO

#  define MAT_TRANSPARENT
#  define MAT_SHADER_TO_RGBA
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

/* Common interface */
GPU_SHADER_NAMED_INTERFACE_INFO(eevee_surf_iface, interp)
/* World Position. */
SMOOTH(float3, P)
/* Translation-free world position for mesh surface derivatives. */
SMOOTH(float3, P_relative)
/* World Normal. */
SMOOTH(float3, N)
GPU_SHADER_NAMED_INTERFACE_END(interp)

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_surf_flat_iface, interp_flat)
FLAT(uint, resource_id_raw)
GPU_SHADER_NAMED_INTERFACE_END(interp_flat)

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_surf_disp_iface, disp_interp)
/* World space position of the surface before displacement. */
SMOOTH(float3, P_base)
GPU_SHADER_NAMED_INTERFACE_END(disp_interp)

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_bake_geom_iface, bake_interp)
/* World geometric normal. */
FLAT(float3, Ng)
FLAT(int, primitive_id)
GPU_SHADER_NAMED_INTERFACE_END(bake_interp)

GPU_SHADER_CREATE_INFO(eevee_geom_bake_mesh)
TYPEDEF_SOURCE("eevee_defines.hh")
DEFINE("MAT_GEOM_MESH")
VERTEX_IN(0, float3, pos)
VERTEX_IN(1, float3, nor)
VERTEX_IN(2, float2, bake_uv)
VERTEX_IN(3, float3, geom_nor)
VERTEX_IN(4, int, primitive_id)
VERTEX_SOURCE("eevee_geom_bake_mesh_vert.glsl")
VERTEX_OUT(eevee_surf_iface)
VERTEX_OUT(eevee_surf_flat_iface)
VERTEX_OUT(eevee_bake_geom_iface)
VERTEX_OUT(eevee_surf_disp_iface)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_object_infos)
ADDITIONAL_INFO(draw_resource_id_varying)
ADDITIONAL_INFO(draw_view)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_geom_bake_mesh_basic)
TYPEDEF_SOURCE("eevee_defines.hh")
DEFINE("MAT_GEOM_MESH")
VERTEX_IN(0, float3, pos)
VERTEX_IN(1, float3, nor)
VERTEX_IN(2, float2, bake_uv)
VERTEX_IN(3, float3, geom_nor)
VERTEX_IN(4, int, primitive_id)
VERTEX_SOURCE("eevee_geom_bake_mesh_basic_vert.glsl")
VERTEX_OUT(eevee_surf_iface)
VERTEX_OUT(eevee_surf_flat_iface)
VERTEX_OUT(eevee_bake_geom_iface)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_object_infos)
ADDITIONAL_INFO(draw_resource_id_varying)
ADDITIONAL_INFO(draw_view)
GPU_SHADER_CREATE_END()

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_surf_shadow_iface, shadow_iface)
FLAT(int, shadow_view_id)
GPU_SHADER_NAMED_INTERFACE_END(shadow_iface)

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_surf_shadow_clipping_iface, shadow_clip)
SMOOTH(float3, position)
SMOOTH(float3, vector)
GPU_SHADER_NAMED_INTERFACE_END(shadow_clip)

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_surf_curve_iface, curve_interp)
SMOOTH(float3, tangent)
SMOOTH(float3, binormal)
SMOOTH(float, time)
SMOOTH(float, time_width)
SMOOTH(float, radius)
SMOOTH(float, point_id) /* Smooth to be used for barycentric. */
GPU_SHADER_NAMED_INTERFACE_END(curve_interp)

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_surf_curve_flat_iface, curve_interp_flat)
FLAT(int, strand_id)
GPU_SHADER_NAMED_INTERFACE_END(curve_interp_flat)

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_surf_pointcloud_iface, pointcloud_interp)
SMOOTH(float, radius)
SMOOTH(float3, position)
GPU_SHADER_NAMED_INTERFACE_END(pointcloud_interp)

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_surf_pointcloud_flat_iface, pointcloud_interp_flat)
FLAT(int, id)
GPU_SHADER_NAMED_INTERFACE_END(pointcloud_interp_flat)

/* WORKAROUND: Until we get condition support for interfaces. */
GPU_SHADER_CREATE_INFO(eevee_shadow_iface_info)
VERTEX_OUT(eevee_surf_shadow_iface)
VERTEX_OUT(eevee_surf_shadow_clipping_iface)
GPU_SHADER_CREATE_END()

/* WORKAROUND: Until we get condition support for interfaces. */
GPU_SHADER_CREATE_INFO(eevee_geom_curves_iface_info)
VERTEX_OUT(eevee_surf_curve_iface)
VERTEX_OUT(eevee_surf_curve_flat_iface)
GPU_SHADER_CREATE_END()

/* WORKAROUND: Until we get condition support for interfaces. */
GPU_SHADER_CREATE_INFO(eevee_geom_disp_iface_info)
VERTEX_OUT(eevee_surf_disp_iface)
GPU_SHADER_CREATE_END()

/* WORKAROUND: Until we get condition support for interfaces. */
GPU_SHADER_CREATE_INFO(eevee_geom_pointcloud_iface_info)
VERTEX_OUT(eevee_surf_pointcloud_iface)
VERTEX_OUT(eevee_surf_pointcloud_flat_iface)
GPU_SHADER_CREATE_END()

/* WORKAROUND: Until we remove global accesses to the interface. */
GPU_SHADER_CREATE_INFO(eevee_geom_iface_info)
VERTEX_OUT(eevee_surf_iface)
VERTEX_OUT(eevee_surf_flat_iface)
GPU_SHADER_CREATE_END()

GPU_SHADER_NAMED_INTERFACE_INFO(eevee_clip_plane_iface, clip_interp)
SMOOTH(float, clip_distance)
GPU_SHADER_NAMED_INTERFACE_END(clip_interp)

/* Mesh batch of tessellated meshes, bound as storage buffers (see #GPU_batch_bind_as_resources).
 * Material attributes are added by the material code-generation. */
GPU_SHADER_CREATE_INFO(eevee_geom_mesh_tess_data)
TYPEDEF_SOURCE("eevee_defines.hh")
PUSH_CONSTANT(int, tessellation_expand)
PUSH_CONSTANT(bool, gpu_index_no_buffer)
PUSH_CONSTANT(bool, gpu_index_16bit)
PUSH_CONSTANT(int, gpu_index_base_index)
STORAGE_BUF_FREQ(MAT_TESS_INDEX_BUF_SLOT, read, uint, gpu_index_buf[], GEOMETRY)
STORAGE_BUF_FREQ(MAT_TESS_POS_BUF_SLOT, read, uint, vbo_pos[], GEOMETRY)
PUSH_CONSTANT(int2, gpu_attr_17)
PUSH_CONSTANT(int, gpu_attr_17_meta)
STORAGE_BUF_FREQ(MAT_TESS_NOR_BUF_SLOT, read, uint, vbo_nor[], GEOMETRY)
PUSH_CONSTANT(int2, gpu_attr_18)
PUSH_CONSTANT(int, gpu_attr_18_meta)
DEFINE("GPU_INDEX_LOAD")
DEFINE("MAT_GEOM_MESH_TESS")
GPU_SHADER_CREATE_END()

/* WORKAROUND: Until we remove global accesses to the interface. */
GPU_SHADER_CREATE_INFO(eevee_clip_plane)
VERTEX_OUT(eevee_clip_plane_iface)
TYPEDEF_SOURCE("eevee_uniform_shared.hh")
UNIFORM_BUF(CLIP_PLANE_BUF, ClipPlaneData, clip_plane)
GPU_SHADER_CREATE_END()
