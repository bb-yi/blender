# npr-test_render_info_sampling

## Test Content

Render three simple Eevee materials that use the `Render Info` sampling outputs.

## Pass Criteria

- `Current Sample` accumulates close to the average of samples `0, 1, 2, 3`.
- `Total Samples` renders close to `4`.
- `Current Sample -> White Noise Texture.W` compiles and renders a finite color.

## Entry Point

`run.py`

## Original Test

`blender_5_1_port\tests\python\npr\test_render_info_sampling.py`
