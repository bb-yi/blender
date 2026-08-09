# glsl-function-vec4-input-w

## Test Content

This case builds a temporary Eevee scene with a plane material driven by a
`GLSL Function` node. The shader function accepts a `vec4` input and returns
`vec4(vec3(color.w), 1.0)`.

The test sets the generated `color` input socket to `(0, 0, 0, 0)` and renders
the plane. This covers the release-path regression where unlinked vec4 inputs
could lose their fourth component and behave as if `w` was `1.0`.

## Pass Criteria

The center pixel of the render must be black: red, green, and blue are each
below `0.05`. A white center pixel means the vec4 input `w` component was
forced to `1.0` and the regression has returned.

## Entry Point

`run.py`
