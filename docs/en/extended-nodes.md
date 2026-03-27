# Extended Shader Nodes

This page documents 20+ new shader nodes added to Blender 5.1 NPR Port.

## Render Info Node

**Purpose:** Access post-render image properties

**Outputs:**
- `Frag Coord` - Fragment/pixel coordinates (X, Y, Z, W)
- `Width` - Render target width (pixels)
- `Height` - Render target height (pixels)

**Usage:** Use in calculations that require screen resolution or pixel position.

---

## Scene Time Node

**Purpose:** Access time-based values for animation

**Outputs:**
- `Frame` - Current frame number
- `Seconds` - Elapsed time (seconds)
- `Timeline` - Timeline position
- `Scaled Frame` - Custom-scaled frame number

**Usage:** Create time-based animations and effects.

---

## Screen Derivative Node

**Purpose:** Detect edges and gradients in screen space

**Inputs:**
- `Value` - Input to differentiate

**Outputs:**
- `DDX` - X direction derivative
- `DDY` - Y direction derivative
- `DDXY` - Combined derivatives

**Usage:** Edge detection, gradient mapping, post-process effects.

---

## Light Info Node

**Purpose:** Access per-light information

**Inputs:**
- `Light Type` - Filter: All / Sun / Point / Spot

**Outputs:**
- `Direction` - Light direction
- `Distance` - Distance to light source
- `Color` - Light color
- `Energy` - Light intensity

**Usage:** Per-light calculations, conditional lighting effects.

---

## Scene Color Node

**Purpose:** Read scene data in filter materials

**Inputs:**
- `UV`/`Coordinate` - Sampling coordinates (default: screen space)

**Outputs:**
- `Color` - Scene color at that coordinate
- `Depth` - Scene depth value
- `Normal` - Scene normal data

**Parameters:**
- `Source` - Color / Depth / Normal / Emission / Environment

**Usage:** Post-process effects, reflections, depth-based effects.

---

## Additional Nodes

**Other important nodes:**

- **Portal In/Out** - Portal utility nodes
- **Screenspace Info** - View space information
- **World Environment** - Access environment
- **World To Tangent** - Coordinate transformation
- **Bevel** - Edge bevel processing
- **Curvature** - Output surface curvature
- **Shader Info** - Shader parameter access
- **Render Texture** - Sample pre-rendered texture

!!! tip
For detailed parameters and usage, refer to Blender's official shader node documentation combined with NPR Port extensions.
