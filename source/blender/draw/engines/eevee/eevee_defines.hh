/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * List of defines that are shared with the GPUShaderCreateInfos. We do this to avoid
 * dragging larger headers into the createInfo pipeline which would cause problems.
 */

#pragma once

/* Look Up Tables. */
#define LUT_WORKGROUP_SIZE 16

/* Hierarchical Z down-sampling. */
[[maybe_unused]] static constexpr int HIZ_MIP_COUNT = 7;
/* NOTE: The shader is written to update 5 mipmaps using LDS.
 * Each thread actually update 4 LOD_0 pixels, which means the number of MIPLVL we cover is:
 * `(log2(HIZ_GROUP_SIZE) + 1) + LOD_0 + LAST_GROUP_LVL = HIZ_MIP_COUNT` */
[[maybe_unused]] static constexpr int HIZ_GROUP_SIZE = 16;

/* Avoid too much overhead caused by resizing the light buffers too many time. */
#define LIGHT_CHUNK 256

[[maybe_unused]] static constexpr int CULLING_SELECT_GROUP_SIZE = 256;
[[maybe_unused]] static constexpr int CULLING_SORT_GROUP_SIZE = 256;
[[maybe_unused]] static constexpr int CULLING_ZBIN_GROUP_SIZE = 1024;
[[maybe_unused]] static constexpr int CULLING_TILE_GROUP_SIZE = 256;

/* Reflection Probes. */
/* When changed update parallel sum loop in `eevee_lightprobe_sphere_remap_comp.glsl`. */
[[maybe_unused]] static constexpr int SPHERE_PROBE_REMAP_GROUP_SIZE = 32;
[[maybe_unused]] static constexpr int SPHERE_PROBE_GROUP_SIZE = 16;
[[maybe_unused]] static constexpr int SPHERE_PROBE_SELECT_GROUP_SIZE = 64;
[[maybe_unused]] static constexpr int SPHERE_PROBE_MIPMAP_LEVELS = 5;
[[maybe_unused]] static constexpr int SPHERE_PROBE_SH_GROUP_SIZE = 256;
[[maybe_unused]] static constexpr int SPHERE_PROBE_SH_SAMPLES_PER_GROUP = 64;
/* Must be power of two for correct partitioning. */
[[maybe_unused]] static constexpr int SPHERE_PROBE_ATLAS_MAX_SUBDIV = 12;
[[maybe_unused]] static constexpr int SPHERE_PROBE_ATLAS_RES = (1
                                                                << SPHERE_PROBE_ATLAS_MAX_SUBDIV);
/* Maximum number of thread-groups dispatched for remapping a probe to octahedral mapping. */
[[maybe_unused]] static constexpr int SPHERE_PROBE_MAX_HARMONIC =
    (SPHERE_PROBE_ATLAS_RES / SPHERE_PROBE_REMAP_GROUP_SIZE) *
    (SPHERE_PROBE_ATLAS_RES / SPHERE_PROBE_REMAP_GROUP_SIZE);
/* Start and end value for mixing sphere probe and volume probes. */
[[maybe_unused]] static constexpr float SPHERE_PROBE_MIX_START_ROUGHNESS = 0.7f;
[[maybe_unused]] static constexpr float SPHERE_PROBE_MIX_END_ROUGHNESS = 0.9f;
/* Roughness of the last mip map for sphere probes. */
[[maybe_unused]] static constexpr float SPHERE_PROBE_MIP_MAX_ROUGHNESS = 0.7f;
#define SPHERE_PROBE_FORMAT SFLOAT_16_16_16_16
/**
 * Limited by the UBO size limit `(16384 bytes / sizeof(SphereProbeData))`.
 */
[[maybe_unused]] static constexpr int SPHERE_PROBE_MAX = 128;

/** NOTE: Runtime format only. */
#define VOLUME_PROBE_FORMAT SFLOAT_16_16_16_16

