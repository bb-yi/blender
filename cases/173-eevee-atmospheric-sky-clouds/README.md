# eevee-atmospheric-sky-clouds

## Test Content

This case exercises the production World implementation in
`tools/atmospheric_sky_clouds` with the compact assets stored in this case.
It does not substitute a simplified shader.

The test verifies:

- the eight LUT, volume-noise, weather, blue-noise, star, and moon assets
  match the manifest SHA-256, byte size, and logical/physical dimensions;
- the World `GLSL Function` parses as `READY`, exposes its expected boundary,
  and keeps every `sampler2D`/`sampler3D` input connected through an
  `Image to Closure` node;
- the VolumeCloud runtime controls expose the expected vector, color, scalar,
  and integer socket types;
- 3D LUT/noise strips use the declared `physical_x = z * width + x` layout;
- Eevee compiles and renders the shader at 48x32 for noon atmosphere,
  midnight atmosphere, and Cloud Alpha debug output;
- changing the time produces a measurable day/night response; and
- the cloud ray march produces both opaque body and clear holes.

The renders remain in Blender's in-memory `Render Result`; this Release case
does not create image output files.

## Pass Criteria

All manifest, node-tree, and VolumeCloud runtime-interface assertions must
pass. Every rendered channel must be finite. Noon mean luminance must exceed midnight mean luminance by at least
10 percent, and their mean absolute RGB difference must exceed `0.01`.

Cloud Alpha must stay in `[0, 1]`, have a maximum above `0.12`, a minimum below
`0.08`, and a range above `0.10`. These checks reject missing/flat volume
textures and a raymarch that produces either an empty or solid sky.

On success the script prints
`ATMOSPHERIC_SKY_CLOUDS_RELEASE_OK parse=READY assets=8` with the measured
day/night and cloud metrics.

## Entry Point

`run.py`
