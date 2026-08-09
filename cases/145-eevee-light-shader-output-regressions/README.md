# Eevee Light Shader Output Regressions

## Test Content

Builds a minimal Eevee scene in a background Blender session with a diffuse
receiver and a point light using Light datablock nodes. The light's
`Light Shader Output` color is driven by `Scene Time` through a color ramp so
frame 1 renders blue-biased direct lighting and frame 11 renders red-biased
direct lighting.

The case then saves the generated `.blend`, reopens it, and renders frame 11
again. This covers the regressions around time-dependent custom light shader
results, cache invalidation between frames, and persistence of the custom Light
Shader Output node tree through file reload.

## Pass Criteria

- Frame 1 and frame 11 differ by a measurable pixel delta.
- Frame 11 has a stronger red channel than frame 1.
- Frame 1 has a stronger blue channel than frame 11.
- After saving and reopening the generated `.blend`, frame 11 still renders a
  red-dominant custom Light Shader Output result.
- The generated `.blend` and render outputs are written under this case's
  `out` directory.

## Test Entry

`run.py`
