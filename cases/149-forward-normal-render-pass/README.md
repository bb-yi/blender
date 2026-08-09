# Forward Normal Render Pass

## Test Content

This release case validates that Eevee forward materials, especially
`BLENDED` materials, write usable data to the Normal render pass.

The script creates a tilted plane with a semi-transparent BLENDED material,
enables the Eevee Normal pass, renders in background mode, and samples the
center of the plane and a background corner.

## Pass Criteria

- The center pixel over the BLENDED plane must contain non-black Normal pass
  data.
- The center normal color must be clearly different from the black background.
- The EXR render-pass output and `summary.json` are written under this case's `out`
  directory.

## Test Entry

`run.py`
