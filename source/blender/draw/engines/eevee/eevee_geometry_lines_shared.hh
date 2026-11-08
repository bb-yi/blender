/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#ifndef GPU_SHADER
#  include "BLI_memory_utils.hh"
#  include "GPU_shader_shared.hh"
namespace blender::eevee {
#endif

struct [[host_shared]] GeometryLineBrushData {
  float4 color;
  float4 tip;          /* width, opacity, spacing, stretch */
  float4 variation;    /* rotation, softness, width random, rotation random */
  float4 taper;        /* width start/end, alpha start/end */
  float4 dash;         /* length, space, phase, enabled */
  float4 deformation;  /* extend, normal offset, wave, cycles */
  float4 offset_noise; /* x, y, noise, grain */
  int4 options;        /* mode, copies, seed, image/procedural tip flags */
  int4 curve_options;  /* width curve, opacity curve, raster response, orientation */
  float4 maps; /* width map index/strength, color map index/strength */
  float4 detail_maps; /* brush map index/opacity, distortion map index/amount in pixels */
  float4 wave_settings; /* wavelength px, phase radians, amplitude variation, period variation */
  float4 wave_options; /* phase variation, pixel wavelength mode, padding */
};

struct [[host_shared]] GeometryLineMapData {
  float4 transform; /* tiling xy, offset xy */
  float4 rotation; /* cosine, sine, coordinate source, padding */
  int4 sampling; /* wrap U, wrap V, filter, padding */
};

struct [[host_shared]] GeometryLineCurveData {
  float2 values[256]; /* Separate storage: shaders fetch two samples without copying a LUT per stamp. */
};

struct [[host_shared]] GeometryLineReductionData {
  float4 origin; /* evaluated reference position, start distance */
  float4 domain; /* inverse distance span, end distance, active, padding */
  float values[256];
};

struct [[host_shared]] GeometryLinePartData {
  float4 a; /* screen xyz, per-stroke random rotation in degrees */
  float4 b;
  float4 previous_a;
  float4 previous_b;
  float4 next_a;
  float4 next_b;
  float4 arc;   /* start, part length, path length, width scale */
  uint4 range; /* instance offset, instance count, brush, copy */
  uint4 meta;  /* seed, closed/first/last flags + tip layer at bit 8, width/opacity reduction */
  uint4 sampling; /* first grid index, grid stride, path grid count, stable object seed */
  float4 width_uv_a; /* source UV, inverse clip w, valid */
  float4 width_uv_b;
  float4 color_uv_a;
  float4 color_uv_b;
  float4 distortion_uv_a;
  float4 distortion_uv_b;
  float4 brush_color_a; /* Attribute RGB + inverse clip w; zero w means missing. */
  float4 brush_color_b;
  float4 offset_normals; /* path normals at a/b, prepared before choosing pen direction */
};

struct [[host_shared]] GeometryLineInstanceData {
  float4 a;      /* xyz, radius */
  float4 b;      /* xyz, radius */
  float4 stroke; /* start arc, end arc, path length (0 = closed), angle */
  float4 motion_a; /* previous-current and current-next deltas at start */
  float4 motion_b; /* previous-current and current-next deltas at end */
  uint4 meta;    /* brush, mode in low 2 bits + tip layer, source part, packed half opacity */
};

#ifndef GPU_SHADER
BLI_STATIC_ASSERT_ALIGN(GeometryLineBrushData, 16)
BLI_STATIC_ASSERT_ALIGN(GeometryLineCurveData, 8)
BLI_STATIC_ASSERT_ALIGN(GeometryLineReductionData, 16)
BLI_STATIC_ASSERT_ALIGN(GeometryLineMapData, 16)
BLI_STATIC_ASSERT_ALIGN(GeometryLinePartData, 16)
BLI_STATIC_ASSERT_ALIGN(GeometryLineInstanceData, 16)
}  // namespace blender::eevee
#endif
