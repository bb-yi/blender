# Parallax Closure Node

## Test Content

Builds a minimal Eevee scene with a UV-mapped plane, generated color and height
images, and native `ShaderNodeParallax` materials. The case probes the node
interface for all supported modes, including hidden `Refinement Steps` in
Parallax Occlusion, visible `Refinement Steps` in the relief modes, `Normal`
output exposure, and the world-space sun direction socket used by parallax
shadow.

The render path covers Plane Offset, Parallax Occlusion, Relief Parallax
Mapping, Secant Method Relief Mapping, Image to Closure height sampling,
Closure Output height sampling with both Image Texture and 3D Noise Texture
height inputs, Normal output generation, Offset behavior, single-direction
parallax shadow, and undo memfile round-tripping for Parallax node storage.

## Pass Criteria

- The Parallax node exposes `UV` and `Normal`, and exposes `Shadow` only when
  shadow is enabled.
- Plane Offset exposes only `UV` and `Scale` inputs.
- Parallax Occlusion exposes no `Refinement Steps` input.
- Relief and Secant Relief expose `Refinement Steps`.
- The shadow input is named `Sun Direction (World Space)`.
- `Scale = 0` renders the same as direct UV.
- Positive and negative Plane Offset scales render differently.
- Height Offset changes the Parallax Occlusion render.
- Image to Closure and Closure Output Image Texture height sources both affect
  UVs.
- Closure Output 3D Noise height affects UVs, and changing only the input UV Z
  coordinate changes the render.
- Undoing a Parallax node socket change must read the memfile back without
  crashing and must restore the socket value.
- Connected `Normal` output changes with height.
- Oblique world-space sun direction changes the parallax shadow result.

## Test Entry

`run.py`
