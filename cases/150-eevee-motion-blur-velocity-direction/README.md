# Eevee Motion Blur Velocity Direction

## Test Content

This release case validates ordinary-object motion blur in Eevee.

The script builds a small synthetic scene with two objects sharing one material:
a static cube and a UV sphere animated horizontally across the frame. It renders
frame 13 twice, once with motion blur disabled and once with motion blur enabled,
then compares fixed image regions.

This catches regressions where the velocity prepass writes or binds the wrong
resources and the motion blur pass smears unrelated static/background pixels or
uses an incorrect velocity direction/length.

## Pass Criteria

- The moving-object region must differ clearly between the motion-blur on/off
  renders.
- The static-object region must stay effectively unchanged.
- The right-side background region must stay effectively unchanged.
- The static/background difference must remain far below the moving-object
  difference.
- Rendered PNGs and `summary.json` are written under this case's `out`
  directory.

## Test Entry

`run.py`
