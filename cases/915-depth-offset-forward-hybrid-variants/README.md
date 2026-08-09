# Depth Offset Forward / Hybrid Variants

## Test Content

This release case covers Depth Offset material variants that are not exercised
by the regular deferred Depth Offset repro.

The script creates two fresh Eevee scenes:

- A Shader To RGB material with a linked Material Output `Depth Offset`. This
  forces the deferred shader-to-rgba hybrid surface path.
- A `BLENDED` surface material with a linked Material Output `Depth Offset`.
  This forces the forward surface path.

Both scenes render a green front plane over a red back plane with a black world
background. The front material uses a linked value node for `Depth Offset`.

## Pass Criteria

- The hybrid material contains a `ShaderNodeShaderToRGB` node and a linked
  `Depth Offset` socket.
- The forward material uses `surface_render_method = BLENDED` and a linked
  `Depth Offset` socket.
- Eevee renders both variants without shader creation or compilation errors.
- The center region of both rendered images is visibly green and non-black.

## Test Entry

`run.py`
