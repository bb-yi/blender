# Eevee Light Shader Fast Path

## Test Content

This case validates the fast-path contract for Eevee Light Shader Output.

It builds synthetic scenes in a background Blender session and writes render
outputs under this case's `out` directory. The uniform scenes intentionally keep
`Range Scale` at `1.0`, so they exercise the uniform buffer path instead of the
screen/froxel/front-layer point-dependent caches.

The case covers:

- Uniform `Scene Time` light shader results in deferred surface lighting.
- Uniform `Scene Time` light shader results in forward/blended surface lighting.
- Uniform `Scene Time` light shader results in volume direct scattering.
- `Light Space -> Checker` through `Shader to RGB`, which requires the
  front-most surface cache.
- Surface texture-layer overflow fallback for point-dependent custom light
  shaders, with a normal non-custom light left in the scene to prove rendering
  continues.

## Pass Criteria

- Deferred, forward, and volume uniform renders visibly change between frame 1
  and frame 11, proving the shared uniform result buffer is used.
- The `Shader to RGB` render contains both red and blue checker regions from the
  point-dependent front-layer cache.
- The overflow scene renders successfully after creating more unique
  point-dependent light shader layers than `gpu.capabilities.max_texture_layers_get()`.
- All outputs are written under this case's `out` directory.

## Test Entry

`run.py`
