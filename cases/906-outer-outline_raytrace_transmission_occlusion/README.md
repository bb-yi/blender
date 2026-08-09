# outer-outline_raytrace_transmission_occlusion

## Test Content

Verifies the Eevee NPR outline behavior for the local Raytrace Transmission
repro scenes:

- `test\折射通道测试.blend`: the foreground refraction plane covers Suzanne.
- `test\描边透射遮挡测试.blend`: the foreground refraction plane covers the
  left half, while the right half of Suzanne remains visible.

The case toggles `Material.001.use_screen_refraction` and
`Material.001.use_raytrace_refraction`, renders PNGs with the release install
Blender, then analyzes the output pixels.

## Pass Criteria

- In `test\折射通道测试.blend`, a raytrace-transmission foreground plane leaves no
  bright Suzanne outline pixels in the covered center/right region.
- In `test\描边透射遮挡测试.blend`, Raytrace Transmission ON and OFF produce nearly
  identical bright-outline masks in the visible right-side Suzanne region. This
  catches the regression where the outer contour remains visible but internal
  depth/normal outlines disappear when Raytrace Transmission is enabled.

## Test Entry

`run.py`
