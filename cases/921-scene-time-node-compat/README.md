# Scene Time Node Compatibility

## Test Content

The test creates the unified `GeometryNodeInputSceneTime` node and verifies its `Scale` input,
`Frame`, `Seconds`, `Timeline`, and `Scaled Frame` outputs, including negative Scale behavior and
round-trip link persistence. A real Geometry Nodes modifier stores all four outputs as point
attributes while the scene uses a subframe and a non-unit frame map, so CPU evaluation is checked
against the raw scene frame rather than the time-remapped value.

The test also opens fixtures containing the removed custom shader node and the old two-output
official node. Both must migrate to the unified node while preserving the applicable links.

## Pass Criteria

- The source test prints `SCENE_TIME_NODE_COMPAT_OK` and exits successfully.
- Geometry evaluation returns the expected raw frame, seconds, normalized timeline, and scaled
  frame values with `frame_map_old=2`, `frame_map_new=1`, and a subframe.
- Scale defaults to `1.0`, accepts a negative value, and all four output links survive save/reload.
- Both legacy fixtures contain one `GeometryNodeInputSceneTime` node, no `ShaderNodeSceneTime`, and
  the expected migrated links.

## Entry Point

`run.py`
