# NPR Tree Workflow

## 1. Core Concept

`NPR Tree` is a node tree attached after a regular object material to perform color post-processing, allowing shader output to be stylized in color form.

## 2. How to Attach It

1. Keep the normal `Material Output` node and the base surface shading in a regular object material.

2. Select the `Material Output` node.

3. Create or assign a node group in its `NPR Tree` property.

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-18-15.png" alt="NPR Tree Slot" style="border-radius: 10px;">
	<br>
</div>

4. To edit that tree, switch `Shader Type` to `NPR` at the top of the Shader Editor.

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-18-33.png" alt="NPR Shader Type" style="border-radius: 10px;">
	<br>
</div>

## 3. Notes

- The material render mode needs to be set to `Dithered (Deferred)`

- You can use `Ctrl + Tab` to quickly switch between the object shader and the NPR tree

- The action used by the current `NPR Tree` can be viewed, switched, or created in `Material Properties > Animation > NPR Tree Action`; this slot is separate from the regular material action

<div align="center">
	<img src="images/SnowShot_2026-03-31_03-35-17.png" alt="NPR Tree Action" style="border-radius: 10px;">
	<br>
</div>

## 4. Main NPR Nodes

In addition to the dedicated NPR nodes below, `Curvature` and `Raycast` can now also be used directly inside `NPR Tree`.

### NPR Input

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-22-04.png" alt="NPR Input" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Reads the input buffers provided by the NPR rendering stage.

#### Outputs

- `Combined Color`: Final combined color after shading

- `Diffuse Color`: Diffuse color

- `Diffuse Direct`: Direct diffuse lighting

- `Diffuse Indirect`: Indirect diffuse lighting from ray tracing and probes

- `Specular Color`: Specular color

- `Specular Direct`: Direct specular lighting

- `Specular Indirect`: Indirect reflection

- `Position`: World-space position

- `Normal`: Surface normal

These outputs behave more like image handles / texture handles, which makes them suitable for neighborhood sampling through `Image Sample` or for feeding into NPR nodes that accept this kind of input.

### NPR Refraction

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-22-16.png" alt="NPR Refraction" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Reads refraction-related buffers, similar to `Screenspace Info`.

See the usage notes of `Screenspace Info` for setup requirements.

#### Outputs

- `Combined Color`: Final color at the refraction event

- `Position`: World-space position of the refraction hit

### Image Sample

#### Entry

`Add > Utilities > Image Sample`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-25-34.png" alt="Image Sample" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Input: `Image` (image handle), `Offset` (sample offset)

- Output: `Color` (sampled color)

#### Purpose

Samples image handles produced by nodes such as `NPR Input` or `NPR Refraction`.

#### Offset Modes

- `View`: Offset in view space

- `Pixel`: Offset in pixel space

### For Each Light

#### Entry

`Add > Utilities > For Each Light`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-26-08.png" alt="For Each Light" style="border-radius: 10px;">
	<br>
</div>

#### Notes

Executes its internal logic once for each light affecting the current surface, outputting information for one light on each iteration.

<div align="center">
	<img src="images/SnowShot_2026-03-28_06-55-43.png" alt="For Each Light Example" style="border-radius: 10px;">
	<br>
	<sub>Reference setup</sub>
</div>

#### Built-In Available Data

`For Each Light Input` currently provides:

- Input: `Normal` (surface normal)

- Outputs: `Color` (light color), `Direction` (light direction), `Distance` (light distance), `Attenuation` (light attenuation), `Shadow Mask` (shadow mask)

## Built-In NPR Node Group Assets

The current version already ports and organizes a set of commonly used node groups from the Blender 4.4 NPR build into assets that can be used in 5.1.

### Main Built-In Node Groups

- `Cavity`

- `Co-Planar Edge Detection`

- `Curvature`

- `Kuwahara`

- `Shading Models`

- `Surface Curvature`

### Asset Notes

- These node groups have already been migrated to Blender 5.1 format.

- Because some repeat-zone node names changed, projects from the 4.4 NPR Prototype may require manual reconnection of the related zone nodes.

!!! warning "Eevee Only"
    `NPR Tree` is currently supported only in `Eevee`, not in `Cycles`.
