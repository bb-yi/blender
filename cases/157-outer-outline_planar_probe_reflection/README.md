# outer-outline_planar_probe_reflection

## Test Content

Loads the copied `描边平面探头测试.blend` asset, renders the Eevee planar reflection scene, and checks that the visible outline on the source monkey remains present while the reflected outline stays bounded and does not blow up into a full-screen artifact.

## Pass Criteria

- The asset opens in a factory-startup background Blender session.
- The scene contains one direct `ShaderNodeOutlineControl` material path.
- The top monkey region keeps a strong red outline.
- The bottom reflected monkey region keeps a red outline, but it must stay bounded and not exceed the source outline by a large margin.

## Test Entry

`run.py`

## Original Asset

`test\描边平面探头测试.blend`
