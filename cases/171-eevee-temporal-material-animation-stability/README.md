# Eevee Temporal Material Animation Stability

## Test Content

This case opens a foreground Blender child process because the regression depends on the real
Rendered Viewport playback path. It displays a static white emission strip against a black world,
enables temporal reprojection, and captures eight distinct playback frames in four states:

1. The material has no animation data.
2. The material has a constant FCurve on a property that does not affect the node shader.
3. The FCurve has been deleted while the layered Action remains assigned and empty.
4. A connected `Scene Time` path is multiplied by zero, so the rendered result remains constant
   while the renderer intentionally invalidates history every frame.

The second state reproduces dependency-graph updates from material animation without changing the
rendered image. The third state covers the persistent empty-Action condition left by deleting the
last material keyframe. The fourth state verifies the deterministic-jitter fallback used only when
time-dependent shading genuinely requires history invalidation.

## Pass Conditions

The child process must confirm that timeline playback is active, capture eight distinct timeline
frames per state, observe one material FCurve in the animated state, and observe zero FCurves while
the same Action remains assigned in the final state. The runner measures the subpixel position of a
high-contrast vertical edge in every capture. The no-animation control must remain below 0.30
pixels. The animated and empty-Action spans must also remain below 0.30 pixels and within 0.12
pixels of the control, proving temporal reprojection remains active. The visually constant Scene
Time stage must remain below 0.05 pixels while its history is intentionally discarded.
