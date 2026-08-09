# npr-test_goo_shader_info_smoke

## Test Content

Verifies that the `Shader Info` node is registered with the expected input and output socket shape.

## Pass Criteria

- `ShaderNodeShaderInfo` exists and has the label `Shader Info`.
- Input sockets are `World Position`, `Normal`, and `Exponent`.
- Output sockets are `Diffuse Shading`, `Shadow`, `Ambient Lighting`, `Half-Lambert Factor`, `Blinn-Phong Factor`, `Self Shadow`, and `Cast Shadow`.

## Test Entry

`run.py`

## Original Test

`blender_5_1_port\tests\python\npr\test_goo_shader_info_smoke.py`
