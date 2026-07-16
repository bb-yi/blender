# Blender 5.2.0 LTS NPR Port - Features and Usage Guide

## Project Introduction

`Blender 5.2.0 LTS NPR Port` is an NPR-focused `Blender` branch. In addition to integrating characteristic nodes from `Goo Engine` and the `4.4 NPR-prototype`, it adds a set of Eevee-oriented extension nodes, a Filter Graph workflow, and interface improvements.

The release package includes `Cycles`, but the NPR extensions documented here are `Eevee`-only and cannot be used in `Cycles`.

## Documentation Scope

This document describes the NPR / Eevee extension features that have been added to the current `Blender 5.2.0 LTS NPR Port` branch compared with official `Blender 5.2.0 LTS`, together with their basic usage.

## 5.2.0 LTS Highlights

- Migrated to the official `Blender 5.2.0 LTS` code base and adapted the Eevee NPR render paths
- Replaced the legacy linear Filter Materials list with a scene-level `Filter Graph` supporting four execution stages and reusable Filter Pass materials
- Added a `Node / Code` editor to `GLSL Function`, with inline internal-Text editing and an atomic Apply / Discard workflow that protects the live material
- Added `mat2`, `mat3`, and `mat4` input, `out` parameter, and return-value support to `GLSL Function`, available in Eevee object materials, Filter materials, and `NPR Tree`
- Retained NPR extensions such as `GLSL Script Expression`, `Native Camera FX Outputs`, `Eevee Performance` shadow / probe attribution, and `OKLab Color Ramp`
- Fixed Eevee shader, transparent AOV, Scene Color offset-sampling, and node-interface compatibility regressions found during the 5.2 migration

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
    <img src="images/filter_graph_editor_overview.png" alt="Eevee Filter Graph" style="border-radius: 10px;">
    <br>
    <sub>Eevee Filter Graph</sub>
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
- [`Filter Graph`](scene-extensions.md#2-filter-graph)
- [`Eevee Outline`](scene-extensions.md#4-eevee-outline)
- [`Native Camera FX Outputs`](scene-extensions.md#3-native-camera-fx-outputs)
- [`Scene Color / AOV Input / Filter Pass / Stage Output`](scene-extensions.md#2-filter-graph)

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
    The release package can run `Cycles`, but the NPR Port extensions documented here require the **Eevee render engine** and cannot be used in `Cycles`.
