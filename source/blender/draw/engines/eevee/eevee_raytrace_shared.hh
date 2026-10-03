/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared code between host and client code-bases.
 */

#pragma once

#include "GPU_shader_shared_utils.hh"

#ifndef GPU_SHADER
namespace blender::eevee {
#endif

struct [[host_shared]] ScreenThicknessParameters {
  float thickness_ndc_scale;
  float thickness_ndc_bias;
  float thickness_vs_scale;
  float thickness_vs_bias;

  /* Return the depth buffer Z thickness of a pixel at a given view space Z depth. */
  static ScreenThicknessParameters build(float4x4 winmat,
                                         float avg_pixel_radius_unit,
                                         float min_pixel_thickness,
                                         float min_constant_thickness)
  {
    ScreenThicknessParameters params;
    avg_pixel_radius_unit *= min_pixel_thickness;
    if (winmat[3][3] == 0.0f) {
      /* Perspective pixels increase footprint with the distance. */
      params.thickness_vs_scale = avg_pixel_radius_unit;
      params.thickness_vs_bias = -min_constant_thickness;
      params.thickness_ndc_scale = winmat[3][2];
      params.thickness_ndc_bias = 0.0f;
      /* Convert from NDC to screen range. */
      params.thickness_ndc_scale *= 0.5f;
    }
    else {
      /* Orthographic pixels have fixed footprint. */
      params.thickness_vs_scale = 0.0f;
      params.thickness_vs_bias = 1.0f; /* Avoid NaN. */
      params.thickness_ndc_scale = 0.0f;
      params.thickness_ndc_bias = -(avg_pixel_radius_unit + min_constant_thickness) * winmat[2][2];
      /* Convert from NDC to screen range. */
      params.thickness_ndc_bias *= 0.5f;
    }
    return params;
  }

  /* Return the depth buffer Z thickness of a pixel at a given view space Z depth. */
  float pixel_depth_thickness_at(float vs_z) const
  {
    float vs_thickness = vs_z * thickness_vs_scale + thickness_vs_bias;
    /* NDC offset from view space offset.
     * From http://terathon.com/gdc07_lengyel.pdf (slide 24) */
    float ndc_thickness = vs_thickness / (vs_z * (vs_z + vs_thickness));
    return ndc_thickness * thickness_ndc_scale + thickness_ndc_bias;
  }
};

enum [[host_shared]] eHardwareTracePhase : uint32_t {
  HWRT_TRACE_PHASE_FULL = 0u,
  HWRT_TRACE_PHASE_PRECOMBINE = 1u,
  HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR = 2u,
};

struct [[host_shared]] HardwareDirectLightData {
  /** Candidate lights are selected from the existing Eevee light-culling tiles. */
  uint selection_mode;
  /** Tile size inherited from Eevee light culling. */
  uint tile_size_px;
  /** Number of bitmap words describing the local-light subset for one tile. */
  uint tile_word_len;
  /** Number of local lights considered by the tile candidate stage before stochastic picking. */
  uint candidate_local_lights_len;
  /** Total local lights in the current culling input. */
  uint local_lights_len;
  /** Directional / sun lights that bypass the local tile candidate bitmap. */
  uint sun_lights_len;
  /** Fixed stochastic light samples evaluated per shading point before denoise. */
  uint light_samples_per_shading_point;
  /** Directional/sun lighting is handled outside the bounded local-light tile budget. */
  bool32_t trace_sun_lights_separately;
  /** Mesh emissives are excluded until they have an explicit sampled-emitter representation. */
  bool32_t sample_emissive_meshes;
  /** Baseline importance weight for analytic local lights. */
  float local_light_importance_scale;
  /** Extra importance weight reserved for area-light candidates. */
  float area_light_importance_scale;
  /** Reserved extra importance weight for textured/light-profile candidates. */
  float textured_light_importance_scale;
  /** Separate directional/sun importance scale for the dedicated sun path. */
  float sun_light_importance_scale;
  /** Leading directional entries reserved for extracted world suns. */
  uint world_sun_lights_len;
  int _pad1;
  int _pad2;
};

struct [[host_shared]] HardwareDirectLightWorkTile {
  /** Tile coordinate in light-culling space packed as `packUvec2x16(tile_coord)`. */
  uint packed_tile_coord;
  /** Offset into Eevee's existing `light_tile_buf` bitmap for this tile. */
  uint candidate_word_offset;
  /** Number of bitmap words that describe this tile's local-light candidate subset. */
  uint candidate_word_count;
  /** Bounded stochastic light samples reserved for this tile. */
  uint sample_budget;
};

struct [[host_shared]] HardwareDirectLightVisibilitySample {
  /** Source tile in light-culling space packed as `packUvec2x16(tile_coord)`. */
  uint packed_tile_coord;
  /** Full-resolution texel used to read RT visibility for this tile sample. */
  uint packed_sample_texel;
  /** Selected local-light index in Eevee light-buffer order, or `0xFFFFFFFFu` if unused. */
  uint local_light_index;
  /** Selected sun-light index relative to the sun range, or `0xFFFFFFFFu` if unused. */
  uint sun_light_index;
  /** RT visibility for the selected local-light sample. Nuru: RGB so refractive shadows
   * (e.g. glass) can carry per-channel attenuation through to the receiver. `packed_float3`
   * keeps 3+1 packing with the trailing scalar so the struct stays 16-byte aligned. */
  packed_float3 local_visibility;
  /** Importance used when selecting the local-light sample. */
  float local_importance;
  /** RT visibility for the selected sun-light sample. */
  packed_float3 sun_visibility;
  /** Importance used when selecting the sun-light sample. */
  float sun_importance;
  /** Nuru NIS training feedback: luminance of the weighted local-light contribution
   * (realized L/p estimate), written back by the accumulation kernel. */
  float train_luma;
  float _pad0;
  float _pad1;
  float _pad2;
};

struct [[host_shared]] RayTraceData {
  /** ViewProjection matrix used to render the previous frame. */
  float4x4 history_persmat;
  /** ViewProjection matrix used to denoise the previous frame. */
  float4x4 denoise_history_persmat;
  /** Input resolution. */
  int2 full_resolution;
  /** Inverse of input resolution to get screen UVs. */
  float2 full_resolution_inv;
  /** Scale and bias to go from ray-trace resolution to input resolution. */
  int2 trace_pixel_offset;
  int trace_pixel_scale;
  /** View space thickness the objects. */
  float thickness;
  /** Scale and bias to go from fast GI resolution to input resolution. */
  int2 fast_gi_resolution_bias;
  int fast_gi_resolution_scale;
  /** Bias to the full-screen buffer LOD to account for radiance buffer top down-scaling factor. */
  float fast_gi_lod_bias;
  /** Scale to apply to full-screen UVs to remove padding. */
  float2 fast_gi_uv_scale;
  /** Determine how fast the sample steps are getting bigger. */
  float quality;
  /** Maximum roughness for which we will trace a ray. */
  float roughness_mask_scale;
  float roughness_mask_bias;
  /** If set to true will bypass spatial denoising. */
  bool32_t skip_denoise;
  /** If set to false will bypass tracing for refractive closures. */
  bool32_t trace_refraction;
  /** Closure being ray-traced. */
  int closure_index;
  /** If true, consider backface hit as valid. Otherwise, use ray miss pipeline. */
  bool32_t use_backface_hit;
  /** Amount of frontface lighting to use for backface hits. */
  float backface_hit_scale;
  uint hardware_trace_phase;
  uint hardware_feature_mask;
  bool32_t use_hardware_tracing_method;
  bool32_t hardware_nis_enable;
  uint _pad1;
  uint _pad2;

