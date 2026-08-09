# outer-filter_object_mask_real_blend

## Test Content

Opens the exact local repro scene `assets/Filter Object Mask测试.blend`.

That blend contains an Eevee Filter Material whose color output is wired
directly from `Filter Object Mask`, with the target object set to Suzanne.
The case first checks that legacy `Scene Color` nodes were lifted out of the
Filter material into the scene Filter Graph, then renders the saved scene with
the release install Blender and samples the resulting pixels.

## Pass Criteria

- The blend must still have `ViewLayer.use_pass_cryptomatte_object` enabled.
- Converted Filter materials must not contain `ShaderNodeSceneColor`; they must
  receive scene images through `Pass Input`, and sampled legacy alpha must go
  through `Image Sample`.
- The center pixel, which lies inside Suzanne in this saved camera framing,
  must be bright white-like after render.
- A corner background pixel must stay dark.
- The bright mask must cover a substantial area and span a meaningful width
  and height, so the test does not pass because of only a few accidental bright
  pixels.

This catches the real-scene regression where `Filter Object Mask` becomes black
even though the repro blend has the Crypto Object pass enabled and the filter
material should output Suzanne's mask directly.

## Test Entry

`run.py`
