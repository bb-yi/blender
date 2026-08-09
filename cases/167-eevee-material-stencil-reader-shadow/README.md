# EEVEE Material Stencil Reader Shadow

## Test Content

This case verifies that a read-only material stencil reader remains a normal EEVEE
shadow caster. The scene has a small stencil writer patch, a larger floor that
receives shadows, and a red reader sphere masked by the stencil test.

## Pass Criteria

- The masked red reader is visible in the default render.
- The default render has a measurable floor shadow compared with an explicit
  `visible_shadow = False` render of the same reader object.
- This catches accidental fixes that make stencil readers stop casting shadows
  entirely.
