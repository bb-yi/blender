# New Shader Nodes Render

## Purpose

Verifies representative newer NPR/Goo shader nodes actually affect Eevee render
output, not just Blender RNA registration.

This case renders small procedural scenes that exercise:

- `Scene Time`
- `Render Texture`
- `Portal In` / `Portal Out`
- `Twirl`
- `Hex Grid Texture`

## Pass Criteria

- `Scene Time` scaled frame drives the center pixel close to `2.0` at frame 20
  with scale 10.
- An unbound `Render Texture` outputs black.
- An active `Render Texture` used by an ordinary Surface material captures the configured camera's
  red color and front-facing normal without shader compilation errors or a native crash.
- `Portal Out` forwards the `Portal In` color.
- `Twirl` with amount 8 materially changes the image compared with amount 0.
- `Hex Grid Texture` produces visible color variation across the image.

## Test Entry

`run.py`
