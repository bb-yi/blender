# Blender 5.1 NPR Port - Features and Usage Guide

## Project Introduction

`Blender 5.1 NPR Port` is an NPR-focused `Blender` branch. In addition to integrating characteristic nodes from `Goo Engine` and the `4.4 NPR-prototype`, it adds a set of Eevee-oriented extension nodes, filter workflows, and interface improvements.

Most features are `Eevee`-only and do not support `Cycles`.

## Documentation Scope

This document describes the NPR / Eevee extension features that have been added to the current `Blender 5.1 NPR Port` branch compared with official `Blender 5.1`, together with their basic usage.

## 5.1.2 Highlights

- Merged official `Blender 5.1.2` fixes and version updates
- Added `GLSL Script Expression` for quick custom scalar, vector, or color outputs from a single GLSL expression
- Added `label` support to `GLSL Function` `@glsl_meta v1`, allowing input, output, and `sampler2D` sockets to use custom UI display names
- Added shadow and probe attribution groups to the `Eevee Performance` view: `Shadow Contexts`, `Shadow Lights`, and `Probe Costs`
- Added `Native Camera FX Outputs`, allowing Eevee native `Motion Blur` and `Depth of Field` to be applied to selected View Layer channels
- Restored the standalone `OKLab Color Ramp` node; regular `Color Ramp` keeps the existing RGB / HSV / HSL workflow
- Fixed `GLSL Function` `vec4` inputs losing the `w` component during refresh or compile paths
- Fixed `Scene Color` `Position` source offset sampling so it matches the other scene-buffer sources
- Fixed transparent / `Blended` Forward layers overwriting behind-surface AOVs when they do not explicitly write an AOV
- Updated the NPR Port splash screen

## Node Overview

<div align="center">
    <img src="images/SnowShot_2026-03-28_07-50-39.png" alt="Shader Nodes" style="border-radius: 10px;">
    <br>
    <sub>Shader Nodes</sub>
</div>

<div align="center">
    <img src="images/SnowShot_2026-03-28_04-23-33.png" alt="NPR Tree Nodes" style="border-radius: 10px;">
    <br>
    <sub>NPR Tree Nodes (some shader nodes can also be used inside NPR Tree)</sub>
</div>

<div align="center">
    <img src="images/SnowShot_2026-03-28_04-30-16.png" alt="Filter Nodes" style="border-radius: 10px;">
    <br>
    <sub>Filter Nodes</sub>
</div>

<div class="grid cards" markdown>

- **Scene-Level Extensions**

    ---

    Start with the scene-level Eevee extensions to understand the core features.

    [View Scene-Level Extensions →](scene-extensions.md)

- **Extended Nodes**

    ---

    Dive into the functionality of each newly added node.

    [View Extended Nodes →](extended-nodes.md)

- **NPR Workflow**

    ---

    Learn how the NPR Tree workflow is organized.

    [View NPR Workflow →](npr-workflow.md)

- **Interface & Settings**

    ---

    Check additional interface options and workflow settings.

    [View Interface & Settings →](interface-guide.md)

</div>

## Main Feature Categories

### 1. Scene-Level Eevee Extensions

- [`Render Textures`](scene-extensions.md#1-render-textures)
- [`Filter Materials`](scene-extensions.md#2-filter-materials)
- [`Eevee Outline`](scene-extensions.md#4-eevee-outline)
- [`Native Camera FX Outputs`](scene-extensions.md#3-native-camera-fx-outputs)
- [`AOV Input / AOV Output` in the `Filter` domain](scene-extensions.md#2-filter-materials)

### 2. Shader Nodes

- [`Filter Object Info`](extended-nodes.md#filter-object-info)
- [`Filter Mask`](extended-nodes.md#filter-mask)
- [`Scene Color`](extended-nodes.md#scene-color)
- [`Render Info`](extended-nodes.md#render-info)
- [`Scene Time`](extended-nodes.md#scene-time)
- [`Screen Derivative`](extended-nodes.md#screen-derivative)
- [`Portal In / Portal Out`](extended-nodes.md#portal-in-portal-out)
- [`Outline Control`](extended-nodes.md#outline-control)
- [`Screenspace Info`](extended-nodes.md#screenspace-info)
- [`World Environment`](extended-nodes.md#world-environment)
- [`Light Probe Color`](extended-nodes.md#light-probe-color)
- [`World To Tangent`](extended-nodes.md#world-to-tangent)
- [`GLSL Function`](extended-nodes.md#glsl-function)
- [`GLSL Script Expression`](extended-nodes.md#glsl-script-expression)
- [`Image to Closure`](extended-nodes.md#image-to-closure)
- [`Light Shader Info`](extended-nodes.md#light-shader-info-light-shader-output)
- [`Light Shader Output`](extended-nodes.md#light-shader-info-light-shader-output)
- [`Basis Transform`](extended-nodes.md#basis-transform)
- [`Twirl`](extended-nodes.md#twirl)
- [`Water Ripples`](extended-nodes.md#water-ripples)
- [`Hex Grid Texture`](extended-nodes.md#hex-grid-texture)
- [`SDF Primitive`](extended-nodes.md#sdf-primitive)
- [`SDF Operator`](extended-nodes.md#sdf-operator)
- [`SDF Vector Operator`](extended-nodes.md#sdf-vector-operator)
- [`Bevel`](extended-nodes.md#bevel)
- [`Curvature`](extended-nodes.md#curvature)
- [`Shader Info`](extended-nodes.md#shader-info)
- [`Light Info`](extended-nodes.md#light-info)
- [`OKLab Color Ramp`](extended-nodes.md#oklab-color-ramp)

### 3. NPR Workflow

- [`NPR Input`](npr-workflow.md#npr-input)
- [`NPR Refraction`](npr-workflow.md#npr-refraction)
- [`Image Sample`](npr-workflow.md#image-sample)
- [`For Each Light`](npr-workflow.md#for-each-light)
- [Built-in node-group assets](npr-workflow.md#5-built-in-npr-node-group-assets)

### 4. Interface & Settings

- [`Eevee Performance` Outliner view](interface-guide.md#1-eevee-performance)
- [Shadow / probe cost attribution in `Eevee Performance`](interface-guide.md#shadow-and-probe-attribution)
- [Material preview control](interface-guide.md#2-material-selector-previews)
- [Material face culling](interface-guide.md#3-material-face-culling)
- [Material `ZTest / Stencil / Color Write / Depth Write`](interface-guide.md#4-material-surface-render-state)
- [Lightgroup management](interface-guide.md#5-eevee-lightgroup-id)
- [Sun `Shadow Map Scale`](interface-guide.md#6-sun-shadow-map-scale)
- [Splash version tag](interface-guide.md#7-splash-version-tag)
- [Pose bone Outliner visibility](interface-guide.md#8-pose-bone-outliner-visibility)

!!! warning "Eevee Only"
    All NPR Port features require the **Eevee render engine**. `Cycles` is not supported.
