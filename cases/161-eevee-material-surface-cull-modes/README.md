# eevee-material-surface-cull-modes

## Test Coverage

This case protects Eevee material surface culling in the Blender 5.2 NPR main build.

It procedurally renders a camera-facing plane and a reversed plane with each material `surface_cull_method`:

- `NONE` must render both front-facing and back-facing surfaces.
- `BACK` must render front-facing surfaces and hide back-facing surfaces.
- `FRONT` must hide front-facing surfaces and render back-facing surfaces.

The test covers the dithered/deferred material path and a blended material path that uses a Transparent/Emission shader mix.

## Pass Criteria

- A no-object background render establishes the hidden-surface brightness baseline.
- Each expected visible surface must render substantially brighter than the background.
- Each expected hidden surface must stay at background brightness.
- The assertion must pass for `NONE`, `BACK`, and `FRONT` culling in both tested material paths.

## Entry Point

`run.py`
