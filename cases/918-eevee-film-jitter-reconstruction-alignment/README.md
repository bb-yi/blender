# Eevee Film Jitter Reconstruction Alignment

## Test Content

This case builds a minimal final-render scene in Python: a white asymmetric
emission triangle on a transparent background, viewed through an orthographic
camera at 256 by 256 pixels. Lighting, compositing, ray tracing, outlines,
motion blur, and temporal reprojection are disabled so the result isolates
camera pixel jitter and Film reconstruction.

The scene is rendered once with one Eevee sample and once with 64 samples. The
test compares the alpha centroid of both images. A correct multi-sample render
softens the triangle edge without translating the triangle. If
`Film::update_sample_table()` or the per-sample uniform upload is missing, the
camera projection jitters while the reconstruction filter retains stale data,
leaving an approximately half-pixel image offset.

The case writes `triangle_1_sample.png`, `triangle_64_samples.png`,
`triangle_red_cyan_overlay.png`, and `summary.json` under its `out` directory.
The overlay stores one-sample alpha in red and 64-sample alpha in cyan, making a
directional offset visible as red and cyan fringes along all three long edges.

## Pass Conditions

- The absolute alpha-centroid displacement must be below 0.10 pixels on both
  axes.
- The 64-sample render must contain at least 500 fractional-alpha pixels,
  proving that multi-sample edge reconstruction ran.
- At least 500 alpha pixels must differ by more than 1/255 between the two
  renders, proving that the comparison is not two identical single-sample
  outputs.
- Total alpha mass must differ by less than 0.5 percent, preventing missing or
  clipped content from masquerading as an alignment result.
- The rendered triangle must remain away from the image boundary and contain a
  substantial foreground area.

## Test Entry

`run.py`
