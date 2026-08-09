# eevee-referenced-object-data-channel

## Test content

This case exercises the reusable Eevee referenced-object data contract before and after the
Draw Manager implementation:

- Light Info exposes one fixed set of enabled outputs for Point, Sun, Spot, and Area lights.
- Two materials can reference the same light and observe the same frame data.
- Transform, color, energy, type, and visibility changes update values without rebuilding the
  compiled material shader.
- A regular Object Attribute works both alone and beside a referenced-object node.
- Two referenced lights whose UIDs map to the same initial slot in a four-slot table both render
  their own colors. The test reports the expected collision/probe shape but does not claim native
  Draw Manager probe telemetry.
- Empty and removed light references produce the documented safe default instead of a crash, and
  rebinding the node resolves the replacement UID and color.

## Pass criteria

The Blender Python process exits successfully and every assertion in the source test passes.
The output must include the timestamp, rendered collision, attribute-only, attribute-coexistence,
empty-target, deleted-target, and rebound-target markers; a changing shader timestamp after the
warm-up is a failure. The background release runner may report `NOT_COMPILED`/zero for a pass that
is released immediately after a one-shot render, so the timestamp is treated as an observational
signal and not a standalone compile success check.

## Entry point

`run.py`

## Source test

`blender_npr_post/tests/python/npr/test_referenced_object_data_channel.py`