/**
 * Limited by the performance impact it can cause.
 * Limited by the max layer count supported by a hardware (256).
 * Limited by the UBO size limit `(16384 bytes / sizeof(PlanarProbeData))`.
 */
[[maybe_unused]] static constexpr int PLANAR_PROBE_MAX = 16;

/**
 * IMPORTANT: Some data packing are tweaked for these values.
 * Be sure to update them accordingly.
 * SHADOW_TILEMAP_RES max is 32 because of the shared bitmaps used for LOD tagging.
 * It is also limited by the maximum thread group size (1024).
 */
#if 0
/* Useful for debugging the tile-copy version of the shadow rendering without making debugging
 * tools unresponsive. */
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_RES = 4;
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_LOD = 2; /* LOG2(SHADOW_TILEMAP_RES) */
#else
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_RES = 32;
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_LOD = 5; /* LOG2(SHADOW_TILEMAP_RES) */
#endif
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_LOD0_LEN = ((SHADOW_TILEMAP_RES / 1) *
                                                                 (SHADOW_TILEMAP_RES / 1));
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_LOD1_LEN = ((SHADOW_TILEMAP_RES / 2) *
                                                                 (SHADOW_TILEMAP_RES / 2));
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_LOD2_LEN = ((SHADOW_TILEMAP_RES / 4) *
                                                                 (SHADOW_TILEMAP_RES / 4));
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_LOD3_LEN = ((SHADOW_TILEMAP_RES / 8) *
                                                                 (SHADOW_TILEMAP_RES / 8));
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_LOD4_LEN = ((SHADOW_TILEMAP_RES / 16) *
                                                                 (SHADOW_TILEMAP_RES / 16));
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_LOD5_LEN = ((SHADOW_TILEMAP_RES / 32) *
                                                                 (SHADOW_TILEMAP_RES / 32));
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_PER_ROW = 64;
[[maybe_unused]] static constexpr int SHADOW_TILEDATA_PER_TILEMAP =
    (SHADOW_TILEMAP_LOD0_LEN + SHADOW_TILEMAP_LOD1_LEN + SHADOW_TILEMAP_LOD2_LEN +
     SHADOW_TILEMAP_LOD3_LEN + SHADOW_TILEMAP_LOD4_LEN + SHADOW_TILEMAP_LOD5_LEN);
/* Maximum number of relative LOD distance we can store. */
[[maybe_unused]] static constexpr int SHADOW_TILEMAP_MAX_CLIPMAP_LOD = 8;
#if 0
/* Useful for debugging the tile-copy version of the shadow rendering without making debugging
 * tools unresponsive. */
[[maybe_unused]] static constexpr int SHADOW_PAGE_CLEAR_GROUP_SIZE= 8;
[[maybe_unused]] static constexpr int SHADOW_PAGE_RES= 8;
[[maybe_unused]] static constexpr int SHADOW_PAGE_LOD= 3; /* LOG2(SHADOW_PAGE_RES) */
#else
[[maybe_unused]] static constexpr int SHADOW_PAGE_CLEAR_GROUP_SIZE = 32;
[[maybe_unused]] static constexpr int SHADOW_PAGE_RES = 256;
[[maybe_unused]] static constexpr int SHADOW_PAGE_LOD = 8; /* LOG2(SHADOW_PAGE_RES) */
#endif
/* For testing only. */
// #define SHADOW_FORCE_LOD0
[[maybe_unused]] static constexpr int SHADOW_MAP_MAX_RES = (SHADOW_PAGE_RES * SHADOW_TILEMAP_RES);
[[maybe_unused]] static constexpr int SHADOW_DEPTH_SCAN_GROUP_SIZE = 8;
[[maybe_unused]] static constexpr int SHADOW_AABB_TAG_GROUP_SIZE = 64;
[[maybe_unused]] static constexpr int SHADOW_MAX_TILEMAP = 4096;
[[maybe_unused]] static constexpr int SHADOW_MAX_TILE = (SHADOW_MAX_TILEMAP *
                                                         SHADOW_TILEDATA_PER_TILEMAP);
