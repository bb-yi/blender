/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Query values match NodeShaderDrawInfoQuery in DNA_node_types.h. Keep the stage tests here,
 * rather than inferring context from a material merely containing a particular output node. */
[[node]]
void node_draw_info(float query, out float result)
{
  result = 0.0f;
  switch (int(query)) {
    case 0: /* Outline Shell. */
#ifdef MAT_OUTLINE_SHELL
      result = 1.0f;
#endif
      break;
    case 1: /* Shadow Pass. */
#ifdef MAT_SHADOW
      result = 1.0f;
#endif
      break;
    case 2: /* Render Texture Capture. */
      result = float((draw_info_context_flags() & DRAW_CONTEXT_RENDER_TEXTURE_CAPTURE) != 0u);
      break;
    case 3: /* Probe Capture. */
#if defined(MAT_CAPTURE) || defined(MAT_PROBE_CAPTURE)
      result = 1.0f;
#else
      result = float((draw_info_context_flags() & (DRAW_CONTEXT_SPHERE_PROBE_CAPTURE |
                                                   DRAW_CONTEXT_PLANAR_PROBE_CAPTURE)) != 0u);
#endif
      break;
    case 4: /* Image Render. */
      result = float((draw_info_context_flags() & DRAW_CONTEXT_IMAGE_RENDER) != 0u);
      break;
    case 5: /* Color Bake. */
#ifdef MAT_BAKE_COLOR
      result = 1.0f;
#endif
      break;
    case 6: /* Viewport. */
      result = float((draw_info_context_flags() & DRAW_CONTEXT_VIEWPORT) != 0u);
      break;
    case 7: /* Depth Prepass. */
#ifdef MAT_DEPTH
      result = 1.0f;
#endif
      break;
    case 8: /* Deferred. */
#if defined(MAT_DEFERRED) || \
    (defined(NPR_SHADER) && !defined(MAT_GEOM_WORLD) && !defined(MAT_BAKE_COLOR))
      result = 1.0f;
#endif
      break;
    case 9: /* Forward. */
#ifdef MAT_FORWARD
      result = 1.0f;
#endif
      break;
    case 10: /* NPR Evaluation. */
#ifdef NPR_SHADER
      result = 1.0f;
#endif
      break;
    case 11: /* Filter Pass. */
#ifdef MAT_FILTER
      result = 1.0f;
#endif
      break;
    case 12: /* World. */
#if defined(MAT_GEOM_WORLD) && !defined(MAT_FILTER)
      result = 1.0f;
#endif
      break;
    case 13: /* Vertex Evaluation. */
#ifdef GPU_VERTEX_SHADER
      result = 1.0f;
#endif
      break;
    case 14: /* Fragment Evaluation. */
#ifdef GPU_FRAGMENT_SHADER
      result = 1.0f;
#endif
      break;
    case 15: /* Volume Evaluation. */
#ifdef MAT_VOLUME
      result = 1.0f;
#endif
      break;
    case 16: /* Sphere Probe Capture. */
#ifdef MAT_SPHERE_PROBE_CAPTURE
      result = 1.0f;
#else
      result = float((draw_info_context_flags() & DRAW_CONTEXT_SPHERE_PROBE_CAPTURE) != 0u);
#endif
      break;
    case 17: /* Planar Probe Capture. */
#ifdef MAT_PLANAR_PROBE_CAPTURE
      result = 1.0f;
#else
      result = float((draw_info_context_flags() & DRAW_CONTEXT_PLANAR_PROBE_CAPTURE) != 0u);
#endif
      break;
    case 18: /* Irradiance Capture. */
#ifdef MAT_CAPTURE
      result = 1.0f;
#endif
      break;
    case 19: /* Pre-FX Render. */
      result = float((draw_info_context_flags() & DRAW_CONTEXT_PRE_FX_RENDER) != 0u);
      break;
    case 20: /* Playback. */
      result = float((draw_info_context_flags() & DRAW_CONTEXT_PLAYBACK) != 0u);
      break;
    case 21: /* Navigating. */
      result = float((draw_info_context_flags() & DRAW_CONTEXT_NAVIGATING) != 0u);
      break;
  }
}
