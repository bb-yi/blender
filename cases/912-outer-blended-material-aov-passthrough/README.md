# outer-blended-material-aov-passthrough

This case opens `assets/blended_material_aov_passthrough.blend`, a copy of the
local repro scene for the blended-material AOV occlusion bug.

The scene has a Suzanne material that writes a pure red color AOV named `AOV`.
A foreground screen uses Eevee `BLENDED` surface rendering while its Principled
Alpha remains `1.0`. The compositor outputs only the AOV.

## Test Content

- Verifies the view layer still contains the `AOV` color pass.
- Verifies the foreground material remains `BLENDED` / `BLEND` with Principled
  Alpha `1.0`.
- Verifies at least one material contains `ShaderNodeOutputAOV`.
- Renders the scene in Eevee and inspects the saved compositor output.

## Pass Criteria

The rendered output must contain a substantial red-dominant region from the
behind-Suzanne AOV. A black output fails the case because that means the
foreground `BLENDED` material cleared or occluded the AOV buffer.
