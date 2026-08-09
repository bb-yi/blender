# npr-test_goo_shader_info_reflection_plane

## Test Content

Loads the copied `shader info 反射平面测试.blend` asset, renders the Eevee reflection plane scene, and checks the reflection brightness on the left and right monkey regions.

## Pass Criteria

- The asset opens in a factory-startup background Blender session.
- The scene still contains one direct `Shader Info` material path and one NPR `Shader Info` material path.
- The left reflection area is visibly lit after the fix.
- The right reflection area stays visible as a control.

## Test Entry

`run.py`

## Original Asset

`test\shader info 反射平面测试.blend`
