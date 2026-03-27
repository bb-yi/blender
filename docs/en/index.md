# Blender 5.1 NPR Port - New Features & Usage Guide

## Project Introduction

`Blender 5.1 NPR Port` is an NPR-specialized Blender fork that combines features from `Goo Engine` and `4.4 NPR-prototype` with additional useful specialty nodes.

Most features are `Eevee` exclusive and do not support `Cycles`.

## Documentation Scope

This documentation covers NPR/Eevee extension features that have been added to `Blender 5.1 NPR Port` compared to official `Blender 5.1` and currently exist in this branch, as well as their basic usage methods.

## Main Feature Categories

### 1. Scene-Level Eevee Extensions
- **Render Textures** - Scene-level additional render texture system
- **Filter Materials** - Full-screen filter stack

### 2. Shader Nodes (20+ new nodes)
- Render Info, Scene Time, Screen Derivative
- Portal In/Out, Screenspace Info
- World Environment, Bevel, Curvature
- Shader Info, Light Info, Scene Color

### 3. NPR Tree Workflow
- NPR Input/Output nodes
- Per-light processing
- Built-in node group assets

### 4. Interface & Settings
- Material preview control
- World environment configuration
- Light group management

!!! warning "Eevee Only"
    All NPR Port features require the **Eevee render engine**. Cycles is not supported.

**Ready to explore?** Check out the sections below for more information!

---

## Quick Links

- [Scene-Level Extensions](scene-extensions.md) - Render Textures & Filter Materials
- [Extended Nodes](extended-nodes.md) - 20+ new shader nodes
- [NPR Workflow](npr-workflow.md) - NPR Tree processing pipeline
- [Interface Guide](interface-guide.md) - Settings & troubleshooting
