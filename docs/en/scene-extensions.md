# Scene-Level Eevee Extensions

## 1. Render Textures

### Feature Description

`Render Textures` is a scene-level Eevee extra render texture system.

It allows the scene to maintain up to `4` Render Texture slots in advance. Each slot can specify a camera and an output type, render the scene from that camera into a texture first, and then let regular object materials sample it through the `Render Texture` node.

### Panel Entry Point

`Scene Properties > Render Textures`

<div align="center">
    <img src="images/SnowShot_2026-03-28_04-37-13.png" alt="Render Textures Panel" style="border-radius: 10px;">
    <br>
</div>

### Configurable Parameters

Each Render Texture entry currently supports:

- `Name`: Identifier of the texture entry, used for node references

- `Enabled`: Enable or disable rendering of this texture

- `Source`: Select what to capture

    - `Color`: Capture the final Eevee color

    - `Depth`: Capture linear depth

    - `Normal`: Capture normals

- `Camera`: Camera used to render this texture

- `Resolution X / Y`: Output texture size in pixels; higher resolution uses more memory

- `Update Mode`: Controls how often the texture is updated

    - `Every Sample`: Update on every sample

    - `Every Frame`: Update once per frame

    - `Manual`: Update manually

- `Format`: Output data precision

    - `RGBA16F`: 16-bit floating-point color, balanced quality and performance

    - `RGBA32F`: 32-bit high-precision color, requires more VRAM

    - `R16F`: 16-bit floating-point depth value

    - `R32F`: 32-bit high-precision depth value

### Basic Usage

1. Open `Scene Properties > Render Textures`.

2. Create a `Render Texture` entry.

3. Select `Source`, `Camera`, resolution, update mode, and format.

4. Add `Add > Texture > Render Texture` in a regular object material.

5. Select the matching `Render Texture` entry in the node panel.

6. Use the node's `Color` / `Alpha` output in later material calculations.

## 2. Filter Materials

### Feature Description

This is a scene-level Eevee full-screen filter stack. Each entry is a `Filter` domain material, and the current frame is processed in list order.

### Panel Entry Point

`Scene Properties > Filter Materials`

<div align="center">
    <img src="images/SnowShot_2026-03-28_04-41-53.png" alt="Filter Materials Panel" style="border-radius: 10px;">
    <br>
</div>

Node tree entry: Shader Node Editor > Shader Type > Filter

<div align="center">
    <img src="images/SnowShot_2026-03-28_04-42-42.png" alt="Filter Shader Type" style="border-radius: 10px;">
    <br>
</div>

### Basic Usage

1. Open `Scene Properties > Filter Materials`.

2. Create an entry, or click `New Filter Material` directly.

3. The selected material must be a `Filter` domain material.

4. Open Shader Editor and switch the top `Shader Type` to `Filter`.

5. Use `Scene Color` to read scene data, and use `Filter Object Info` when you want the filter to react to a chosen object's transform or viewport color.

6. Use `Execution Stage` to choose where the filter runs.

### Important Notes

- The default sampling coordinate of `Scene Color` is the `Window` output of the `Texture Coordinate` node

- `AOV` input is supported

- `Execution Stage` insertion points:

    - `Before Volume Fog`: Execute before volume fog processing

    - `Before Depth of Field`: Execute before depth of field

    - `Before Composite`: Execute before the compositor