[[maybe_unused]] static constexpr int SHADOW_MAX_PAGE = 32768;
[[maybe_unused]] static constexpr int SHADOW_BOUNDS_GROUP_SIZE = 64;
[[maybe_unused]] static constexpr int SHADOW_CLIPMAP_GROUP_SIZE = 64;
[[maybe_unused]] static constexpr int SHADOW_VIEW_MAX = 64 /* Must match DRW_VIEW_MAX. */;
[[maybe_unused]] static constexpr int SHADOW_RENDER_MAP_SIZE = (SHADOW_VIEW_MAX *
                                                                SHADOW_TILEMAP_LOD0_LEN);
[[maybe_unused]] static constexpr int SHADOW_ATOMIC = 1;
[[maybe_unused]] static constexpr int SHADOW_PAGE_PER_ROW = 8;
[[maybe_unused]] static constexpr int SHADOW_PAGE_PER_COL = 8;
[[maybe_unused]] static constexpr int SHADOW_PAGE_MAX_LAYER = 512;
[[maybe_unused]] static constexpr int SHADOW_PAGE_PER_LAYER = (SHADOW_PAGE_PER_ROW *
                                                               SHADOW_PAGE_PER_COL);
[[maybe_unused]] static constexpr int SHADOW_MAX_STEP = 16;
[[maybe_unused]] static constexpr int SHADOW_MAX_RAY = 4;
[[maybe_unused]] static constexpr int SHADOW_ROG_ID = 0;

/* Gbuffer. */
/** IMPORTANT: Make sure all Gbuffer frame-buffer setup matches this. */
#define GBUF_HEADER_FB_LAYER_COUNT 1
/* Optional Principled NPR data uses the existing UINT32 header image binding. Bin order is kept
 * even when the regular GBuffer compacts its closures into layer order. */
#define GBUF_NPR_COLOR_LAYER 2
#define GBUF_NPR_MULTIPLIER_LAYER 5
#define GBUF_NPR_ADDITIVE_LAYER 8
#define GBUF_NPR_RIM_LAYER 11
#define GBUF_NPR_INDIRECT_WEIGHT_LAYER 12
#define GBUF_NPR_LIGHT_POLICY_LAYER 13
#define GBUF_NPR_SHADOW_STRENGTH_LAYER 16
#define GBUF_NPR_DIRECT_GAIN_LAYER 17
#define GBUF_NPR_HEADER_LAYER_COUNT 18
/* Allocated only when a surface diffusion node is present. Indexed by closure bin. */
#define GBUF_DIFFUSION_RADIUS_LAYER 18
#define GBUF_DIFFUSION_STRENGTH_LAYER 21
#define GBUF_DIFFUSION_DIRECT_LAYER 24
#define GBUF_DIFFUSION_INDIRECT_LAYER 27
#define GBUF_DIFFUSION_HEADER_LAYER_COUNT 30
#define GBUF_CLOSURE_FB_LAYER_COUNT 2
#define GBUF_NORMAL_FB_LAYER_COUNT 1

/* Deferred Lighting. */
#define DEFERRED_RADIANCE_FORMAT UINT_32
#define DEFERRED_GBUFFER_ROG_ID 0

/* Ray-tracing. */
[[maybe_unused]] static constexpr int RAYTRACE_GROUP_SIZE = 8;
/* Keep this as a define to avoid shader variations. */
#define RAYTRACE_RADIANCE_FORMAT UFLOAT_11_11_10
#define RAYTRACE_RAYTIME_FORMAT SFLOAT_32
#define RAYTRACE_VARIANCE_FORMAT SFLOAT_16
#define RAYTRACE_TILEMASK_FORMAT UINT_8

/* Sub-Surface Scattering. */
[[maybe_unused]] static constexpr int SUBSURFACE_GROUP_SIZE = RAYTRACE_GROUP_SIZE;
#define SUBSURFACE_RADIANCE_FORMAT UFLOAT_11_11_10
#define SUBSURFACE_OBJECT_ID_FORMAT UINT_16

