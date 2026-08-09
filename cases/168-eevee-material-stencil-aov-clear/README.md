# eevee-material-stencil-aov-clear

This case protects the EEVEE material stencil prepass path for secondary deferred layers.

## Test Content

The script builds a small scene in Blender:

- A back plane writes a red color AOV.
- A smaller front plane uses DITHERED raytrace transmission so it renders as a later deferred layer.
- The front plane is rendered once without stencil and once with read-only material stencil enabled.
- A Filter Graph material displays the color AOV in the final image.

## Pass Criteria

The back-plane corner must remain red in both variants. The center covered by the front plane must be black in both variants. If the stencil path leaves EEVEE's internal prepass-untouched marker set, the stale red AOV from the back layer leaks through the stencil variant and this case fails.
