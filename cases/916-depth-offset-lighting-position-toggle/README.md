# Depth Offset Lighting Position Toggle

## Test Content

This release case checks the intended `Depth Offset Affects Lighting` behavior.
It creates synthetic Eevee scenes with a lit Suzanne material and renders:

- A no-offset baseline.
- A linked Material Output `Depth Offset` with lighting influence disabled.
- The same linked `Depth Offset` with lighting influence enabled.

The sequence is run for both regular deferred materials and `BLENDED` forward
materials.

## Pass Criteria

- With `Depth Offset Affects Lighting` disabled, the rendered lighting must stay
  close to the no-offset baseline for both deferred and forward paths.
- With `Depth Offset Affects Lighting` enabled, the rendered lighting must differ
  measurably from the same baseline for both paths.
- All renders must complete without shader compilation errors.

## Test Entry

`run.py`
