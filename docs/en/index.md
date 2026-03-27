# Blender 5.1 NPR Port - Features & Usage Guide

## Project Introduction

**Blender 5.1 NPR Port** is a specialized NPR (Non-Photorealistic Rendering) branch of Blender that combines features from the `Goo Engine` and `4.4 NPR-prototype`, plus additional utility nodes.

Most features are **Eevee-exclusive** and do not support Cycles.

## Documentation Scope

This documentation covers NPR/Eevee extensions that have been added to `Blender 5.1 NPR Port` compared to official `Blender 5.1`, along with their basic usage instructions.

## Node Overview

### Shader Nodes
![Shader Nodes](images/SnowShot_2026-03-28_04-33-09.png)

### NPR Tree Nodes
![NPR Tree Nodes](images/SnowShot_2026-03-28_04-23-33.png)
*Some shader nodes can also be used in NPR Trees*

### Filter Nodes
![Filter Nodes](images/SnowShot_2026-03-28_04-30-16.png)

## Main Feature Categories

### 1. Scene-Level Eevee Extensions

- **Render Textures** - Scene-level additional render texture system
- **Filter Materials** - Full-screen filter stack

### 2. Shader Nodes (20+ new nodes)

**Utility Nodes:**
- Render Info (fragment coordinates, screen dimensions)
- Scene Time (frame number, timeline info)
- Screen Derivative (DDX, DDY operations)
- Portal In/Out (portal utilities)
- Screenspace Info (view position, depth)
- World Environment (environment access)
- World To Tangent (coordinate transforms)
- Bevel (edge beveling)
- Curvature (surface curvature)
- Shader Info (shader parameters)
- Light Info (lighting information)
- Scene Color (scene color composition)

### 3. NPR Tree Workflow

- **NPR Input** - Main NPR tree input node
- **NPR Refraction** - Refraction control
- **Image Sample** - Texture sampling
- **For Each Light** - Per-light processing
- Built-in node groups (Cavity, Curvature, Kuwahara, etc.)

### 4. Interface & Settings

- Material preview settings
- World environment exclusion
- Lightgroup configuration
- Startup splash information

## Quick Navigation

<div align="center">
  <table style="width: 100%; text-align: center;">
    <tr>
      <td style="padding: 20px; border: 1px solid #ccc; border-radius: 8px;">
        <h3>🎨 Scene Extensions</h3>
        <p>Render Textures & Filter Materials</p>
      </td>
      <td style="padding: 20px; border: 1px solid #ccc; border-radius: 8px;">
        <h3>🔧 Extended Nodes</h3>
        <p>20+ new shader nodes</p>
      </td>
    </tr>
    <tr>
      <td style="padding: 20px; border: 1px solid #ccc; border-radius: 8px;">
        <h3>🌳 NPR Tree Workflow</h3>
        <p>Pipeline & node composition</p>
      </td>
      <td style="padding: 20px; border: 1px solid #ccc; border-radius: 8px;">
        <h3>⚙️ Interface Guide</h3>
        <p>Settings & troubleshooting</p>
      </td>
    </tr>
  </table>
</div>

## Important Notes

!!! warning "Eevee Only"
    All NPR Port features require **Eevee render engine**. Cycles is not supported.

!!! info "Version"
    Documentation for **Blender 5.1 NPR Port**
    Last updated: 2026-03-28

## Support & Credits

- Based on Goo Engine and 4.4 NPR-prototype
- Community-driven development
- For issues and questions, please check the troubleshooting section

---

**Ready to get started?** Choose a topic from the navigation menu to explore the full documentation.
