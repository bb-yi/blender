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

## 2. Filter Graph

#### Feature Description

`Filter Graph` is a scene-level Eevee full-screen filter execution graph. It replaces the legacy linear Filter Materials list with node connections. The scene selects its authoritative graph through `Scene.eevee.filter_graph`; image handles in that graph can feed multiple Filter Passes and publish results at different render stages.

#### Panel and Editor Entry

- Create or assign the scene Filter Graph at `Scene Properties > Filter Graph`.
- Switch any area to the `Eevee Filter Graph` editor to edit the graph assigned to the current scene.
- A `Filter Pass` still uses a material in the `Filter` domain. Enter that material to edit its internal node tree in the Shader Editor.

#### Filter Graph Nodes

- `Scene Color`: exposes the current stage's `Color Image`, `Depth Image`, `Normal Image`, and `Position Image` handles.
- `AOV Input`: reads a named View Layer AOV and exposes `Color` and `Value` image handles.
- `Filter Pass`: executes one `Filter` domain material. Its input interface is synchronized from the material's `Pass Input`, and its output interface is synchronized from `Filter Output`.
- `Stage Output`: publishes the connected image at the selected render stage. Each stage can have only one active `Stage Output`.

#### Inside a Filter Material

Creating a Filter Pass material builds this default chain:

`Pass Input -> Image Sample -> Filter Output`

`Pass Input` receives image handles from the Filter Graph, `Image Sample` samples them at the current or an offset pixel, and `Filter Output` returns results to the graph's `Filter Pass`. Dynamic interfaces can be maintained on `Pass Input` and `Filter Output`; one Filter Pass supports at most `32` inputs and `32` outputs.

#### Basic Workflow

1. Create or assign a graph at `Scene Properties > Filter Graph`.
2. Switch to the `Eevee Filter Graph` editor.
3. Feed a `Filter Pass` from `Scene Color` or `AOV Input`.
4. Create or assign a Filter material on the `Filter Pass`, then enter the material to build the actual filter logic.
5. Connect the `Filter Pass` result to a `Stage Output`, choose its execution stage, and make sure that output is active.
6. Choose `Full`, `1/2`, `1/4`, `1/8`, or `1/16` execution resolution on each `Filter Pass` according to quality and performance needs.

#### Execution Stages

- `Before Volume Fog`
- `Before PostFX`
- `Before Depth of Field`
- `Before Composite`

If multiple `Stage Output` nodes target the same stage, only one can be active. Other stages can each maintain an independent active output chain.

#### Data-Blocks and Legacy Migration

- Filter Graphs created from the scene panel and Filter materials created from a `Filter Pass` automatically receive a fake user. Temporarily unlinking them from a scene or node therefore does not immediately make them disappear on save.
- When a file containing the 5.1 linear Filter Materials list is opened, its entries are migrated automatically into an equivalent Filter Graph, preserving their execution stages and list order in the generated connections.
- Filter materials can still use Filter-domain nodes such as `Filter Object Info`, `Filter Mask`, and `GLSL Function`; route scene buffers and AOVs at graph level when possible.

## 3. Native Camera FX Outputs

#### Feature Description

`Native Camera FX Outputs` is a View Layer level Eevee native post-effect output system. It can extract a selected render channel, apply Eevee native `Motion Blur` and / or `Depth of Field` to that channel, and publish the result as a new render pass.

This is useful for generating camera-FX versions of outlines, AOVs, depth, normals, lighting components, and other channels for compositor, filter, or external post-production workflows.

#### Panel Entry

`View Layer Properties > Passes > Native Camera FX Outputs`

#### Configurable Fields

Each output entry supports:

- `Name`: Name of the generated render pass
- `Enabled`: Whether this output is generated
- `Source`: Source channel to process
- `Shader AOV`: AOV name used when `Source` is `Shader AOV`
- `Motion Blur`: Apply Eevee native motion blur
- `Depth of Field`: Apply Eevee native depth of field

#### Supported Sources

- `Depth`
- `Normal`
- `Position`
- `Vector`
- `Diffuse Light`
- `Diffuse Color`
- `Specular Light`
- `Specular Color`
- `Volume Light`
- `Emission`
- `Environment`
- `Shadow`
- `Ambient Occlusion`
- `Transparent`
- `Shader AOV`
- `Outline`

#### Basic Workflow

1. Switch the render engine to `Eevee`.
2. Open `View Layer Properties > Passes > Native Camera FX Outputs`.
3. Add an output entry.
4. Set `Name` and `Source`.
5. Enable `Motion Blur`, `Depth of Field`, or both as needed.
6. Read the generated render pass by name in the compositor or later pipeline stage.

#### Important Notes

- `Motion Blur` still requires Eevee motion blur to be enabled for the scene / View Layer
- `Depth of Field` still uses the active camera depth-of-field settings
- `Shader AOV` sources must point to an existing AOV name on the View Layer
- Invalid entries usually indicate a name conflict, a missing source AOV, or that the current output limit has been exceeded
- The outline channel can be selected as the `Outline` source and exported with its own depth of field or motion blur

## 4. Eevee Outline

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
- Objects in Holdout collections no longer write `Outline Control` parameters and no longer contribute Freestyle / marked-edge outline seeds
- `Blended` foreground materials participate in behind-outline occlusion: fully opaque foreground blocks behind outlines, fully transparent foreground leaves them unchanged
- Semi-transparent `Blended` foreground attenuates behind-outline intensity by material transmittance without tinting the outline with the foreground material color

#### Suggested Images

- `images/placeholder_eevee_outline.png`
  - Suggested content: the `Render Properties > Outline` panel
- `images/placeholder_outline_render_pass.png`
  - Suggested content: the `View Layer > Passes > Data > Outline` toggle, or a compositor example reading the `Outline` pass