/* Film. */
#define FILM_GROUP_SIZE 16

/* Motion Blur. */
#define MOTION_BLUR_GROUP_SIZE 32
#define MOTION_BLUR_DILATE_GROUP_SIZE 512

/* Irradiance Cache. */
/** Maximum number of entities inside the cache. */
[[maybe_unused]] static constexpr int IRRADIANCE_GRID_MAX = 64;

/* Depth Of Field. */
[[maybe_unused]] static constexpr int DOF_TILES_SIZE = 8;
[[maybe_unused]] static constexpr int DOF_TILES_FLATTEN_GROUP_SIZE = DOF_TILES_SIZE;
[[maybe_unused]] static constexpr int DOF_TILES_DILATE_GROUP_SIZE = 8;
[[maybe_unused]] static constexpr int DOF_BOKEH_LUT_SIZE = 32;
[[maybe_unused]] static constexpr int DOF_MAX_SLIGHT_FOCUS_RADIUS = 5;
[[maybe_unused]] static constexpr int DOF_SLIGHT_FOCUS_SAMPLE_MAX = 16;
[[maybe_unused]] static constexpr int DOF_MIP_COUNT = 4;
[[maybe_unused]] static constexpr int DOF_REDUCE_GROUP_SIZE = (1 << (DOF_MIP_COUNT - 1));
[[maybe_unused]] static constexpr int DOF_DEFAULT_GROUP_SIZE = 32;
[[maybe_unused]] static constexpr int DOF_STABILIZE_GROUP_SIZE = 16;
[[maybe_unused]] static constexpr int DOF_FILTER_GROUP_SIZE = 8;
[[maybe_unused]] static constexpr int DOF_GATHER_GROUP_SIZE = DOF_TILES_SIZE;
[[maybe_unused]] static constexpr int DOF_RESOLVE_GROUP_SIZE = (DOF_TILES_SIZE * 2);

/* Ambient Occlusion. */
#define AMBIENT_OCCLUSION_PASS_TILE_SIZE 16

/* IrradianceBake. */
#define SURFEL_GROUP_SIZE 256
#define SURFEL_LIST_GROUP_SIZE 256
#define IRRADIANCE_GRID_GROUP_SIZE 4 /* In each dimension, so 4x4x4 workgroup size. */
#define IRRADIANCE_GRID_BRICK_SIZE 4 /* In each dimension, so 4x4x4 brick size. */
#define IRRADIANCE_BOUNDS_GROUP_SIZE 64

/* Volumes. */
#define VOLUME_GROUP_SIZE 4
#define VOLUME_INTEGRATION_GROUP_SIZE 8
[[maybe_unused]] static constexpr int VOLUME_HIT_DEPTH_MAX = 16;

/* Velocity. */
#define VERTEX_COPY_GROUP_SIZE 64

/* Utility Texture. */
#define UTIL_TEX_SIZE 64
#define UTIL_BSDF_LAYER_COUNT 16
/* Scale and bias to avoid interpolation of the border pixel.
 * Remap UVs to the border pixels centers. */
#define UTIL_TEX_UV_SCALE ((UTIL_TEX_SIZE - 1.0f) / UTIL_TEX_SIZE)
#define UTIL_TEX_UV_BIAS (0.5f / UTIL_TEX_SIZE)

#define UTIL_BLUE_NOISE_LAYER 0
#define UTIL_SSS_TRANSMITTANCE_PROFILE_LAYER 1
#define UTIL_LTC_MAT_LAYER 2
#define UTIL_BRDF_LAYER 3
#define UTIL_BSDF_LAYER 4
#define UTIL_DISK_INTEGRAL_LAYER UTIL_SSS_TRANSMITTANCE_PROFILE_LAYER
#define UTIL_DISK_INTEGRAL_COMP 3

