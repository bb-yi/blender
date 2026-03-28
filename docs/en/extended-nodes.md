# Extended Shader Nodes

This page documents the major shader nodes added to Blender 5.1 NPR Port.

## Render Info Node

**Purpose:** Access render target properties.

**Outputs:**
- `Frag Coord` - Fragment/pixel coordinates (X, Y, Z, W)
- `Width` - Render target width in pixels
- `Height` - Render target height in pixels

**Usage:** Use in calculations that require screen resolution or pixel position.

---

## Scene Time Node

**Purpose:** Access time-based values for animation.

**Inputs:**
- `Scale` - A value used to scale frame numbers

**Outputs:**
- `Frame` - Current frame number
- `Seconds` - Elapsed time in seconds
- `Timeline` - Timeline position mapped to 0-1
- `Scaled Frame` - Frame number after scaling

**Usage:** Create time-based animations and effects.

---

## Screen Derivative Node

**Purpose:** Detect edges and gradients in screen space.

**Outputs:**
- `DDX` - X direction derivative
- `DDY` - Y direction derivative
- `DDXY` - Combined DDX + DDY

**Usage:** Edge detection, gradient mapping, and post-process effects.

---

## Portal In / Portal Out

**Purpose:** Organize node links with named portal values.

**Usage:**
- `Portal In` stores a named value in the current node tree.
- `Portal Out` retrieves that value later in the same tree.

**Limits:**
- Works only within the same shader node tree.
- Cannot cross node trees or node groups automatically.

---

## Render Texture Node

**Purpose:** Sample a render texture configured in scene settings.

**Inputs:**
- `Vector`

**Outputs:**
- `Color`
- `Alpha`

---

## Screenspace Info Node

**Purpose:** Access color and depth buffers from the current render.

**Inputs:**
- `View Position`

**Outputs:**
- `Scene Color`
- `Scene Depth`

---

## World Environment Node

**Purpose:** Sample Eevee world environment color.

**Inputs:**
- `Direction`

**Outputs:**
- `Color`

---

## World To Tangent Node

**Purpose:** Convert a world-space direction into tangent space.

**Inputs:**
- `Vector`

**Outputs:**
- `Vector`

---

## Basis Transform Node

**Purpose:** Transform points, vectors, or normals between custom bases.

**Inputs:**
- `Vector`
- `Origin`
- `X Axis`
- `Y Axis`
- `Z Axis`

**Outputs:**
- `Vector`

---

## Bevel Node

**Purpose:** Generate an approximate beveled normal in Eevee.

**Inputs:**
- `Radius`
- `Normal`

**Outputs:**
- `Normal`

---

## Curvature Node

**Purpose:** Provide curvature and rim outputs from screen-space data.

**Inputs:**
- `Samples`
- `Sample Radius`
- `Thickness`
- `Scale`

**Outputs:**
- `Scene Curvature`
- `Scene Rim`

---

## Shader Info Node

**Purpose:** Provide shading-related information such as diffuse, shadow, and ambient lighting.

**Inputs:**
- `World Position`
- `Normal`

**Outputs:**
- `Diffuse Shading`
- `Shadow`
- `Ambient Lighting`
- `Half-Lambert Factor`

---

## Light Info Node

**Purpose:** Access per-light information.

**Outputs:**
- `Color`
- `Power`
- `Type`
- `Position`
- `Direction`
- `Radius`
- `Spot Size`
- `Sun Angle`

**Usage:** Use `For Each Light` for per-light workflows.

---

## Scene Color Node

**Purpose:** Read scene data in filter materials.

**Inputs:**
- `UV` / `Coordinate`

**Outputs:**
- `Color`
- `Depth`
- `Normal`

**Parameters:**
- `Source` - Color / Depth / Normal / Emission / Environment

**Usage:** Post-process effects, reflections, depth-based effects.

---

## Additional Nodes

- `NPR Input`
- `NPR Refraction`
- `Image Sample`
- `For Each Light`

!!! tip
For detailed parameters and usage, refer to Blender's official shader node documentation together with the NPR Port extensions.
