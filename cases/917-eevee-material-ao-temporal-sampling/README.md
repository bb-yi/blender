# Eevee Material AO Temporal Sampling

## Test Content

The case procedurally recreates the `AO噪点测试.blend` setup: a smooth subdivided Suzanne above a
plane, with an Ambient Occlusion node using 16 node samples and distance 1.0. The AO color is raised
to power 20 and connected to the material surface to make correlated sampling noise visible.

The scene is rendered once with one Eevee render sample and once with 64 render samples. Both PNG
outputs are retained under `temp/release_test_outputs/eevee_material_ao_temporal_sampling`.

## Pass Conditions

- The one-sample central Suzanne crop must have Laplacian high-frequency energy above 0.15, proving
  that the scene exercises stochastic AO instead of producing a flat image.
- The 64-sample energy must be below 0.06.
- The 64-sample energy must be no more than 12 percent of the one-sample energy.

These assertions catch the regression where `sampling_rng_3D_get(SAMPLING_AO_U)` returned a
constant zero because the dynamic material shader exposes `CREATE_INFO_eevee_Sampling`, not the
legacy `EEVEE_SAMPLING_DATA` gate.

## Test Entry

`run.py`
