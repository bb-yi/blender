# npr-test_goo_screen_derivative_render

## Test Content

Verifies the `Screen Derivative` node in a real Eevee render.

The material feeds `X + Y` from Geometry Position into the node and measures
the rendered output for `DDX`, `DDY`, and `DDXY`.

## Pass Criteria

- `DDX` is non-zero and positive.
- `DDY` is non-zero and positive.
- `DDXY` is positive and stays within `0.002` of `DDX + DDY`.

The tolerance intentionally allows small EXR/render quantization drift while
still catching a real mismatch in the derivative combination logic.

## Test Entry

`run.py`

## Original Test

`blender_5_1_port\tests\python\npr\test_goo_screen_derivative_render.py`
