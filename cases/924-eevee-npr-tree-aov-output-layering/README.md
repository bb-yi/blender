# EEVEE NPR Tree AOV output layering

## Test Content

This case first checks that the EEVEE NPR Tree Add menu exposes AOV Output
while the Filter Tree Add menu keeps it hidden. It then renders two
programmatically generated EEVEE scenes and reads the actual Render Layers AOV
outputs from a multilayer EXR. It also routes one AOV through a Filter Graph and
checks the resulting Combined pass independently.

The opaque scene verifies NPR-only Color and Value outputs, a current Surface
AOV read through NPR Input, and NPR overwrite of an AOV already written by the
Surface tree. The layered scene places a smaller ray-traced transmission plane
in front of an opaque AOV writer so that NPR Input must read the previous
deferred layer.

## Pass Criteria

- AOV Output is present in the NPR Tree Add menu and absent from the Filter
  Tree Add menu.
- The opaque NPR Tree writes red to its Color AOV and `0.625` to its Value AOV.
- NPR Input copies a green current-Surface AOV to a second AOV.
- NPR Output AOV overwrites a green Surface AOV with red.
- The front transmission layer reads the green back-layer AOV and writes it to
  a new AOV inside the front plane.
- The front layer overwrites a blue back-layer AOV with red only inside its
  coverage; outside the front plane the blue value remains.
- Filter Graph output matches the direct NPR-written AOV in both scenes.
- All expected values use an absolute tolerance of `0.02`.
