# outer-depth-offset-material-output-crash

This case opens the single `.blend` file stored in `assets/`, a copy of the
local repro scene for the Depth Offset material-output crash.

## Test Content

- Verifies the file still contains a material output whose `Surface` and
  `Depth Offset` inputs are both linked.
- Sets the linked `Depth Offset` value to a positive value and renders through
  Eevee.
- Sets the same linked `Depth Offset` value to a negative value and renders
  through Eevee.
- Loads both saved PNGs and samples the center region.

## Pass Criteria

The scene must render successfully without an access violation in shader
material compilation. With a positive `Depth Offset`, the center region must
show the green offset material and keep opaque/non-black output. With a negative
`Depth Offset`, the center region must show the red plane. This matches the 5.1
behavior and catches regressions where positive offsets only update the prepass
depth but the material pass is rejected, leaving a transparent or black cutout.
