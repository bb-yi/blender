# Extended Shader Nodes

## 1. Filter-Domain Nodes

### Filter Object Info

#### Entry

`Add > Input > Filter Object Info`

Available only in the `Filter` domain.

<div align="center">
	<img src="images/filter_object_info.png" alt="Filter Object Info" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Reads the world-space transform and viewport display color of a chosen object, making it easier to drive full-screen filters from scene helpers or controller objects.

#### Node Setting

- `Object`

#### Outputs

- `Location`
- `Rotation`
- `Scale`
- `Color`

#### Notes

- `Location`: world-space location of the chosen object
- `Rotation`: world-space Euler rotation in radians
- `Scale`: world-space scale of the chosen object
- `Color`: viewport display color of the chosen object
- If no object is assigned, the node falls back to `0` for location / rotation / color and `1` for scale

### Filter Mask

#### Entry

`Add > Input > Filter Mask`

Available only in the `Filter` domain.

<div align="center">
	<img src="images/placeholder_filter_mask.png" alt="Filter Mask" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Uses Eevee `Cryptomatte` object information to build fast object masks for filter materials.

#### Output

- `Mask`

#### Panel Options

- `Mode`
  - `Single Object`
  - `Object List`
  - `Collection`

#### Notes

- `Single Object` is useful for one controller object
- `Object List` is useful when a manual object set is needed, and can be filled from the current selection
- `Collection` is useful when the mask should follow a collection hierarchy
- The output is a `0-1` float mask that can be used with `Mix`, thresholds, AOV writing, or any other filter logic
- Only renderable geometry objects are supported

### Scene Color

#### Entry

`Add > Input > Scene Color`

Available only in the `Filter` domain.

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-15-31.png" alt="Scene Color" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Reads the current Eevee scene buffer. The `Source` can be switched in the node panel:

- `Color`
- `Depth`
- `Normal`
- `Position`

#### Inputs / Outputs

- Input: `Vector`
- Outputs: `Color`, `Alpha`

#### Notes

- `Color`: read the resolved scene color
- `Depth`: read linear scene depth
- `Normal`: read scene normals
- `Position`: read the world-space position pass
- If `Vector` is not connected, the node samples with the `Window` output of the `Texture Coordinate` node

## 2. General Eevee Utility Nodes

### Render Info

#### Entry

`Add > Input > Render Info`

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-51-10.png" alt="Render Info" style="border-radius: 10px;">
	<br>
</div>

#### Outputs

- `Frag Coord`
- `Width`
- `Height`

#### Purpose

Provides the coordinate and pixel size of the current Eevee render window.

#### Notes

- `Frag Coord.xy` is normalized screen UV in `0-1`
- `Frag Coord.z` is the current fragment depth
- `Width` / `Height` are the current render-region pixel dimensions

### Scene Time

#### Entry

`Add > Input > Scene Time`

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-52-48.png" alt="Scene Time" style="border-radius: 10px;">
	<br>
</div>

#### Input

- `Scale`

#### Outputs

- `Frame`
- `Seconds`
- `Timeline`
- `Scaled Frame`

#### Purpose

Provides time-related values from the current scene.

### Screen Derivative

#### Entry

`Add > Utilities > Math > Screen Derivative`

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-53-23.png" alt="Screen Derivative" style="border-radius: 10px;">
	<br>
</div>

#### Feature

Gets the difference between neighboring pixels in screen space:

- `DDX`
- `DDY`
- `DDXY`

where `DDXY` means `DDX + DDY`.

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

#### Notes

- `Portal In`: stores a named, typed value in the current node tree
- `Portal Out`: retrieves that value later by name inside the same node tree
- Creating a new `Portal In` generates a unique name automatically
- `Portal Out` includes a magnifier button that jumps to the matching `Portal In`
- Portals are only recognized inside the same shader node tree and do not automatically pass through node groups

## 3. Eevee Object Material Nodes

### Outline Control

#### Entry

`Add > Output > Outline Control`

Available in `Eevee` object materials and `NPR Tree`.

#### Inputs

- `Line Color`
- `Line Alpha`
- `Line Width`
- `Depth Threshold`
- `Normal Threshold`
- `Outline ID`

#### Purpose

Writes outline parameters for Eevee's built-in screen-space outline pass.

#### Basic Workflow

