# eevee_taa_soft_shadow_history_reset

## Test Content

Creates a small Eevee viewport scene with:

- temporal reprojection enabled;
- jittered viewport soft shadows enabled;
- an area light casting a soft shadow from a sphere onto a ground plane.

The test warms the rendered viewport at one receiver position, moves the ground
plane, then captures the first viewport frame after the update. It also captures
a fresh reference viewport started directly at the moved receiver position.

## Pass Criteria

- The viewport captures must contain real lit scene content, not an empty or UI
  only image.
- The first post-update capture must stay close to the fresh moved-position
  reference across the central viewport crop.
- The maximum local difference is allowed to vary slightly because soft shadows
  are stochastic, but the mean difference must stay low enough to catch stale
  Combined history being stretched across the receiver.

This covers the regression where `sampling.reset()` restarts sample counting but
Film still reprojects the previous Combined buffer during the first interactive
sample after a scene-content change.

## Test Entry

`run.py`
