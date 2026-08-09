# eevee-viewport-aov-through-transmission

## Test Coverage

This case protects a viewport-only Eevee regression from the local repro scene
`AOV through transmission`.

The scene has:

- A view-layer color AOV named `AOV`.
- A behind object material that writes the AOV.
- A foreground ray-traced transmissive object whose NPR Tree reads that AOV.

Final/background render already works. The bug was that rendered viewport did not allocate the
AOV buffer for regular material NPR AOV Input users, so the transmissive foreground region became
black even though render output stayed correct.

## Pass Criteria

- The child viewport process exits successfully.
- The repro blend still defines the `AOV` color AOV.
- A background Eevee render of the repro scene is non-black.
- The rendered viewport screenshot has a bright, non-black foreground transmission region matching
  the AOV data behind it.

## Entry Point

`run.py`
