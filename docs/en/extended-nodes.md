# Extended Shader Nodes

## 1. General Eevee Utility Nodes

### Render Info

#### Entry

`Add > Input > Render Info`

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-51-10.png" alt="Render Info" style="border-radius: 10px;">
	<br>
</div>

#### Outputs

- `Frag Coord`: Screen-space coordinate (`xy` normalized to 0-1, `z` is depth)

- `Width`: Render region width

- `Height`: Render region height

#### Purpose

Provides the coordinate and pixel size of the current Eevee render window.

### Scene Time

#### Entry

`Add > Input > Scene Time`

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-52-48.png" alt="Scene Time" style="border-radius: 10px;">
	<br>
</div>

#### Input

- `Scale`: Value used to scale frame numbers

#### Outputs

- `Frame`: Current frame number

- `Seconds`: Seconds corresponding to the current frame

- `Timeline`: Scene time remapped to 0-1 from start frame to end frame

- `Scaled Frame`: Current frame divided by `Scale`

### Screen Derivative

#### Entry

`Add > Utilities > Math > Screen Derivative`

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-53-23.png" alt="Screen Derivative" style="border-radius: 10px;">
	<br>
</div>

#### Feature

Gets differences between neighboring pixels in screen space:

- `DDX`: Screen-space derivative in X direction

- `DDY`: Screen-space derivative in Y direction

- `DDXY`: Combination of `DDX` and `DDY` (`DDX + DDY`)

### Portal In / Portal Out

#### Entry

- `Add > Layout > Portal In`

- `Add > Layout > Portal Out`

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-53-44.png" alt="Portal Nodes" style="border-radius: 10px;">
	<br>
</div>

#### Feature Description

These are “portal” nodes used to organize node links.

Their workflow can be understood as:

- `Portal In`: Store a named, typed value in the current node tree

- `Portal Out`: Retrieve that value later by name within the same node tree

#### Other Notes

- Creating a new `Portal In` automatically generates a unique name.

- `Portal Out` has a magnifier button that can jump to the matching `Portal In` location.

#### Limits

- Only recognized inside the same shader node tree.

- Does not support crossing different node trees.

- Does not automatically pass through node groups.

- Inputs with the same name should keep only one source.

## 2. Eevee Object Material Nodes

### Render Texture

#### Entry

`Add > Texture > Render Texture`

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-54-27.png" alt="Render Texture" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Reads a `Render Textures` entry configured earlier in the scene.

#### Inputs / Outputs

- Input: `Vector`
- Outputs: `Color`, `Alpha`

### Screenspace Info

#### Entry

`Add > Input > Screenspace Info`

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-56-22.png" alt="Screenspace Info" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Input: `View Position` (camera-space position)
- Outputs: `Scene Color` (scene color), `Scene Depth` (scene depth)

#### Purpose

Gets the contents of the current render buffer color or depth.

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-59-57.png" alt="Screenspace Result" style="border-radius: 10px;">
	<br>
</div>

#### Usage Notes

- `Raytracing` must be enabled in render settings
- Set material `Render Method` to `Dithered`
- Enable `Raytraced Transmission` in material options
- The default `View Position` input is `position` transformed into camera space, then with the z-axis inverted

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-59-28.png" alt="Screenspace Setup" style="border-radius: 10px;">
	<br>
</div>

### World Environment

#### Entry

`Add > Input > World Environment`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-01-56.png" alt="World Environment" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Input: `Direction` (sampling direction)
- Output: `Color` (environment color)

#### Purpose

Directly samples the `Eevee` world environment color, without depending on whether geometry exists behind the screen.

#### Notes

- Reads the world lighting probe color; probe resolution can be adjusted in the world environment

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-03-27.png" alt="World Environment Probe" style="border-radius: 10px;">
	<br>
</div>

- If `Direction` is not connected, the current surface view direction is used by default
- If `Direction` is connected, the world environment can be sampled in the specified direction

### World To Tangent

#### Entry

`Add > Utilities > Vector > World To Tangent`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-04-02.png" alt="World To Tangent" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Input: `Vector` (world-space direction)
- Output: `Vector` (tangent-space direction)

#### Purpose

Converts a world-space direction vector into the tangent space of the current surface.

#### Notes

- A `UV Map` can be specified in the node panel, and the tangent of that UV is used as the transform basis.

### Basis Transform

#### Entry

`Add > Utilities > Vector > Basis Transform`

<div align="center">
	<img src="images/SnowShot_2026-03-28_07-51-05.png" alt="Basis Transform" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Input: `Vector` (point, direction, or normal to transform)

- Input: `Origin` (origin of the custom basis, used only in `Point` mode)

- Input: `X Axis`, `Y Axis`, `Z Axis` (custom basis axes)

- Output: `Vector` (transformed result)

#### Purpose

Uses `origin + three basis axes` inside material nodes to perform custom coordinate-system transforms. This is useful when there is no matrix input type available and you still need to process points, direction vectors, or normals.

#### Panel Options

- `Direction`

	- `To Basis`: Interpret the input from world/current coordinates into the custom basis

	- `From Basis`: Convert the input from custom basis coordinates back to external coordinates

- `Type`

	- `Point`: Includes `Origin` translation

	- `Vector`: Only transforms direction and length, without translation

	- `Normal`: Transforms following normal rules and normalizes before output

- `Basis Input`

	- `XYZ`: Use all three input axes directly

	- `XY` / `XZ` / `YZ`: Use only two axes; the third axis is generated automatically by cross product

- `Orthonormalize`

	- When enabled, input axes are orthogonalized and normalized. More suitable for tangent space or local orientation bases

	- When disabled, input axis lengths are preserved, which can be used for basis transforms with scaling

- `Fallback`

	- `Pass Through`: Output the original input when the basis degenerates

	- `Zero`: Output `0, 0, 0` when the basis degenerates

### Bevel

#### Entry

`Add > Input > Bevel`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-06-05.png" alt="Bevel" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Input: `Radius` (bevel radius), `Normal` (surface normal hint)
- Output: `Normal` (approximated beveled normal)

#### Panel Option

- `Samples` (higher sample counts improve quality but cost more performance)

#### Purpose

Generates an approximate beveled normal in `Eevee` so hard edges can look smoother.

#### Notes

- `Cycles` still uses the original true geometric bevel algorithm

- `Eevee` here uses a same-object screen-space approximation

- The result depends on current view, depth buffer, and visible neighborhood, and is not equivalent to the true `Bevel` in `Cycles`
