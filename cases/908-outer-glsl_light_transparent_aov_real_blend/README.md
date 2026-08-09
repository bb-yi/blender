# outer-glsl_light_transparent_aov_real_blend

## Test Content

Opens the exact repro scene `assets/灯光函数透明aov报错.blend`.

That blend contains an Eevee material named `Wuwa_up` with a transparent
shader path, a `GLSL Function` node that uses light-access helpers, and an
`AOV Output` node targeting the `PBR` color AOV. This case protects the
real-scene regression where the generated fragment shader failed in
`gpu_shader_material_glsl_light_access.glsl` with a syntax error around an
unused-variable fallback.

## Pass Criteria

- The blend must still contain a mesh material using both
  `ShaderNodeGLSLFunction` and `ShaderNodeOutputAOV`.
- The active view layer must still define the `PBR` color AOV.
- A clean background Eevee render with `--log gpu.shader` must complete.
- The child render must print `GLSL_LIGHT_TRANSPARENT_AOV_REPRO_OK`.
- The shader log must reference `gpu_shader_material_glsl_light_access.glsl`
  and must not contain `gpu.shader` ERROR, `C0000`, `syntax error`, or
  `unexpected ')'`.

## Test Entry

`run.py`
