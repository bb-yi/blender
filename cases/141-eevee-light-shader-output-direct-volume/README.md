# Eevee Light Shader Output Direct And Volume

## Test Content

Runs the stable Eevee Light Shader validation scripts for this branch as a
release packaging case. The case covers the Light datablock node UI surface,
default node-tree pass-through, custom direct light color/intensity/attenuation,
Range Scale, Malt-style distance attenuation semantics, `Scene Time`, and Light
Shader Info vector outputs.

The surface script also covers the forward support added on this branch: a
uniform `Scene Time` light shader is checked through a forward/blended material,
and a point-dependent `Light Space` checker light shader is checked through a
`Shader to RGB` material using the front-layer cache.

It also runs the volume validation path, which checks custom light shader
results in Eevee volume direct scattering.

## Pass Criteria

- `Light Shader Info` and `Light Shader Output` are registered and usable in
  Light datablock node trees.
- The default light shader node tree renders exactly like the non-custom Eevee
  direct light path.
- Custom direct lighting changes only the intended light and responds to
  color, intensity, attenuation, Range Scale, `Scene Time`, and info vectors.
- Forward/blended materials can read uniform light shader results.
- `Shader to RGB` can read the front-most point-dependent light shader cache.
- Volume direct scattering responds to custom color, attenuation, Light Space,
  and `Scene Time`.
- Generated render outputs are written under this case's `out` directory.

## Test Entry

`run.py`
