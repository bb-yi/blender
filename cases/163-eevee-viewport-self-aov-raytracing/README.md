# Eevee Viewport Self AOV Raytracing

## Test Content

This case opens a repro scene where the Suzanne material writes color AOV `AOV` in its surface
tree, then reads that same AOV from the material NPR Tree while Eevee ray tracing is enabled.

## Pass Criteria

The rendered camera viewport and a normal Eevee render must show the Suzanne self-AOV checker
pattern. The sampled Suzanne head region must be visibly non-black; a fully black silhouette is a
failure.
