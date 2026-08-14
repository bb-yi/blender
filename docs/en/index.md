---
hide:
  - navigation
  - toc
---

<div class="npr-hero" markdown>

<div class="npr-eyebrow">Blender 5.2.0 LTS · NPR Port · fd9fabb4f531</div>

# Stylized rendering extensions on Eevee

<p class="lead">
NPR capabilities are grouped into four paths you can start from immediately: Scene pipeline, extended nodes, NPR Tree, and UI controls.
The package includes Cycles; this site documents <strong>Eevee</strong> extensions only.
</p>

<div class="npr-actions">
<a class="npr-btn npr-btn--primary" data-npr-latest-win href="https://github.com/bb-yi/blender/releases/download/v5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604/blender-5.2.0-npr-port-win64-fd9fabb4f531-20260811-235604.zip">Download stable</a>
<a class="npr-btn npr-btn--outline" href="#modules">Modules</a>
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender">GitHub</a>
</div>

<div class="npr-meta">
<span class="npr-chip">Tests <strong>110/110 · Win</strong></span>
<span class="npr-chip">Date <strong>2026-08-11</strong></span>
<span class="npr-chip">Platforms <strong>Win · Linux · macOS</strong></span>
<span class="npr-chip">Engine <strong>Eevee</strong></span>
</div>

</div>

<hr class="npr-rule">

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">Map</div>

## Pick a working path
<p>Grouped by the Blender entry points you actually open — not a flat feature dump.</p>
</div>

<div class="npr-map">
<a href="scene-extensions.html">
<span class="code">01 · Scene</span>
<span class="title">Scene pipeline</span>
<span class="desc">Render textures, Filter Graph, camera outputs, outline</span>
</a>
<a href="extended-nodes.html">
<span class="code">02 · Nodes</span>
<span class="title">Extended nodes</span>
<span class="desc">Filter domain, helpers, GLSL, SDF, material nodes</span>
</a>
<a href="npr-workflow.html">
<span class="code">03 · NPR Tree</span>
<span class="title">NPR workflow</span>
<span class="desc">Lighting breakdown, refraction, image sample, per-light loops</span>
</a>
<a href="interface-guide.html">
<span class="code">04 · UI</span>
<span class="title">UI controls</span>
<span class="desc">Performance attribution, material state, light groups, shadow scale</span>
</a>
</div>

<div id="modules" class="npr-section-head" markdown>
<div class="npr-eyebrow">Modules</div>

## Module notes
</div>

<div class="npr-modules" markdown>

<div class="npr-module" markdown>
<div class="npr-module__body" markdown>
<span class="code">01 · Scene</span>
### Scene pipeline
<p class="blurb">Build render textures and post graphs at the scene level, instead of forcing the whole chain into one object material.</p>

- **Render Textures**: scene-level custom render textures
- **Filter Graph**: four-stage graph with reusable Filter Pass materials
- **Camera FX Outputs**: native camera-effect output sources
- **Eevee Outline**: scene-level outline

<div class="npr-tags">
<span>Render Textures</span><span>Filter Graph</span><span>Camera FX</span><span>Outline</span>
</div>

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="scene-extensions.html">Read Scene docs</a>
</div>
</div>
<div class="npr-module__media">
<img src="images/filter_graph_effect_example.png" alt="Filter Graph example with viewport and Filter Material nodes">
<p class="cap">Filter Graph · viewport + Filter Material</p>
</div>
</div>

<div class="npr-module" markdown>
<div class="npr-module__body" markdown>
<span class="code">02 · Nodes</span>
### Extended shader nodes
<p class="blurb">Nodes for Filter work, screenspace data, object materials, and custom GLSL — from sampling to procedural shaping.</p>

- **Filter domain**: Object Info, Mask, Scene Color
- **General helpers**: Render Info, Scene Time, Screen Derivative, Portal
- **Object materials**: Outline Control, Screenspace Info, World / Probe
- **GLSL / procedural**: Function, Script Expression, SDF, OKLab

<div class="npr-tags">
<span>Scene Color</span><span>GLSL Function</span><span>SDF</span><span>Portal</span>
</div>

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="extended-nodes.html">Read node docs</a>
</div>
</div>
<div class="npr-module__media">
<img src="images/overview_shader_nodes.png" alt="Extended shader nodes overview">
<p class="cap">Extended shader · node catalog</p>
</div>
</div>

<div class="npr-module" markdown>
<div class="npr-module__body" markdown>
<span class="code">03 · NPR Tree</span>
### NPR Tree workflow
<p class="blurb">Use a dedicated NPR Tree for lighting breakdown, refraction, sampling, and per-light work, plus built-in node-group assets.</p>

- **NPR Input**: combined / diffuse / specular lighting breakdown
- **NPR Refraction**: refraction path (including volume-approximation fixes)
- **Image Sample**: View / Pixel / UV offset sampling
- **For Each Light**: per-light loops and light info
- **Built-in assets**: drop-in NPR node groups

