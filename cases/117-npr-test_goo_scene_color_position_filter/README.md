# npr-test_goo_scene_color_position_filter

## Test Content

This case verifies the Filter material `Scene Color` node with `POSITION` source.

It renders an orthographic Eevee scene at `257x257`, a deliberately non-64-multiple resolution. The filter material reads `Scene Color(Position)`, encodes world-space `X` into red and `Z` into green, and writes the encoded values to the final image.

This catches regressions where the Position render pass is reconstructed from a padded HiZ texture size instead of the real fullscreen `screen_uv`, which shifts the reported world position at fixed offsets.

## Pass Criteria

- `scene_color.source = "POSITION"` must read back as `POSITION`.
- The render size must be exactly `257x257`.
- The center pixel and multiple off-center pixels must decode to the expected orthographic world-space `X/Z` coordinates.
- Each sampled `X/Z` coordinate must be within `0.03` world units of the expected value.

## Entry Point

`run.py`

## Original Test

`blender_5_1_port_mainfix\tests\python\npr\test_goo_scene_color_position_filter.py`
