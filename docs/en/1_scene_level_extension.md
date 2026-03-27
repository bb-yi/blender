# Scene-Level Eevee Extensions

## Overview

Scene-level Eevee extensions consist of two main features:

1. **Render Textures** - Additional render texture system
2. **Filter Materials** - Full-screen filter stack

Both features are configured in scene properties and provide powerful rendering and post-processing capabilities for Eevee.

## 1. Render Textures

### Description

`Render Textures` is a scene-level Eevee additional render texture system.

It allows a scene to maintain up to `4` Render Texture slots, where each slot can specify a camera and output type. The scene is rendered from that camera perspective into a texture, which can then be sampled in object materials using the `Render Texture` node.

### Panel Location

`Scene Properties > Render Textures`

![Render Textures Panel](images/SnowShot_2026-03-28_04-37-13.png)

### Configurable Parameters

Each Render Texture entry supports:

- `Name` - Identifier name for referencing in nodes

- `Enabled` - Enable/disable rendering for this texture

- `Source` - Select what content to capture
  - `Color` - Capture final Eevee color
  - `Depth` - Capture linear depth
  - `Normal` - Capture surface normals

- `Camera` - Specify camera used to render this texture

- `Resolution X / Y` - Output texture pixel dimensions (higher resolution uses more VRAM)

- `Update Mode` - Control texture update frequency
  - `Every Sample` - Update on each sample
  - `Every Frame` - Update once per frame
  - `Manual` - Update manually

- `Format` - Data precision for output
  - `RGBA16F` - 16-bit float color, balanced quality/performance
  - `RGBA32F` - 32-bit high-precision color, requires more VRAM
  - `R16F` - 16-bit float depth
  - `R32F` - 32-bit high-precision depth

### Basic Usage

1. Open `Scene Properties > Render Textures`

2. Create a new `Render Texture` entry

3. Select `Source`, `Camera`, resolution, update mode, and format

4. In an object material, add `Add > Texture > Render Texture` node

5. In the node panel, select the corresponding `Render Texture` entry

6. Use the node's `Color` / `Alpha` outputs in subsequent material calculations

!!! tip
    Render Textures are ideal for real-time reflections, screen-space effects, and dynamic textures.

## 2. Filter Materials

### Description

A scene-level Eevee full-screen filter stack. Each entry is a `Filter` domain material, applied in order to process each frame.

### Panel Location

`Scene Properties > Filter Materials`

![Filter Materials Panel](images/SnowShot_2026-03-28_04-41-53.png)

Node Editor: Shader Editor > Shader Type > Filter

![Filter Shader Type Selection](images/SnowShot_2026-03-28_04-42-42.png)

### Basic Usage

1. Open `Scene Properties > Filter Materials`

2. Create a new entry or click `New Filter Material`

3. The selected material must be a `Filter` domain material

4. Open Shader Editor and switch Shader Type to `Filter`

5. Use `Scene Color` node to read scene data, output with `Filter Output` node

6. Choose `Execution Stage` to control filter execution order

### Important Notes

- `Scene Color` node samples from Texture Coordinate's `Window` output by default
- Supports `AOV` inputs
- `Execution Stage` positions:
  - `Before Volume Fog` - Execute before volume fog (good for base color adjustment)
  - `Before Depth of Field` - Execute before depth of field (good for detail enhancement)
  - `Before Composite` - Execute before compositor (good for final color grading)

!!! warning
    Filter Materials can only use Filter domain materials. Other domain types are not compatible.

!!! example "Use Cases"
    - Color grading and tone mapping
    - Screen-space effects (edge detection, toon rendering)
    - Motion blur and motion vector effects
    - Post-process denoising and sharpening