<div class="npr-tags">
<span>NPR Input</span><span>Refraction</span><span>Image Sample</span><span>Assets</span>
</div>

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="npr-workflow.html">Read NPR workflow</a>
</div>
</div>
<div class="npr-module__media">
<img src="images/overview_npr_tree_nodes.png" alt="NPR Tree dedicated nodes overview">
<p class="cap">NPR Tree · dedicated nodes</p>
</div>
</div>

<div class="npr-module" markdown>
<div class="npr-module__body" markdown>
<span class="code">04 · UI</span>
### Interface & production controls
<p class="blurb">Performance attribution, material render state, and light controls live in everyday panels for stylized production debugging.</p>

- **Eevee Performance**: shadow / probe cost attribution
- **Material state**: preview, culling, ZTest / Stencil / Write
- **Lights**: Lightgroup ID, Sun Shadow Map Scale
- **Production helpers**: splash version tag, IME, pose-bone Outliner visibility

<div class="npr-tags">
<span>Performance</span><span>Stencil</span><span>Lightgroup</span><span>Shadow Scale</span>
</div>

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="interface-guide.html">Read interface docs</a>
</div>
</div>
<div class="npr-module__media">
<img src="images/eevee_performance_shadow_probe.png" alt="Eevee Performance shadow and probe attribution panel">
<p class="cap">Eevee Performance · Shadow / Probe</p>
</div>
</div>

</div>


<div id="node-catalog" class="npr-section-head" markdown>
<div class="npr-eyebrow">Node catalog</div>

## New nodes at a glance
<p>Relative to stock Blender, this Port adds nodes across shader trees, NPR Tree, and the Filter domain. Full parameters live on the linked pages.</p>
</div>

<div class="npr-node-catalog" markdown>

### Extended shader nodes
<div align="center" markdown>
<img src="images/overview_shader_nodes.png" alt="Extended shader nodes overview" style="border-radius: 10px;">
</div>

- Render Info / Screen Derivative / Portal In·Out  
- Outline Control / Render Texture / Screenspace Info  
- World Environment / Light Probe Color / World To Tangent  
- GLSL Function / Script Expression / Image to Closure  
- Basis Transform / Twirl / Water Ripples / Hex Grid / Parallax  
- SDF Primitive·Operator·Vector Op / Bevel / Curvature  
- Shader Info / Light Info / Light Shader Info·Output / OKLab Color Ramp  

[Extended nodes →](extended-nodes.md)

### NPR Tree nodes
<div align="center" markdown>
<img src="images/overview_npr_tree_nodes.png" alt="NPR Tree nodes overview" style="border-radius: 10px;">
</div>

- NPR Input / NPR Output / NPR Refraction  
- Image Sample (View / Pixel / UV)  
- For Each Light (paired Input + Output zone)  
- Built-in groups: Cavity / Co-Planar Edge / Curvature / Kuwahara / Shading Models / Surface Curvature  

[NPR workflow →](npr-workflow.md)

### Filter-domain nodes
<div align="center" markdown>
<img src="images/overview_filter_domain_nodes.png" alt="Filter-domain nodes overview" style="border-radius: 10px;">
</div>

- Pass Input / Image Sample / Filter Output (default Filter Pass material chain)  
- Filter Object Info / Filter Mask / Scene Color  
- Graph-level: Scene Color · AOV Input · Filter Pass · Stage Output (see Scene docs)  

[Scene / Filter Graph →](scene-extensions.md) · [Filter-domain nodes →](extended-nodes.md#4-filter-domain-nodes)

</div>

<div class="npr-section-head" markdown>
<div class="npr-eyebrow">This build</div>

## Build notes · fd9fabb4f531
<p>This stable package focuses on refraction volume, GLSL matrix helpers, shadow detail, and input comfort.</p>
</div>

<div class="npr-delta" markdown>

<div markdown>
### New
- EEVEE NPR refraction volume approximation, with sampling/capture isolation and background-leak fixes
- `GLSL Function` draw-view matrix helpers (view / projection / model and inverses; see node docs)
- Viewport Shadow LOD visualization
- Improved IME support (5.2 API adaptation and text-editor cursor metrics)
</div>

<div markdown>
### Fixed
- Refraction-volume Image Sample offset black holes, background misses, depth mapping
- Sun `Shadow Map Scale` correctly applied to tilemaps
- White fallback for unconnected GLSL samplers
- Restored NPR Tree AOV output
- Stabilized Render Texture resource lifetime
</div>

</div>

<div class="npr-actions">
<a class="npr-btn npr-btn--outline" href="release.html">Download & integrity info</a>
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md">Full changelog</a>
</div>

!!! warning "Eevee only"
    The package can run Cycles, but the NPR Port extensions documented here require **Eevee**.

<div class="npr-cta-band" markdown>
<div class="npr-eyebrow">Release</div>

## Windows x64 stable package

<p>Windows is the fully validated baseline (110/110). Linux / macOS companion builds ship in the same release with SHA256.</p>

<div class="npr-actions">
<a class="npr-btn npr-btn--primary" href="release.html">Open download page</a>
<a class="npr-btn npr-btn--outline" href="https://github.com/bb-yi/blender/releases">All releases</a>
</div>
</div>
