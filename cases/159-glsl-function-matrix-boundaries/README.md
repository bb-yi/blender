# glsl-function-matrix-boundaries

## Test Content

This case builds a temporary Eevee scene with a plane material driven by a
`GLSL Function` node. The function has `mat2`, `mat3`, and `mat4` inputs,
returns a `mat4`, and writes an `out mat4`.

The test sets the generated matrix column sockets directly. `mat4` columns are
split into `vec3` plus a separate `W` float, so the render also verifies that
the fourth component is preserved through the wrapper.

## Pass Criteria

The GLSL Function must parse as `READY`, expose the expected split column
sockets, compile in Eevee, and render the center pixel close to the expected
color. The emission strength comes from `out mat4` column 4 W, so a wrong W
socket or wrapper order changes the rendered pixel.

## Entry Point

`run.py`