#define PREPASS_FRAG_OUT_NORMAL 0
#define PREPASS_FRAG_OUT_OB_ID 1
#define PREPASS_FRAG_OUT_VELOCITY 2

/* Resource bindings. */

/* Textures. */
/** WARNING: Don't forget to update the reserved slots info. */
/* Used anywhere. (Starts at index 2, since 0 and 1 are used by draw_gpencil) */
#define RBUFS_UTILITY_TEX_SLOT 2
#define HIZ_TEX_SLOT 3
/* Only during surface shading (forward and deferred eval). */
#define SHADOW_TILEMAPS_TEX_SLOT 4
#define SHADOW_ATLAS_TEX_SLOT 5
#define VOLUME_PROBE_TEX_SLOT 6
#define SPHERE_PROBE_TEX_SLOT 7
#define VOLUME_SCATTERING_TEX_SLOT 8
#define VOLUME_TRANSMITTANCE_TEX_SLOT 9
#define HIZ_PREVIOUS_LAYER_TEX_SLOT 10
#define RADIANCE_PREVIOUS_LAYER_TEX_SLOT 11
#define RAYCAST_DEPTH_TEX_SLOT 12
#define OBJECT_ID_TEX_SLOT 13
#define PREPASS_NORMAL_TEX_SLOT 14
/* Currently only used by ray-tracing, but might become used by forward too. */
#define PLANAR_PROBE_DEPTH_TEX_SLOT 15
#define PLANAR_PROBE_RADIANCE_TEX_SLOT 16

#define GBUF_CLOSURE_TEX_SLOT 17
#define GBUF_NORMAL_TEX_SLOT 18
#define GBUF_HEADER_TEX_SLOT 19
#define NPR_RADIANCE_TEX_SLOT 20
#define LIGHT_SHADER_TEX_SLOT 20
#define DIRECT_RADIANCE_NPR_TX_SLOT_1 21
#define INDIRECT_RADIANCE_NPR_TX_SLOT_1 24
#define BACK_HIZ_TX_SLOT 27
#define BACK_RADIANCE_TX_SLOT 28
/* Eevee render texture sampling. */
#define RENDER_TEXTURE_COLOR_TX_SLOT_0 29
#define RENDER_TEXTURE_COLOR_TX_SLOT_1 30
#define RENDER_TEXTURE_COLOR_TX_SLOT_2 31
#define RENDER_TEXTURE_COLOR_TX_SLOT_3 32
#define RENDER_TEXTURE_HISTORY_TX_SLOT_0 33
#define RENDER_TEXTURE_HISTORY_TX_SLOT_1 34
#define RENDER_TEXTURE_HISTORY_TX_SLOT_2 35
#define RENDER_TEXTURE_HISTORY_TX_SLOT_3 36
/* Only during filter material evaluation. */
#define FILTER_SCENE_COLOR_TEX_SLOT 37
#define FILTER_AOV_COLOR_TEX_SLOT 38
#define FILTER_AOV_VALUE_TEX_SLOT 39
#define FILTER_DEPTH_TEX_SLOT 40
#define FILTER_CRYPTOMATTE_TEX_SLOT 41
#define SCENE_SHADOW_TEX_SLOT 42
#define FILTER_GRAPH_INPUT_TEX_SLOT 43
/* NPR material passes already use their radiance input slot. Reuse a filter-only slot
 * already reserved away from material texture allocation. */
#define LIGHT_SHADER_NPR_TEX_SLOT FILTER_SCENE_COLOR_TEX_SLOT
/* Outline post-process shaders are isolated from material evaluation. Reuse this local range
 * instead of consuming high texture units that exceed the minimum per-stage backend limit. */
#define OUTLINE_DEPTH_TEX_SLOT 20
#define OUTLINE_COLOR_TEX_SLOT 21
#define OUTLINE_INFO_TEX_SLOT 22
#define OUTLINE_SEED_TEX_SLOT 23
#define OUTLINE_JFA_TEX_SLOT 24
#define OUTLINE_VECTOR_TEX_SLOT 25
#define OUTLINE_OCCLUSION_DEPTH_TEX_SLOT 26
#define SHADOW_CASTER_ATLAS_TEX_SLOT 44
/* Separate aliases are needed when NPR's BSL policy and legacy Shader Info/Raycast are both
 * present. The textures are shared; only sampler declarations and binding slots are distinct. */
