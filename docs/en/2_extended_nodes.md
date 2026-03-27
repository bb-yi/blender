# Extended Nodes Reference

This page documents 20+ new shader nodes added to Blender 5.1 NPR Port.

## Eevee Utility Nodes

### Render Info
**Outputs:**
- `Frag Coord` - Fragment/pixel coordinates (X, Y, Z, W)
- `Width` - Render target width in pixels
- `Height` - Render target height in pixels

**Usage:** Access rendered image properties in shaders.

### Scene Time
**Outputs:**
- `Frame` - Current frame number
- `Seconds` - Elapsed time in seconds
- `Timeline` - Timeline position
- `Scaled Frame` - Frame with custom scale

**Usage:** Create time-based animations and effects.

### Screen Derivative
**Inputs:**
- `Value` - Input to differentiate

**Outputs:**
- `DDX` - Derivative in X direction
- `DDY` - Derivative in Y direction
- `DDXY` - Both derivatives

**Usage:** Detect edges and gradients in screen space.

### Portal In / Out
**Purpose:** Portal utility nodes for advanced rendering techniques.

### Screenspace Info
**Inputs:**
- `View Position` - Custom view position

**Outputs:**
- `Scene Color` - Current scene color
- `Scene Depth` - Linear depth value

**Usage:** Access screen-space scene data.

### World Environment
**Purpose:** Access world environment properties in material shaders.

### World To Tangent
**Purpose:** Transform coordinates from world space to tangent space.

## Object Material Nodes

### Bevel
**Purpose:** Add beveled edges to surfaces.

**Parameters:**
- `Radius` - Bevel amount
- `Samples` - Quality of bevel

### Curvature
**Outputs:**
- `Curvature` - Surface curvature value

**Usage:** Drive effects based on surface curvature (creases, edges, valleys).

### Shader Info
**Outputs:**
- `View Position` - Position relative to camera
- `Normal` - Surface normal
- Other shader parameters...

**Usage:** Access shader-related information.

### Light Info
**Inputs:**
- `Light Type` - Filter by light type:
  - `All` - All lights
  - `Sun` - Sun/directional lights
  - `Point` - Point lights
  - `Spot` - Spot lights

**Outputs:**
- `Direction` - Light direction
- `Distance` - Distance to light
- `Color` - Light color
- `Energy` - Light intensity

**Usage:** Per-light data access and calculations.

### Render Texture
**Inputs:**
- Select preset Render Texture from scene properties

**Outputs:**
- `Color` - RGB color data
- `Alpha` - Transparency value

**Usage:** Sample pre-rendered textures from other cameras/passes.

## Filter Domain Nodes

### Scene Color
**Inputs:**
- `UV` / `Coordinate` - Sampling coordinates (defaults to screen space)

**Outputs:**
- `Color` - Scene color at coordinates
- `Depth` - Scene depth value
- `Normal` - Scene normal data
- `ID Mask` / `AOV` - Additional render passes

**Parameters:**
- `Source` - Select data source:
  - `Color` - Final color
  - `Depth` - Linear depth
  - `Normal` - Surface normals
  - `Emission` - Emissive colors
  - `Environment` - Environment contribution

**Usage:** Read scene data in filter materials for post-processing effects.

### Filter Output
**Inputs:**
- `Color` - Output color
- `Alpha` - Output alpha

**Purpose:** Final output node for filter domain materials.

---

## Node Organization

All nodes are located in the Shader Editor under:
- **Eevee** category for most utility nodes
- **Texture** category for Render Texture
- **Light** category for Light Info
- **Output** category for Filter Output

## Tips & Best Practices

!!! tip "Performance"
    - Use Light Info with specific light types to reduce calculations
    - Cache expensive computations when possible
    - Be mindful of per-pixel costs in expensive effects

!!! warning "Compatibility"
    - Most nodes are Eevee-exclusive
    - Not compatible with Cycles
    - Some nodes only work in specific shader domains

!!! example "Common Workflows"
    - Use Screen Derivative for edge detection
    - Combine Curvature with color grading for stylized effects
    - Use Scene Color for realistic reflections in Filter Materials
