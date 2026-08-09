# Material ZTest Mode

## Test Content

This release case validates the material-level `ztest_mode` feature from the
public Python/RNA surface down to the Eevee render paths that compare depth in
the opaque prepass and forward transparent rendering.

The script creates fresh materials in a background Blender session and verifies
that:

- `Material.ztest_mode` defaults to `LESS_EQUAL`.
- The enum contains `LESS`, `GREATER`, `LESS_EQUAL`, `GREATER_EQUAL`, `EQUAL`,
  `NOT_EQUAL`, `ALWAYS`, and `NEVER`.
- Every enum value can be assigned.
- A saved `.blend` keeps a `NOT_EQUAL` material value after reopening.
- `gpu.state.depth_test_set` documents the `NOT_EQUAL` mode.

It then renders small Eevee scenes with a nearer red plane and a farther blue
plane. The farther blue material is rendered once per ZTest mode through the
opaque prepass path and once through the forward transparent path. The center
pixel must match the expected depth-test result for each mode.

## Pass Criteria

- RNA default, enum coverage, enum assignment, and saved-file persistence pass.
- Python GPU depth-test API documentation includes `NOT_EQUAL`.
- Opaque and transparent render matrices produce red for `LESS`, `LESS_EQUAL`,
  `EQUAL`, and `NEVER`, and blue for `GREATER`, `GREATER_EQUAL`, `NOT_EQUAL`,
  and `ALWAYS`.
- The generated `.blend`, PNG renders, and `summary.json` are written under
  this case's `out` directory.

## Test Entry

`run.py`
