# Material Stencil Portal

## Test Content

This release case validates the material-stencil portal workflow in Eevee.

The script builds a small orthographic scene with:

- A blue reader plane using `use_stencil=True`, `stencil_test=EQUAL`,
  `stencil_reference=1`, and `stencil_order=0`.
- A front writer plane using `use_color_write=False`,
  `use_depth_write=False`, `use_stencil=True`, `stencil_test=ALWAYS`,
  `stencil_pass_op=REPLACE`, `stencil_reference=1`, and
  `stencil_order=-1`.
- A closer depth-only occluder covering the right half of the image.

The writer runs before the reader because its stencil order is smaller. On the
left side it passes depth, writes stencil, and opens the blue reader. On the
right side the closer depth-only occluder prevents the writer from passing
depth, so the reader stays masked out and the black world remains visible.

## Pass Criteria

- Required material RNA properties for stencil and color/depth write exist.
- The left sample is blue, proving the writer opened the stencil portal before
  the reader rendered.
- The right sample is black, proving ordinary depth testing still prevents
  stencil writes behind a closer depth-only occluder.
- The PNG render and `summary.json` are written under this case's `out`
  directory.

## Test Entry

`run.py`