#define NPR_SHADOW_OBJECT_ID_TEX_SLOT 45
#define NPR_SHADOW_NORMAL_TEX_SLOT 46

/* Images. */
#define RBUFS_COLOR_SLOT 0
#define RBUFS_VALUE_SLOT 1
#define RBUFS_CRYPTOMATTE_SLOT 2
#define GBUF_CLOSURE_SLOT 3
#define GBUF_NORMAL_SLOT 4
#define GBUF_HEADER_SLOT 5
#define OUTLINE_COLOR_SLOT 6
#define OUTLINE_INFO_SLOT 7
#define FILTER_GRAPH_OUTPUT_IMG_SLOT 3
#define OUTLINE_JFA_IN_IMG_SLOT 0
#define OUTLINE_JFA_OUT_IMG_SLOT 1
#define OUTLINE_JFA_STEP_GROUP_SIZE 16
/* Volume properties pass do not write to `rbufs`. Reuse the same bind points. */
#define VOLUME_PROP_SCATTERING_IMG_SLOT 0
#define VOLUME_PROP_EXTINCTION_IMG_SLOT 1
#define VOLUME_PROP_EMISSION_IMG_SLOT 2
#define VOLUME_PROP_PHASE_IMG_SLOT 3
#define VOLUME_PROP_PHASE_WEIGHT_IMG_SLOT 4
#define VOLUME_OCCUPANCY_SLOT 5
/* Only during volume pre-pass. */
#define VOLUME_HIT_DEPTH_SLOT 0
#define VOLUME_HIT_COUNT_SLOT 1
/* Only during shadow rendering. */
#define SHADOW_ATLAS_IMG_SLOT 4
#define SHADOW_CASTER_ATLAS_IMG_SLOT 5

/* Uniform Buffers. */
/* Slot 0 is GPU_NODE_TREE_UBO_SLOT. */
#define UNIFORM_BUF_SLOT 1
/* Split from the main uniform buffer since they're updated multiple times per frame. */
#define PIPELINE_BUF_SLOT 2
#define RAYTRACE_BUF_SLOT 3
/* Only during surface shading (forward and deferred eval). */
#define IRRADIANCE_GRID_BUF_SLOT 4
#define SPHERE_PROBE_BUF_SLOT 5
#define PLANAR_PROBE_BUF_SLOT 6
#define WORLD_SUNLIGHT_BUF_SLOT 7
/* Filter passes do not use probes or velocity. Keep clear of Uniform (1-3) and Draw (8-11). */
#define FILTER_OBJECT_INFO_BUF_SLOT 6
#define FILTER_MASK_HASH_BUF_SLOT 4
#define FILTER_GRAPH_INPUT_BUF_SLOT 5
/* Only during pre-pass. */
#define VELOCITY_CAMERA_PREV_BUF 4
#define VELOCITY_CAMERA_CURR_BUF 5
#define VELOCITY_CAMERA_NEXT_BUF 6
/* Overlap with Draw module reserved slot for volume.
 * But this is used only for the prepass which doesn't bind volume data. */
#define CLIP_PLANE_BUF 7
#define RENDER_TEXTURE_BUF_SLOT 12
/* Only during subsurface scattering */
#define SUBSURFACE_BUF_SLOT 4

