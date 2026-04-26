# Scene-Level Eevee Extensions

## 1. Render Textures

#### Feature Description

`Render Textures` is an additional scene-level Eevee render-texture system.

It lets a scene maintain up to `4` render texture slots. Each slot can point to a camera and an output type, rendering that camera view into a texture that can later be sampled by the `Render Texture` node in regular object materials.

#### Panel Entry

`Scene Properties > Render Textures`

<div align="center">
  <img src="images/SnowShot_2026-03-28_04-37-13.png" alt="Render Textures" style="border-radius: 10px;">
  <br>
</div>

#### Configurable Fields

Each `Render Texture` entry currently supports:

- `Name`
- `Enabled`
- `Source`
  - `Color`: Capture the final Eevee color
  - `Depth`: Capture linear depth
  - `Normal`: Capture normals
- `Camera`
- `Resolution X / Y`
- `Update Mode`
  - `Every Sample`
  - `Every Frame`
  - `Manual`
- `Format`
  - `RGBA16F`
  - `RGBA32F`
  - `R16F`
  - `R32F`

#### Basic Workflow

1. Open `Scene Properties > Render Textures`.
2. Create a new `Render Texture` entry.
3. Choose `Source`, `Camera`, resolution, update mode, and format.
4. Add a `Add > Texture > Render Texture` node in a regular object material.
5. Pick the matching `Render Texture` entry in the node panel.
6. Use the node's `Color` / `Alpha` outputs in the rest of the material.

## 2. Filter Materials

#### Feature Description

`Filter Materials` is a scene-level Eevee full-screen filter stack. Each entry is a material in the `Filter` domain, and the stack is applied in list order to the current frame.

#### Panel Entry

`Scene Properties > Filter Materials`

<div align="center">
  <img src="images/SnowShot_2026-03-28_04-41-53.png" alt="Filter Materials" style="border-radius: 10px;">
  <br>
</div>

Node-tree entry: Shader Editor > Shader Type > `Filter`

<div align="center">
  <img src="images/SnowShot_2026-03-28_04-42-42.png" alt="Filter Shader Type" style="border-radius: 10px;">
  <br>
</div>

#### Basic Workflow

1. Open `Scene Properties > Filter Materials`.
2. Create a new entry, or click `New Filter Material`.
3. The selected material must be a `Filter` domain material.
4. In the Shader Editor, switch the top `Shader Type` to `Filter`.
5. Use `Scene Color` to read scene data, and combine it with `Filter Object Info` or `Filter Mask` when object-driven control is needed.
6. Use `AOV Input` if the filter needs to read an existing custom pass.
7. Use `AOV Output` if the filter should write an intermediate or final result into a named AOV.
8. Choose the insertion point with `Execution Stage`.

#### Important Notes

- The default sampling coordinate for `Scene Color` is the `Window` output of the `Texture Coordinate` node
- `AOV Input` is supported
- `AOV Output` is supported, so a filter can write to a named AOV before sending the final result into `Filter Output`
- `Execution Stage` currently provides three insertion points:
  - `Before Volume Fog`
  - `Before Depth of Field`
  - `Before Composite`

## 3. Eevee Outline

#### Feature Description

`Eevee Outline` is the scene-level master switch for the built-in screen-space outline system in the NPR Port.

#### Panel Entry

`Render Properties > Outline`

Outline render-pass entry:

`View Layer Properties > Passes > Data > Outline`

#### Behavior

- Enabled by default so `Outline Control` nodes and the `Outline` render pass work normally
- When disabled, `Outline Control` no longer affects the Combined result
- When disabled, the `Outline` render pass also stops producing outline data
- The toggle is meant as a quick way to return Eevee to the same look as a build without the outline system
- If the `Outline` render pass is not enabled, outlines are composited into `Combined`
- If the `Outline` render pass is enabled, outline data can be read as a separate pass in compositing or later processing
- No outlines are generated unless a material actually writes outline parameters through `Outline Control`

#### Suggested Images

- `images/placeholder_eevee_outline.png`
  - Suggested content: the `Render Properties > Outline` panel
- `images/placeholder_outline_render_pass.png`
  - Suggested content: the `View Layer > Passes > Data > Outline` toggle, or a compositor example reading the `Outline` pass