1. Add `Outline Control` to a material that should contribute outlines.
2. Use `Line Color`, `Line Alpha`, and `Line Width` to control the visible stroke.
3. Use `Depth Threshold` and `Normal Threshold` to tune silhouette edges versus internal edge detection.
4. Keep `Render Properties > Outline` enabled globally.
5. Enable `View Layer Properties > Passes > Data > Outline` when the outline result should be available as a separate pass.

#### Notes

- This is an auxiliary output node; it does not replace `Material Output`
- `Line Alpha` is multiplied with `Line Color.a`, and together they determine final opacity
- If `Line Width <= 0` or the final alpha resolves to `0`, nothing is written
- `Outline ID = 0` uses automatic grouping from the object resource ID
- `Outline ID > 0` can be used to force multiple objects or surfaces into the same outline group
- `Depth Threshold` is more related to depth discontinuity edges, while `Normal Threshold` is more related to internal edges from normal variation

#### Suggested Images

- `images/placeholder_outline_control.png`
  - Suggested content: the `Outline Control` node with a representative parameter setup

### Render Texture

#### Entry

`Add > Texture > Render Texture`

<div align="center">
	<img src="images/SnowShot_2026-03-28_04-54-27.png" alt="Render Texture" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Reads a `Render Textures` entry configured in the scene.

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

- Input: `View Position`
- Outputs: `Scene Color`, `Scene Depth`

#### Purpose

Reads color or depth from the current render buffer.

#### Usage Notes

- `Raytracing` must be enabled in render settings
- Material `Render Method` must be set to `Dithered`
- `Raytraced Transmission` must be enabled
- The default `View Position` input transforms the current position into camera space and then flips the `Z` axis

### World Environment

#### Entry

`Add > Input > World Environment`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-01-56.png" alt="World Environment" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Input: `Direction`
- Output: `Color`

#### Purpose

Directly samples the `Eevee` world environment color without depending on whether geometry exists behind the screen.

#### Notes

- If `Direction` is not connected, the current surface view direction is used
- If `Direction` is connected, the world environment is sampled in that direction
- The result is closer to Eevee environment / probe lighting than to a screen-space buffer

### Light Probe Color

#### Entry

`Add > Input > Light Probe Color`

#### Inputs / Outputs

- Input: `Direction`
- Outputs: `Reflection`, `Irradiance`, `Combined`

#### Purpose

Reads the currently available Eevee lighting-probe result directly, splitting it into reflection, irradiance, and combined outputs.

### World To Tangent

#### Entry

`Add > Utilities > Vector > World To Tangent`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-04-02.png" alt="World To Tangent" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Input: `Vector`
- Output: `Vector`

#### Purpose

Converts a world-space direction vector into the tangent space of the current surface.

#### Notes

- A `UV Map` can be chosen in the node panel, and the tangent of that UV is used as the basis
- This is useful for anisotropic direction control, tangent-space flow, and local scan-direction effects

### GLSL Function

#### Entry

`Add > Script > GLSL Function`

Available in both `Eevee` object materials and `NPR Tree`.

<div align="center">
	<img src="images/placeholder_glsl_function.png" alt="GLSL Function" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Injects a user-authored GLSL function into the current `Eevee / NPR` material compile path. This is useful for custom math nodes, procedural textures, SDF logic, screen-space effects, and porting parts of external GLSL / HLSL code.

#### Basic Workflow

1. Prepare a GLSL function in the `Text Editor` or point the node to an external `.glsl` file.
2. Add a `GLSL Function` node.
3. Choose the source and the target function in the node panel.
4. Refresh the node after editing the source.
5. Explicitly select the exported function name in `Function`.

#### Example Project

