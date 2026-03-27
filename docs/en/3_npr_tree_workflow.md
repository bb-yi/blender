# NPR Tree Workflow

## Overview

The NPR Tree is a specialized node tree domain for non-photorealistic rendering in Blender 5.1 NPR Port. It provides a flexible pipeline for implementing custom NPR effects and shading models.

## NPR Tree Basics

### What is NPR Tree?

NPR (Non-Photorealistic Rendering) Tree is a custom Blender shader node domain optimized for:
- Custom hatching and line rendering
- Stylized shading models
- Toon rendering workflows
- Custom lighting calculations
- Artistic effect compositing

### How to Use NPR Tree

**Step 1: Create NPR Tree**
- Open Shader Editor
- Create a new material
- In material properties, ensure it has a `Principled BSDF` or compatible shader

**Step 2: Attach NPR Processing**
- Create an `NPR Tree` node in your material
- Connect its inputs/outputs to drive NPR effects
- The NPR Tree processes light and material data

**Step 3: Keyboard Shortcuts**
- Press `Ctrl + Tab` to toggle between material and NPR tree editing
- This switches the node editor to show NPR-specific nodes

## NPR Input Node

**Location:** NPR Tree > Input

**Outputs:**
- `Base Color` - Material color
- `Metallic` - Metal property
- `Roughness` - Surface roughness
- `Normal` - Surface normal
- `Alpha` - Transparency
- `Emission` - Emissive color
- `IOR` - Index of refraction
- `Light Direction` - Direction to light source
- `Specular` - Specular intensity

**Usage:** Access material and lighting properties in NPR processing.

## NPR Refraction

**Purpose:** Control refraction effects within NPR rendering pipeline.

**Parameters:**
- `IOR` - Index of refraction
- `Roughness` - Refraction blur

## Image Sample

**Purpose:** Sample textures within NPR Tree.

**Inputs:**
- `Image` - Image to sample
- `UV` - Sample coordinates

**Outputs:**
- `Color` - Sampled color
- `Alpha` - Transparency

## For Each Light

**Purpose:** Process rendering for each light in the scene.

**Outputs:**
- `Light Index` - Current light ID
- `Light Direction` - Direction from surface to light
- `Light Distance` - Distance to light
- `Light Color` - Light color value
- `Light Energy` - Light intensity

**Usage:** Create per-light rendering passes for techniques like multi-light toon shading.

## Built-in Node Group Assets

The NPR Port includes pre-built node groups for common effects:

### Cavity
- Creates cavity shading based on geometry
- Good for surface detail emphasis

### Co-Planar Edge Detection
- Detects edges between co-planar surfaces
- Useful for outline/silhouette rendering

### Curvature-Based Shading
- Shades based on surface curvature
- Emphasizes edges and creases

### Kuwahara Filter
- Artistic edge-preserving smoothing
- Traditional art effect

### Shading Models
- Pre-built toon shading
- Realistic shading variants
- Stylized shading models

### Surface Curvature
- Calculate surface curvature
- Drive artistic effects with geometry data

## Workflow Example: Toon Shading

```
1. Create material with Principled BSDF
2. Add NPR Tree node
3. In NPR Tree Editor (Ctrl+Tab):
   - Use For Each Light to process each light
   - Sample light colors and directions
   - Use built-in Shading Models for toon effect
   - Combine results with Image Sample nodes
4. Output final shaded color to material
5. Render with Eevee
```

## Dithered Rendering

When using certain NPR Tree configurations, you may need to enable dithered rendering:

**Steps:**
1. Go to Render Properties
2. Enable `Dithered` option for improved quality with NPR effects
3. Adjust sample count as needed

## Tips & Troubleshooting

!!! tip "Best Practices"
    - Test NPR Tree effects in different lighting conditions
    - Use For Each Light for accurate per-light effects
    - Combine multiple Shading Models for complex looks

!!! warning "Performance"
    - For Each Light nodes have per-light cost
    - Limit effects on heavy geometry
    - Consider using baked lighting for static scenes

!!! example "Common Effects"
    - Toon shading with outlines
    - Hatching and line rendering
    - Cel shading with multiple tones
    - NPR with environment reflections

## FAQ

**Q: Can I use NPR Tree with Cycles?**  
A: No, NPR Tree is Eevee-exclusive.

**Q: How do I preview NPR effects in viewport?**  
A: Use rendered viewport shading mode (Ctrl+Z in viewport).

**Q: Can multiple NPR Trees be applied?**  
A: Each material has one NPR Tree. Combine techniques within one tree.

**Q: How do I iterate quickly?**  
A: Use Material Preview mode and adjust in real-time.