/* Storage Buffers. */
#define LIGHT_CULL_BUF_SLOT 0
#define LIGHT_BUF_SLOT 1
#define LIGHT_ZBIN_BUF_SLOT 2
#define LIGHT_TILE_BUF_SLOT 3
#define IRRADIANCE_BRICK_BUF_SLOT 4
/* Only during deferred direct-light evaluation. */
#define LIGHT_SHADER_INDEX_BUF_SLOT 5
/* Light material graphs also expose Draw's model/info SSBOs at 8-11. */
#define LIGHT_SHADER_SURFEL_INDEX_BUF_SLOT 15
#define LIGHT_SHADER_SURFEL_BUF_SLOT 14
#define LIGHT_SHADER_UNIFORM_BUF_SLOT 13
#define SAMPLING_BUF_SLOT 6
#define CRYPTOMATTE_BUF_SLOT 7
/* Only during surface capture. */
#define SURFEL_BUF_SLOT 4
/* Only during surface capture. */
#define CAPTURE_BUF_SLOT 5
/* Only during shadow rendering. */
#define SHADOW_RENDER_MAP_BUF_SLOT 3
#define SHADOW_PAGE_INFO_SLOT 4
#define SHADOW_RENDER_VIEW_BUF_SLOT 5

/* Only during pre-pass. */
#define VELOCITY_OBJ_PREV_BUF_SLOT 0
#define VELOCITY_OBJ_NEXT_BUF_SLOT 1
#define VELOCITY_GEO_PREV_BUF_SLOT 2
#define VELOCITY_GEO_NEXT_BUF_SLOT 3
#define VELOCITY_INDIRECTION_BUF_SLOT 4

/* Only for tessellated meshes (#MAT_GEOM_MESH_TESS). Mesh batch bound as storage buffers.
 * Above the slots used by the engines to avoid any conflict.
 * NOTE: The push constants names (`gpu_attr_N`) use these values and need to be updated too. */
#define MAT_TESS_INDEX_BUF_SLOT 16
#define MAT_TESS_POS_BUF_SLOT 17
#define MAT_TESS_NOR_BUF_SLOT 18
/* Material attributes. */
#define MAT_TESS_ATTR_BUF_SLOT_FIRST 19
#define MAT_TESS_ATTR_BUF_SLOT_LAST 31

#define CLOSURE_WEIGHT_CUTOFF 1e-5f
/* Treat closure as singular if the roughness is below this threshold. */
#define BSDF_ROUGHNESS_THRESHOLD 2e-2f

#define MATERIAL_TEXTURE_RESERVED_SLOT_FIRST RBUFS_UTILITY_TEX_SLOT
#define MATERIAL_TEXTURE_RESERVED_SLOT_LAST_WORLD_VOLUME RBUFS_UTILITY_TEX_SLOT
#define MATERIAL_TEXTURE_RESERVED_SLOT_LAST_NO_EVAL RENDER_TEXTURE_HISTORY_TX_SLOT_3
#define MATERIAL_TEXTURE_RESERVED_SLOT_LAST_HYBRID RENDER_TEXTURE_HISTORY_TX_SLOT_3
#define MATERIAL_TEXTURE_RESERVED_SLOT_LAST_FORWARD RENDER_TEXTURE_HISTORY_TX_SLOT_3
#define MATERIAL_TEXTURE_RESERVED_SLOT_LAST_BAKE FILTER_AOV_VALUE_TEX_SLOT
#define MATERIAL_TEXTURE_RESERVED_SLOT_LAST_WORLD RENDER_TEXTURE_HISTORY_TX_SLOT_3
#define MATERIAL_TEXTURE_RESERVED_SLOT_LAST_FILTER FILTER_GRAPH_INPUT_TEX_SLOT
#define MATERIAL_TEXTURE_RESERVED_SLOT_LAST_NPR SCENE_SHADOW_TEX_SLOT

#define RENDER_TEXTURE_SLOT_MAX 4
#define FILTER_OBJECT_INFO_MAX 512
#define FILTER_GRAPH_INPUT_MAX 32
#define FILTER_GRAPH_OUTPUT_MAX 32
#define FILTER_MASK_HASH_MAX 128
#define OUTLINE_MAX_WIDTH 20.0f

/* Cannot use math libraries in shared headers yet. */
#define EEVEE_PI 3.14159265358979323846f /* pi */