- GLSL Function example `.blend` project:
  [Google Drive](https://drive.google.com/file/d/1dHtj8ZHMT9s2rPHAzfXm7gE7SbQqj-GT/view?usp=sharing)
- This file is intended as a ready-made reference scene for the current `GLSL Function` workflow and node setup.

#### Supported Boundary Types

- Input parameters: `float`, `int`, `bool`, `vec2`, `vec3`, `vec4`, `sampler2D`
- Output parameters: `out float`, `out int`, `out bool`, `out vec2`, `out vec3`, `out vec4`
- Return values: `void`, `float`, `int`, `bool`, `vec2`, `vec3`, `vec4`

#### Important Notes

- `Function` is not auto-selected and must be chosen explicitly
- `sampler2D` appears as a `Closure` input
- `sampler2D` can be connected to `Image to Closure` or a compatible `Closure Output`
- `Closure Output -> sampler2D` currently guarantees only the direct `texture(tex, uv)` path
- If the function depends on `textureLod`, `textureGrad`, `textureSize`, or `texelFetch`, prefer driving it with `Image to Closure`
- `@glsl_meta` supports `default`, `min`, `max`, `hide_value`, and `subtype`
- Supported `subtype` values for `float`: `none`, `unsigned`, `percentage`, `factor`, `mass`, `angle`, `time`, `time_absolute`, `distance`, `wavelength`
- Supported `subtype` values for `vec2 / vec3 / vec4`: `none`, `factor`, `percentage`, `translation`, `direction`, `velocity`, `acceleration`, `euler`, `xyz`
- `subtype=color` is additionally supported for `vec3` and `vec4`
- `@glsl_meta default=` also accepts expressions such as `glsl_position()`, `normalize(glsl_normal())`, or `glsl_ambient_lighting()`
- Expression defaults are recommended only for `float / vec2 / vec3 / vec4` inputs and should not directly reference other exported parameters
- Only `vec3 / vec4` inputs explicitly marked with `subtype=color` become color sockets
- `vec3 + subtype=color` enters GLSL as `rgb` with `alpha = 1.0`
- `vec4 + subtype=color` keeps full `rgba`
- Exported boundary types do not currently support `mat*`, `struct`, or `array`
- `int / bool` boundaries are useful for mode switches, enums, and `lightgroup_id` style parameters
- Built-in geometry helpers are available in the function body: `glsl_position()`, `glsl_normal()`, `glsl_true_normal()`, `glsl_incoming()`
- Built-in ambient-light helper: `glsl_ambient_lighting()`
- Built-in direct-light helpers: `GLSLLight`, `glsl_light_count()`, `glsl_light_get(light_index)`, `glsl_light_shadow(light_index, shading_normal)`
- `GLSLLight.lightgroup_id` maps directly to the light data panel `Lightgroup ID`
- `GLSLLight.attenuation` is a base attenuation term for custom per-light models; it does not include `NdotL`, toon ramps, Blinn-Phong, GGX, shadows, or material-side Fresnel / metallic / roughness behavior
- Recommended diffuse pattern: `light.diffuse_color * light.attenuation * max(dot(N, light.vector), 0.0) * glsl_light_shadow(...)`
- Recommended specular pattern: `light.specular_color * light.attenuation * custom_spec_term * glsl_light_shadow(...)`

#### Example: `mode` Debug Mapping

If you want one `GLSL Function` node to switch between helper outputs with a single `mode` parameter, this mapping is the current reference:

| `mode` | Helper / Field |
| --- | --- |
| `0` | `glsl_position()` |
| `1` | `glsl_normal()` |
| `2` | `glsl_true_normal()` |
| `3` | `glsl_incoming()` |
| `4` | `glsl_ambient_lighting()` |
| `5` | `glsl_light_count()` |
| `6` | `light.valid` |
| `7` | `light.type` |
| `8` | `light.lightgroup_id` |
| `9` | `light.vector` |
| `10` | `light.position` |
| `11` | `light.direction` |
| `12` | `light.distance` |
| `13` | `light.diffuse_color` |
| `14` | `light.specular_color` |
| `15` | `light.attenuation` |
| `16` | `glsl_light_shadow(i, N)` |

#### Example: Filter by `lightgroup_id`

You can filter Eevee direct lights inside `GLSL Function` by reading `GLSLLight.lightgroup_id`.

```glsl
vec4 lightgroup_lambert(vec3 albedo, int target_lightgroup_id)
{
  vec3 N = normalize(glsl_normal());
  vec3 result = vec3(0.0);

  for (int i = 0; i < glsl_light_count(); i++) {
    GLSLLight light = glsl_light_get(i);
    if (!light.valid || light.lightgroup_id != target_lightgroup_id) {
      continue;
    }

    float NdotL = max(dot(N, light.vector), 0.0);
    if (NdotL <= 0.0) {
      continue;
    }

    float shadow = glsl_light_shadow(i, N);
    result += albedo * light.diffuse_color * light.attenuation * NdotL * shadow;
  }

  return vec4(result, 1.0);
}
```

#### Example: PBR-Style Direct Light + Ambient Light

This example shows how the current `GLSL Function` workflow can combine:

- Geometry helpers: `glsl_normal()`, `glsl_true_normal()`, `glsl_incoming()`
- Ambient-light helper: `glsl_ambient_lighting()`
- Per-light helpers: `glsl_light_count()`, `glsl_light_get(i)`, `glsl_light_shadow(i, N)`

Function name suggestion: `pbr_lit`

Recommended inputs:

- `base_color`
- `roughness`
- `metallic`
- `ao`

```glsl
float saturate1(float x)
{
  return clamp(x, 0.0, 1.0);
}

float pow5(float x)
{
  float x2 = x * x;
  return x2 * x2 * x;
}

vec3 fresnel_schlick(float cos_theta, vec3 F0)
{
  return F0 + (vec3(1.0) - F0) * pow5(1.0 - saturate1(cos_theta));
}

float distribution_ggx(float NdotH, float roughness)
{
  float a = roughness * roughness;
  float a2 = a * a;
  float nh2 = NdotH * NdotH;
  float denom = nh2 * (a2 - 1.0) + 1.0;
  return a2 / max(3.14159265 * denom * denom, 1e-6);
}

float geometry_schlick_ggx(float NdotV, float roughness)
{
  float r = roughness + 1.0;
  float k = (r * r) / 8.0;
  return NdotV / max(NdotV * (1.0 - k) + k, 1e-6);
}

float geometry_smith(float NdotV, float NdotL, float roughness)
{
  return geometry_schlick_ggx(NdotV, roughness) *
         geometry_schlick_ggx(NdotL, roughness);
}

vec4 pbr_lit(vec3 base_color, float roughness, float metallic, float ao)
{
  vec3 N = normalize(glsl_normal());
  vec3 Ng = normalize(glsl_true_normal());
  vec3 V = normalize(glsl_incoming());

  if (dot(N, Ng) < 0.0) {
    N = Ng;
  }

  roughness = clamp(roughness, 0.04, 1.0);
  metallic = clamp(metallic, 0.0, 1.0);
  ao = clamp(ao, 0.0, 1.0);

  vec3 F0 = mix(vec3(0.04), base_color, metallic);

  vec3 direct_diffuse = vec3(0.0);
  vec3 direct_specular = vec3(0.0);

  for (int i = 0; i < glsl_light_count(); i++) {
    GLSLLight light = glsl_light_get(i);
    vec3 L = normalize(light.vector);
    vec3 H = normalize(V + L);

    float NdotL = saturate1(dot(N, L));
    float NdotV = saturate1(dot(N, V));
    float NdotH = saturate1(dot(N, H));
    float VdotH = saturate1(dot(V, H));

    if (NdotL <= 1e-5 || NdotV <= 1e-5) {
      continue;
    }

    float shadow = glsl_light_shadow(i, N);

    vec3 F = fresnel_schlick(VdotH, F0);
    float D = distribution_ggx(NdotH, roughness);
    float G = geometry_smith(NdotV, NdotL, roughness);

    vec3 specular_brdf = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-5);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse_brdf = kd * base_color / 3.14159265;

    direct_diffuse += diffuse_brdf *
                      light.diffuse_color *
                      light.attenuation *
                      NdotL *
                      shadow;

    direct_specular += specular_brdf *
                       light.specular_color *
                       light.attenuation *
                       NdotL *
                       shadow;
  }

  vec3 ambient = glsl_ambient_lighting() * base_color * (1.0 - metallic) * ao;
  vec3 color = ambient + direct_diffuse + direct_specular;
  return vec4(max(color, vec3(0.0)), 1.0);
}
```

### Image to Closure

#### Entry

`Add > Texture > Image to Closure`

Available in both `Eevee` object materials and `NPR Tree`.

<div align="center">
	<img src="images/placeholder_image_to_closure.png" alt="Image to Closure" style="border-radius: 10px;">
	<br>
</div>

#### Output

- `Closure`

#### Purpose

Wraps a regular image as a closure-backed source for `sample2D` workflows.

#### Node Settings

- `Image`
- `Interpolation`
- `Extension`

#### Usage Notes

- This node does not expose a normal image socket; the image is selected directly in the node panel
- It is an adapter for `sampler2D` workflows, not a replacement for `Image Texture`
- Prefer it when a function needs image-resource specific sampling behavior

### Basis Transform

#### Entry

`Add > Utilities > Vector > Basis Transform`

<div align="center">
	<img src="images/SnowShot_2026-03-28_07-51-05.png" alt="Basis Transform" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

Transforms points, vectors, or normals to or from a custom basis defined by `Origin + axis inputs`.

#### Inputs / Outputs

- Inputs: `Vector`, `Origin`, `X Axis`, `Y Axis`, `Z Axis`
- Output: `Vector`

#### Panel Options

- `Direction`
  - `To Basis`
  - `From Basis`
- `Vector Type`
  - `Point`
  - `Vector`
  - `Normal`
- `Basis Input`
  - `XY`
  - `XZ`
  - `YZ`
  - `XYZ`
- `Orthonormalize`
- `Fallback`

#### Notes

- `Point` mode uses `Origin` as the translation reference, while `Vector` and `Normal` only transform direction
- `Basis Input` can derive the missing axis from two supplied axes, or use explicit `XYZ`
- `Orthonormalize` helps stabilize imperfect or non-orthogonal input axes
- Useful for local basis projection, procedural texture orientation, anisotropic direction control, and custom normal-space conversion

### Twirl

#### Entry

`Add > Utilities > Vector > Twirl`

<div align="center">
	<img src="images/placeholder_twirl.png" alt="Twirl" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Inputs: `Vector`, `Center`, `Amount`
- Output: `Vector`

#### Purpose

Twists the input coordinate field around a chosen center. This is useful for Goo Engine style swirls, distorted UVs, and radial deformations.

### Water Ripples

#### Entry

`Add > Texture > Water Ripples`

<div align="center">
	<img src="images/placeholder_water_ripples.png" alt="Water Ripples" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Inputs: `Vector`, `Time`, `Scale`, `Intensity`, `Speed`, `Detail`, `Bias`
- Outputs: `Distorted Vector`, `Mask`

#### Panel Options

- `Mode`
  - `Drops`
  - `Ripples`
  - `Flow`
  - `Caustic`

#### Purpose

Generates procedural ripple distortion and a ripple mask.

### Hex Grid Texture

#### Entry

`Add > Texture > Hex Grid Texture`

<div align="center">
	<img src="images/placeholder_hex_grid_texture.png" alt="Hex Grid Texture" style="border-radius: 10px;">
	<br>
</div>

#### Inputs

- `Vector`
- `Scale`
- `Size`
- `Radius`
- `Roundness`

#### Outputs

- `Value`
- `Color`
- `Hex Coords`
- `Position`
- `Cell UV`
- `Cell ID`

#### Panel Options

- `Coordinate Mode`
  - `XY Position`
  - `Hex Position`
- `Value Mode`
  - `Hexagons`
  - `SDF Hexagons`
  - `Dots`
- `Direction`
  - `Horizontal`
  - `Vertical`
  - `Horizontal Tiled`
  - `Vertical Tiled`
- `Clamp`

#### Purpose

Generates a hex-grid texture that can be used for honeycomb patterns, cell partitioning, SDF masks, and hex-coordinate based lookups.

### SDF Primitive

#### Entry

`Add > Texture > SDF Primitive`

<div align="center">
	<img src="images/SnowShot_2026-03-31_03-34-28.png" alt="SDF Primitive" style="border-radius: 10px;">
	<br>
</div>

#### Output

- `Distance`

#### Purpose

Generates signed-distance-field base shapes directly inside material nodes.

### SDF Operator

#### Entry

`Add > Converter > SDF Operator`

<div align="center">
	<img src="images/SnowShot_2026-03-31_03-34-47.png" alt="SDF Operator" style="border-radius: 10px;">
	<br>
</div>

#### Output

- `Distance`

#### Purpose

Combines, trims, and reshapes one or two SDF distance fields.

### SDF Vector Operator

#### Entry

`Add > Utilities > Vector > SDF Vector Operator`

<div align="center">
	<img src="images/SnowShot_2026-04-01_02-15-32.png" alt="SDF Vector Operator" style="border-radius: 10px;">
	<br>
</div>

#### Outputs

- `Vector`
- `Position`
- `Value`

#### Purpose

Preprocesses coordinate, UV, or vector domains before they are fed into `SDF Primitive`.

## 4. Goo Engine / NPR-Oriented Input Nodes

### Bevel

#### Entry

`Add > Input > Bevel`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-06-05.png" alt="Bevel" style="border-radius: 10px;">
	<br>
</div>

#### Inputs / Outputs

- Inputs: `Radius`, `Normal`
- Output: `Normal`

#### Purpose

Generates an approximate beveled normal in `Eevee` so hard edges can look smoother.

### Curvature

#### Entry

`Add > Input > Curvature`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-06-57.png" alt="Curvature" style="border-radius: 10px;">
	<br>
</div>

#### Inputs

- `Samples`
- `Sample Radius`
- `Thickness`
- `Scale`

#### Outputs

- `Scene Curvature`
- `Scene Rim`

#### Panel Options

- `Local`
- `Sample Radius`
  - `Pixel`
  - `View`

#### Notes

- `Local` tries to keep the evaluation focused on the current object
- In `Pixel` mode, `Sample Radius` is interpreted in pixels and therefore changes with render resolution
- In `View` mode, `Sample Radius` is interpreted relative to the view, which helps keep rim width more consistent between viewport and final render
- This is still a screen-space node, so the result depends on camera view, resolution, and sampling radius

### Shader Info

#### Entry

`Add > Input > Shader Info`

<div align="center">
	<img src="images/placeholder_shader_info_blinn_phong.png" alt="Shader Info" style="border-radius: 10px;">
	<br>
</div>

#### Inputs

- `World Position`
- `Normal`
- `Exponent`

#### Outputs

- `Diffuse Shading`
- `Shadow`
- `Ambient Lighting`
- `Half-Lambert Factor`
- `Blinn-Phong Factor`

#### Meaning of Each Output

- `Diffuse Shading`: the summed Lambert diffuse term from valid lights, clamped to `0-1`
- `Shadow`: switchable shadow evaluation output
- `Ambient Lighting`: probe / environment indirect-light contribution
- `Half-Lambert Factor`: summed half-Lambert diffuse term, clamped to `0-1`
- `Blinn-Phong Factor`: averaged Blinn-Phong highlight factor weighted by each light's specular channel, clamped to `0-1`

#### Notes

- `Shadow Mode`
  - `Built-in`
  - `Soft Filtered`
- `Soft Filtered` samples a local neighborhood around the current surface to rebuild smoother gray penumbra from Eevee's dithered black/white shadow result
- `Blinn-Phong Factor` does not automatically include shadow; multiply it with `Shadow` when needed
- `Exponent` controls highlight sharpness and defaults to `16`
- The node panel includes a `Lightgroup` control
- Only lights with the same `Lightgroup ID` participate in this `Shader Info` node
- World-sun style interference is excluded from these direct-light outputs

### Light Info

#### Entry

`Add > Input > Light Info`

<div align="center">
	<img src="images/SnowShot_2026-03-28_05-13-13.png" alt="Light Info" style="border-radius: 10px;">
	<br>
</div>

#### Feature Description

Reads information from a selected light.

#### Fixed Outputs

- `Color`
- `Power`
- `Type`

`Type` is an integer socket with the following values:

- `-1`: no light assigned
- `0`: Point
- `1`: Sun
- `2`: Spot
- `3`: Area

#### Outputs That Appear by Light Type

- `Position`
- `Direction`
- `Radius`
- `Spot Size`
- `Sun Angle`

Current behavior depends on the selected light type:

- `Point`: `Position`, `Radius`
- `Sun`: `Direction`, `Sun Angle`
- `Spot`: `Position`, `Direction`, `Radius`, `Spot Size`
- `Area`: `Position`, `Direction`, `Radius`

#### Notes

- For per-light processing, use `For Each Light` in the `NPR Tree`

## 5. Built-In Node Enhancements

### Color Ramp (OKLab Mode)

#### Entry

`Add > Converter > Color Ramp`

<div align="center">
	<img src="images/placeholder_color_ramp_oklab.png" alt="Color Ramp OKLab" style="border-radius: 10px;">
	<br>
</div>

#### Purpose

`Color Ramp` now supports an `OKLab` blend mode, giving more stable and perceptually smoother color transitions.

#### Notes

- The old standalone `OKLab Color Ramp` node has been merged back into `Color Ramp`
- Existing node setups should now use the `OKLab` mode on `Color Ramp` directly
