/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_geometry_lines_infos.hh"
VERTEX_SHADER_CREATE_INFO(eevee_geometry_lines_draw)

void main()
{
  line_instance = uint(instance_start) + uint(gl_VertexID) / 6u;
  GeometryLineInstanceData instance = instances[line_instance];
  GeometryLineBrushData brush = brushes[instance.meta.x];
  float radius = max(instance.a.w, instance.b.w);
  if ((instance.meta.y & 3u) != 0u && brush.curve_options.z != 0 && antialiasing > 0.0f) {
    radius = max(radius, min(antialiasing, 1.0f));
  }
  float2 padding = float2(radius + antialiasing + 1.0f);
  if ((instance.meta.y & 3u) != 0u) {
    float c = abs(cos(instance.stroke.w)), s = abs(sin(instance.stroke.w));
    padding = float2(c * brush.tip.w + s, s * brush.tip.w + c) * radius + antialiasing + 1.0f;
  }
  float2 lower = min(instance.a.xy, instance.b.xy) - padding;
  float2 upper = max(instance.a.xy, instance.b.xy) + padding;
  uint vertex = uint(gl_VertexID) % 6u;
  float2 corner = float2(vertex == 1u || vertex == 2u || vertex == 4u ? 1.0f : 0.0f,
                         vertex == 2u || vertex == 4u || vertex == 5u ? 1.0f : 0.0f);
  float2 p = mix(lower, upper, corner);
  gl_Position = float4(p / float2(line_extent) * 2.0f - 1.0f, 0.0f, 1.0f);
}
