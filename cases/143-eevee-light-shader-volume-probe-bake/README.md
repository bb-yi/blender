# Eevee Light Shader Volume Probe Bake

## Test Content

Builds a small Eevee Volume Probe bake scene in the background. The scene bakes
direct lighting from a point light onto a diffuse wall, then switches the wall
material to display `Light Probe Color` irradiance through emission. This makes
the rendered pixels depend on the baked Volume Probe cache.

The case validates default pass-through, custom red light shader color,
`Attenuation = 1` distance behavior, and a `Light Space` driven color pattern.

## Pass Criteria

- Default Light Shader nodes bake the same irradiance as removing the custom
  Light Shader nodes.
- A custom red `Light Shader Output` bakes red-dominant irradiance into the
  Volume Probe cache.
- `Attenuation = 1` reduces distance falloff across the sampled wall compared
  with connecting `Default Attenuation`.
- A `Light Space` pattern produces measurable left/right color variation in the
  baked irradiance.
- Render outputs are written under this case's `out` directory.

## Test Entry

`run.py`
