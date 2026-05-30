# NPR Tree Workflow

## 1. Basic Concept

`NPR Tree` is a second shading graph attached to a regular object material. It is used to reorganize and stylize Eevee material results in an NPR-oriented way.

The regular object material still handles the base surface shading, while the `NPR Tree` adds an extra NPR presentation layer on top.

## 2. How to Attach It

1. Keep the normal `Material Output` and base shading in the regular object material.
2. Select the `Material Output` node.
3. Create or assign a node group in its `NPR Tree` property.

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-18-15.png" alt="NPR Tree Slot" style="border-radius: 10px;">
	<br>
</div>

4. To edit this tree, switch the Shader Editor top `Shader Type` to `NPR`.

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-18-33.png" alt="NPR Shader Type" style="border-radius: 10px;">
	<br>
</div>

## 3. Notes

- The material render mode needs to be set to `Dithered (Deferred)`
- `Ctrl + Tab` can be used to switch quickly between the regular object material and the NPR tree
- `NPR Tree` keyframes and drivers use a dedicated `NPR Tree Action`, which can be viewed, switched, or created in `Material Properties > Animation > NPR Tree Action`

<div align="center">
	<img src="images/SnowShot_2026-03-31_03-35-17.png" alt="NPR Tree Action" style="border-radius: 10px;">
	<br>
</div>

## 4. Main NPR Nodes

In addition to the dedicated NPR nodes below, `Curvature`, `Raycast`, `GLSL Function`, and `GLSL Script Expression` can also be used directly inside `NPR Tree`.

### NPR Input

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-22-04.png" alt="NPR Input" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Reads the input buffers provided by the NPR rendering stage.

#### Outputs

- `Combined Color`
- `Diffuse Color`
- `Diffuse Direct`
- `Diffuse Indirect`
- `Specular Color`
- `Specular Direct`
- `Specular Indirect`
- `Position`
- `Normal`

These outputs are closer to image / texture handles than ordinary scalar values, so they are typically passed to `Image Sample` or other NPR-aware nodes for further processing.

### NPR Refraction

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-22-16.png" alt="NPR Refraction" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Reads refraction-related buffers, similar in spirit to `Screenspace Info`.

#### Outputs

- `Combined Color`
- `Position`

### Image Sample

#### Entry

`Add > Utilities > Image Sample`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-25-34.png" alt="Image Sample" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Inputs: `Image`, `Offset`
- Output: `Color`

#### Purpose

Samples image-style handles such as those coming from `NPR Input` or `NPR Refraction`.

#### Offset Modes

- `View`: offset in view space
- `Pixel`: offset in pixel space

### For Each Light

#### Entry

`Add > Utilities > For Each Light`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-26-08.png" alt="For Each Light" style="border-radius: 10px;">
	<br>
</div>

#### Notes

This node runs its internal logic once for every light affecting the current surface and outputs per-light information during each iteration.

#### Built-In Data

`For Each Light Input` currently provides:

- Input: `Normal`
- Outputs: `Color`, `Direction`, `Distance`, `Attenuation`, `Shadow Mask`

It also supports custom area input / output sockets so intermediate values can be passed through the per-light loop.

## 5. Built-In NPR Node Group Assets

This version already ports a group of commonly used node groups from the Blender 4.4 NPR branch and repackages them as 5.1-compatible assets.

### Main Built-In Assets

- `Cavity`
- `Co-Planar Edge Detection`
- `Curvature`
- `Kuwahara`
- `Shading Models`
- `Surface Curvature`

### Asset Notes

- These node groups have been migrated to the Blender 5.1 format
- Because repeat-zone node names changed, old Blender 4.4 NPR Prototype files may need to reconnect region-related nodes manually

!!! warning "Eevee Only"
    `NPR Tree` currently supports only `Eevee`, not `Cycles`.
