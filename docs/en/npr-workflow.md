# NPR Tree Workflow

## Overview

NPR Tree is a flexible, node-based non-photorealistic rendering pipeline. It supports custom line rendering, toon shading, and artistic effects.

### Key Features
- Per-light processing using `For Each Light`
- Real-time preview in viewport
- Built-in node groups for effects (shadows, toon, etc.)
- Complete shading control

---

## NPR Input Node

**Purpose:** Main input for NPR Tree processing

**Outputs (9 total):**
- `Base Color` - Material base color
- `Metallic` - Metallic property
- `Roughness` - Surface roughness
- `Normal` - Surface normal
- `Alpha` - Transparency
- `Emission` - Self-emission color
- `IOR` - Index of refraction
- `Light Direction` - Light source direction
- `Specular` - Specular reflection strength

---

## For Each Light Node

**Purpose:** Process rendering for each light source in the scene
# NPR Tree Workflow

## Overview

`NPR Tree` is a node tree attached after a regular material to perform color post-processing, allowing shader output to be stylized in color form.

### Key Features

- Per-light processing using `For Each Light`
- Real-time preview in viewport
- Built-in node groups for effects (shadows, toon, etc.)
- Complete shading control

---

## NPR Input Node

**Purpose:** Main input for NPR Tree processing

**Outputs (9 total):**
- `Base Color` - Material base color
- `Metallic` - Metallic property
- `Roughness` - Surface roughness
- `Normal` - Surface normal
- `Alpha` - Transparency
- `Emission` - Self-emission color
- `IOR` - Index of refraction
- `Light Direction` - Light source direction
- `Specular` - Specular reflection strength

---

## For Each Light Node

**Purpose:** Process rendering for each light source in the scene

**Outputs:**
- `Light Index` - Current light ID starting from 0
- `Light Direction` - Direction from surface to light source
- `Light Distance` - Distance to light source
- `Light Color` - Light color value
- `Light Energy` - Light intensity

**Usage Pattern:**
```
NPR Input → For Each Light → [Calculate per-light effects]
                                → Merge results → Output
```

**Common Uses:**
- Toon shading with multiple light tones
- Per-light cel shading
- Multi-pass lighting effects

---

## Other NPR Nodes

### Image Sample
- Sample textures within NPR Tree
- Input: Image, UV coordinates
- Output: Color, Alpha

### NPR Refraction
- Control refraction in the NPR pipeline
- Parameters: IOR, Roughness

### Built-in Node Groups
- `Cavity` - Shadow-based shading
- `Kuwahara Filter` - Artistic smoothing
- `Curvature Shading` - Edge-based effects
- `Shading Models` - Prebuilt toon/stylized shaders

---

## Quick Workflow Example

### Simple Toon Shading Setup

1. Create a material with `Principled BSDF`
2. Add an `NPR Tree` node
3. In the NPR Tree (Ctrl+Tab):
   - Connect `NPR Input` to `For Each Light`
   - Sample lighting properties
   - Apply toon shading from node groups
   - Merge all light contributions
4. Output the result to the material
5. Render in viewport or final output

**Tip:** Test with different light setups for best results.

!!! warning
NPR Tree is Eevee-exclusive. Not available in Cycles.
