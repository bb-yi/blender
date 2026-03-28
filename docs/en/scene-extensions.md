# Scene-Level Eevee Extensions

## 1. Render Textures

### Feature Description

`Render Textures` is a scene-level Eevee additional render texture system.

It allows the scene to maintain up to `4` Render Texture slots, each of which can specify a camera and an output type. The scene result from that camera perspective is first rendered into a texture, then sampled in regular object materials through the `Render Texture` node.

### Panel Entry Point

`Scene Properties > Render Textures`

### Configurable Parameters

Each Render Texture entry currently supports:

| Parameter | Description |
|-----------|-------------|
| `Name` | Identifier name of the texture entry, used for referencing in nodes |
| `Enabled` | Enable or disable rendering of this texture |
| `Source` | Choose what to capture: Color / Depth / Normal |
| `Camera` | Specify the camera for rendering this texture |
| `Resolution X/Y` | Output texture pixel dimensions |
| `Update Mode` | Update frequency: Every Sample / Every Frame / Manual |
| `Format` | Output precision: RGBA16F / RGBA32F / R16F / R32F |

### Basic Usage

1. Open `Scene Properties > Render Textures`
2. Create a new entry
3. Select Source, Camera, resolution, and format
4. Add a `Render Texture` node in object materials
5. Select the corresponding entry in the node panel
6. Use the node output in material calculations

!!! tip "Tip"
        Perfect for creating real-time reflections, screen-space effects, and dynamic textures.

## 2. Filter Materials

### Feature Description

It is a scene-level Eevee full-screen filter stack. Each entry is a `Filter` domain material that processes the current frame in list order.

### Panel Entry Point

`Scene Properties > Filter Materials`

Node tree entry point: Shader Node Editor > Shader Type > Filter

### Basic Usage

1. Open `Scene Properties > Filter Materials`
2. Create a new entry or click `New Filter Material`
3. The selected material must be a `Filter` domain material
4. Open the Shader Editor and switch the top `Shader Type` to `Filter`
5. Use `Scene Color` in the filter material to read scene data, then output through `Filter Output`
6. Choose the execution location through `Execution Stage`

### Important Notes

- The default sampling coordinate for `Scene Color` is the `Window` output of the `Texture Coordinate` node

- `AOV` input is supported

- Execution stages:

    - `Before Volume Fog`

    - `Before Depth of Field`

    - `Before Composite`
3. The selected material must be a `Filter` domain material
4. Open Shader Editor and switch the top `Shader Type` to `Filter`
5. Use `Scene Color` to read scene data
6. Output the result with `Filter Output`
7. Select filter execution position via `Execution Stage`

### Execution Stages

- `Before Volume Fog` - Execute before volume fog processing
- `Before Depth of Field` - Execute before depth of field
- `Before Composite` - Execute before compositing

!!! warning "Note"
    Filter Materials can only use Filter domain materials.

!!! example "Use Cases"
    - Color grading and tone mapping
    - Screen-space effects (e.g., edge detection, toon rendering)
    - Motion blur and motion vector effects
    - Post-process denoising and sharpening
