# Eevee Light Shader Probe Reflection

## Test Content

Uses the checked-in copy of the user's probe test scene:

`assets/灯光节点反射探头测试.blend`

The scene contains a point light whose `Light Shader Output` color is driven by
a checker texture in light space, a lit plane, a Sphere Probe, and a metallic
sphere reflecting the captured lighting result.

## Pass Criteria

- The asset opens in a factory-startup background Blender session.
- The rendered sphere reflection contains visible non-black lighting.
- The sphere reflection contains a measurable checker pattern contrast.
- Rendering the same scene with the custom light shader nodes bypassed reduces
  the checker-scale local variation, so the result is not just material or
  camera gradients.
- Render outputs are written under this case's `out` directory.

## Test Entry

`run.py`
