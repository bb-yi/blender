# outer-transmission_aov_occlusion_real_blend

## Test Content

Opens the exact local repro scene `assets/透射AOV遮挡测试.blend`.

The scene has a Suzanne behind a transmissive sphere. Suzanne writes a pure red
`AOV_001`, and the sphere has an attached NPR Tree that reads `AOV_001` through
an `AOV Input` node and outputs it as the visible material color.

## Pass Criteria

- The view layer must still contain the `AOV_001` color AOV.
- The front sphere material must keep an attached NPR Tree with an `AOV Input`
  node reading `AOV_001`.
- After Eevee render, the center region covered by the sphere must be red
  dominant and non-black.
- A background corner must stay dark, so the test does not pass because the
  whole image became red.

This catches the regression where transparent or transmissive foreground layers
clear the AOV buffer for the surface behind them, making the NPR AOV read return
black.

## Test Entry

`run.py`
