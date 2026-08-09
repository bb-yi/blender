# EEVEE Outline routing and Forward parity

## Test Content

This case builds a minimal Eevee scene containing a blue sphere with a red
`ShaderNodeOutlineControl` output. It exercises both the final render path and
the Rendered Viewport path.

The final-render child processes run once with OpenGL and once with Vulkan. For
each backend they render both `DITHERED` and `BLENDED` materials, and toggle
`ViewLayerEEVEE.use_pass_outline` so the test can inspect the actual Combined
and Outline outputs written by the compositor.

The Vulkan child additionally captures Rendered Viewport output with the pass
enabled and disabled. This covers the Film pass-allocation path used by the
viewport, not only the final render path.

## Pass Criteria

- With `use_pass_outline=True`, the Outline output contains red outline pixels
  while Combined contains no red outline pixels.
- With `use_pass_outline=False`, the red outline returns to Combined.
- Both `DITHERED` and `BLENDED` materials produce a visible outline in the
  final render.
- The Vulkan Rendered Viewport shows the same enabled/disabled pass routing.
- OpenGL and Vulkan child logs contain no shader compilation error, missing GPU
  binding, validation failure, device-lost error, timeout, or native crash.
