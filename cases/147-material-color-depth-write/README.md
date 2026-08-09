# Material Color Write / Depth Write

## Test Content

This release case validates the material-level `use_color_write` and
`use_depth_write` properties from the public Python/RNA surface down to the
Eevee render path.

The script creates fresh materials in a background Blender session and verifies
that:

- `Material.use_color_write` and `Material.use_depth_write` both default to
  `True` on a new material.
- Both properties accept assignment to `False` and back to `True`.
- A saved `.blend` keeps `False/False` after reopening.

It then renders four Eevee scenes with a black world background and a near red
plane occluding a far blue plane. Only the near red plane's material switches
between the four combinations of color/depth write while the back blue plane
keeps the defaults.

| Front material | Expected center pixel | Why                                                       |
| -------------- | --------------------- | --------------------------------------------------------- |
| color=on,  depth=on  | red                   | default: front shades red and occludes the blue plane     |
| color=off, depth=on  | black (world clear)   | front skips color but still occludes the blue plane       |
| color=on,  depth=off | red                   | front shades red but lets later draws keep their depth    |
| color=off, depth=off | blue                  | front skips both writes so the blue plane shows through   |

## Pass Criteria

- RNA defaults, enum assignment for both properties, and saved-file persistence
  pass.
- Eevee renders match the table above on the center pixel.
- Generated `.blend`, PNG renders, and `summary.json` are written under this
  case's `out` directory.

## Test Entry

`run.py`
