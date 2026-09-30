# EXR layer color-space compatibility fixture

`legacy-settings.blend` was generated with the unmodified NPR Release build
`680997dc4ccb` (2026-09-11), before the per-layer color-space feature.
It contains a File Output node and a generated image using sRGB.
The regression checks that missing output override flags stay disabled and
existing images retain global color-space interpretation.

Used by `tests/python/npr/test_exr_layer_color_spaces.py`.
