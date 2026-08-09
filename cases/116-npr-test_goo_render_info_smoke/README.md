# npr-test_goo_render_info_smoke

## Test Content

Verify `ShaderNodeRenderInfo` registration and socket shape.

## Pass Criteria

- `ShaderNodeRenderInfo` exists and is labeled `Render Info`.
- The node has no input sockets.
- Outputs are `Frag Coord`, `Width`, `Height`, `Resolution`, `Current Sample`, and `Total Samples`.
- Vector and value sockets use the expected Blender socket types.

## Entry Point

`run.py`

## Original Test

`blender_5_1_port\tests\python\npr\test_goo_render_info_smoke.py`
