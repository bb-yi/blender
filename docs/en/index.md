---
hide:
  - navigation
  - toc
---

<div class="npr-hero" markdown>

<div class="npr-kicker"><span class="dot"></span> Blender 5.2.0 LTS · NPR Port</div>

# A Blender branch built for stylized rendering

<p class="lead">
Combines strengths from Goo Engine and the 4.4 NPR-prototype, plus Eevee extension nodes, a Filter Graph workflow, and interface upgrades.
Release builds include Cycles, but the NPR extensions documented here are <strong>Eevee-only</strong>.
</p>

<div class="npr-actions">
<a class="npr-btn npr-btn--primary" href="release.html">⬇ Download stable</a>
<a class="npr-btn npr-btn--ghost" href="scene-extensions.html">Browse docs</a>
<a class="npr-btn npr-btn--ghost" href="https://github.com/bb-yi/blender">GitHub</a>
</div>

<div class="npr-meta">
<span class="npr-chip">Build <strong>fd9fabb4f531</strong></span>
<span class="npr-chip">Released <strong>2026-08-11</strong></span>
<span class="npr-chip">Release tests <strong>110/110</strong></span>
<span class="npr-chip">Platform <strong>Windows x64</strong></span>
</div>

</div>

## Highlights in 5.2.0 LTS

<div class="npr-highlights" markdown>

<div class="npr-highlight" markdown>
**Filter Graph workflow**
<p>Scene-level four-stage execution graph replacing the legacy linear Filter Materials list, with reusable Filter Pass materials.</p>
</div>

<div class="npr-highlight" markdown>
**GLSL Function upgrades**
<p>Node / Code editing modes, matrix inputs and return values, plus draw-view transform matrix helpers.</p>
</div>

<div class="npr-highlight" markdown>
**Refraction volume approx.**
<p>More stable refraction backgrounds with volumes and improved Image Sample offset behavior.</p>
</div>

<div class="npr-highlight" markdown>
**UI & performance**
<p>Keeps Eevee Performance shadow/probe attribution, material render-state controls, light groups, and more.</p>
</div>

</div>

## Node overview

<div class="npr-shots">
<figure class="npr-shot">
  <img src="images/SnowShot_2026-03-28_07-50-39.png" alt="Shader Nodes">
  <figcaption>Shader nodes</figcaption>
</figure>
<figure class="npr-shot">
  <img src="images/SnowShot_2026-03-28_04-23-33.png" alt="NPR Tree Nodes">
  <figcaption>NPR Tree nodes</figcaption>
</figure>
<figure class="npr-shot">
  <img src="images/filter_graph_editor_overview.png" alt="Eevee Filter Graph">
  <figcaption>Eevee Filter Graph</figcaption>
</figure>
</div>

## Start here

<div class="grid cards" markdown>

-   :material-graph:{ .lg .middle } **Scene extensions**

    ---

    Render Textures, Filter Graph, Outline, Camera FX.

    [:octicons-arrow-right-24: Open scene extensions](scene-extensions.md)

-   :material-vector-polyline:{ .lg .middle } **Extended nodes**

    ---

    Scene Color, GLSL Function, SDF, OKLab, and more.

    [:octicons-arrow-right-24: Open extended nodes](extended-nodes.md)

-   :material-tree:{ .lg .middle } **NPR workflow**

    ---

    NPR Input / Refraction / Image Sample and built-in assets.

    [:octicons-arrow-right-24: Open NPR workflow](npr-workflow.md)

-   :material-tune-vertical:{ .lg .middle } **Interface & settings**

    ---

    Performance, material state, light groups, Shadow Map Scale.

    [:octicons-arrow-right-24: Open interface guide](interface-guide.md)

-   :material-download:{ .lg .middle } **Download & release**

    ---

    Stable package, SHA256 checks, notes, and previous builds.

    [:octicons-arrow-right-24: Open download page](release.md)

-   :material-book-open-page-variant:{ .lg .middle } **Documentation scope**

    ---

    Covers NPR / Eevee extensions that **actually exist** in this branch vs official 5.2.0 LTS.

    [:octicons-arrow-right-24: Full changelog on GitHub](https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md)

</div>

## Feature index

<div class="npr-feature-grid" markdown>

<div class="npr-feature-card" markdown>
### 1. Scene-level Eevee extensions
- [`Render Textures`](scene-extensions.md#1-render-textures)
- [`Filter Graph`](scene-extensions.md#2-filter-graph)
- [`Native Camera FX Outputs`](scene-extensions.md#3-native-camera-fx-outputs)
- [`Eevee Outline`](scene-extensions.md#4-eevee-outline)
- [Scene Color / AOV / Filter Pass](scene-extensions.md#2-filter-graph)
</div>

<div class="npr-feature-card" markdown>
### 2. Shader nodes
- [`GLSL Function`](extended-nodes.md#glsl-function) / [`GLSL Script Expression`](extended-nodes.md#glsl-script-expression)
- [`Scene Color`](extended-nodes.md#scene-color) · [`Filter Mask`](extended-nodes.md#filter-mask)
- [`Portal In/Out`](extended-nodes.md#portal-in-portal-out) · [`Outline Control`](extended-nodes.md#outline-control)
- [`SDF`](extended-nodes.md#sdf-primitive) · [`OKLab Color Ramp`](extended-nodes.md#oklab-color-ramp)
- [Full node list →](extended-nodes.md)
</div>

<div class="npr-feature-card" markdown>
### 3. NPR Tree workflow
- [`NPR Input`](npr-workflow.md#npr-input)
- [`NPR Refraction`](npr-workflow.md#npr-refraction)
- [`Image Sample`](npr-workflow.md#image-sample)
- [`For Each Light`](npr-workflow.md#for-each-light)
- [Built-in node-group assets](npr-workflow.md#5-built-in-npr-node-group-assets)
</div>

<div class="npr-feature-card" markdown>
### 4. Interface & settings
- [`Eevee Performance`](interface-guide.md#1-eevee-performance)
- [Material preview / culling / render state](interface-guide.md#2-material-selector-previews)
- [Light groups · Shadow Map Scale](interface-guide.md#5-eevee-lightgroup-id)
- [Splash version tag](interface-guide.md#7-splash-version-tag)
- [Pose bone Outliner visibility](interface-guide.md#9-pose-bone-outliner-visibility)
</div>

</div>

!!! warning "Eevee only"
    The release package can run Cycles, but the NPR Port extensions documented here require the **Eevee render engine** and cannot be used in Cycles.
