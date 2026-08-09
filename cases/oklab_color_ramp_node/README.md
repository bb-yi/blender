# oklab-color-ramp-node

## Test Content

This case verifies the standalone `OKLab Color Ramp` node restored from the Goo-style workflow.

It checks that `ShaderNodeOKLabColorRamp` is available as a common node in Shader, Geometry, Compositor, and Eevee Light node trees, while the regular `Color Ramp` node keeps only the public RGB / HSV / HSL color modes. The OKLab node itself must keep `OKLAB` as its internal storage mode.

It also saves and reloads a file containing the OKLab node, opens a legacy file saved as `ShaderNodeValToRGB + OKLAB` to verify migration back to the independent node, and renders a minimal Eevee scene that uses OKLab nodes in both the material and light shader graphs, so missing registration or GPU shader functions fail the test.

## Pass Criteria

- `ShaderNodeOKLabColorRamp` can be created in Shader, Geometry, Compositor, and Light node trees.
- `ShaderNodeValToRGB.color_ramp.color_mode` does not expose `OKLAB`.
- `ShaderNodeOKLabColorRamp.color_ramp.color_mode` exposes only `OKLAB`.
- OKLab nodes survive save and reopen as `ShaderNodeOKLabColorRamp`.
- The legacy `ShaderNodeValToRGB + OKLAB` asset opens as `ShaderNodeOKLabColorRamp`.
- A minimal Eevee render using OKLab material and light shader nodes finishes successfully.

## Entry Point

`run.py`