  struct ScreenThicknessParameters fast_gi_thickness;
  struct ScreenThicknessParameters ray_thickness;
  struct HardwareDirectLightData hardware_direct_light;

  /* Nuru owns the supported indirect families in hardware mode. Turning a family off must
   * not restore unoccluded probe lighting. Unsupported 5.2 closures keep their native path. */
  bool skip_hardware_phase(bool is_diffuse, bool is_reflection, bool is_refraction) const
  {
    if (!use_hardware_tracing_method) {
      return false;
    }
    if ((is_diffuse && (hardware_feature_mask & (1u << 0u)) == 0u) ||
        (is_reflection && (hardware_feature_mask & (1u << 2u)) == 0u) ||
        (is_refraction && (hardware_feature_mask & (1u << 3u)) == 0u))
    {
      return true;
    }
    bool late_specular = is_reflection || is_refraction;
    if (hardware_trace_phase == HWRT_TRACE_PHASE_PRECOMBINE) {
      return late_specular;
    }
    if (hardware_trace_phase == HWRT_TRACE_PHASE_SCENE_FINAL_SPECULAR) {
      return !late_specular;
    }
    return false;
  }
};

enum [[host_shared]] eHardwareHitEvalFlag : uint32_t {
  HIT_EVAL_FLAG_FRONT_FACING = 1u << 0,
};

/** Sparse material replay record, matching Nuru's traced triangle payload. */
struct [[host_shared]] HardwareHitEvalRecord {
  uint packed_texel;
  uint resource_id_raw;
  uint primitive_id;
  uint flags;
  float2 barycentric_coords;
  float2 _pad0;
  packed_float3 view_origin;
  float _pad1;
};
BLI_STATIC_ASSERT_ALIGN(HardwareHitEvalRecord, 16)

struct [[host_shared]] AOData {
  float2 pixel_size;
  float distance;
  float lod_factor;

  float thickness_near;
  float thickness_far;
  float angle_bias;
  float gi_distance;

  float lod_factor_ao;
  float _pad0;
  float _pad1;
  float _pad2;
};

#ifndef GPU_SHADER
}  // namespace blender::eevee
#endif
