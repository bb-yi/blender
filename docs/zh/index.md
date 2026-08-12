---
hide:
  - navigation
  - toc
---

<div class="npr-hero" markdown>

<div class="npr-kicker"><span class="dot"></span> Blender 5.2.0 LTS · NPR Port</div>

# 面向风格化渲染的 Blender 分支

<p class="lead">
融合 Goo Engine 与 4.4 NPR-prototype 的特色能力，并扩展 Eevee 节点、Filter Graph 工作流与界面增强。
正式包包含 Cycles，但本文档介绍的 NPR 扩展功能仅支持 <strong>Eevee</strong>。
</p>

<div class="npr-actions">
<a class="npr-btn npr-btn--primary" href="release.html">⬇ 下载正式版</a>
<a class="npr-btn npr-btn--ghost" href="scene-extensions.html">阅读功能文档</a>
<a class="npr-btn npr-btn--ghost" href="https://github.com/bb-yi/blender">GitHub</a>
</div>

<div class="npr-meta">
<span class="npr-chip">当前版本 <strong>fd9fabb4f531</strong></span>
<span class="npr-chip">发布日期 <strong>2026-08-11</strong></span>
<span class="npr-chip">Release 测试 <strong>110/110</strong></span>
<span class="npr-chip">平台 <strong>Windows x64</strong></span>
</div>

</div>

## 本版重点

<div class="npr-highlights" markdown>

<div class="npr-highlight" markdown>
**Filter Graph 工作流**
<p>场景级四阶段执行图，替代旧线性 Filter Materials，支持可复用 Filter Pass 材质。</p>
</div>

<div class="npr-highlight" markdown>
**GLSL Function 增强**
<p>Node / Code 双模式、矩阵输入与返回值、draw-view 变换矩阵 helper。</p>
</div>

<div class="npr-highlight" markdown>
**折射体积近似**
<p>改善体积场景中的折射背景合成，以及 Image Sample 偏移稳定性。</p>
</div>

<div class="npr-highlight" markdown>
**界面与性能**
<p>保留 Eevee Performance 阴影 / 探针归因、材质状态控制与灯光组等扩展。</p>
</div>

</div>

## 节点一览

<div class="npr-shots">
<figure class="npr-shot">
  <img src="images/SnowShot_2026-03-28_07-50-39.png" alt="Shader Nodes">
  <figcaption>着色器节点</figcaption>
</figure>
<figure class="npr-shot">
  <img src="images/SnowShot_2026-03-28_04-23-33.png" alt="NPR Tree Nodes">
  <figcaption>NPR Tree 节点</figcaption>
</figure>
<figure class="npr-shot">
  <img src="images/filter_graph_editor_overview.png" alt="Eevee Filter Graph">
  <figcaption>Eevee Filter Graph</figcaption>
</figure>
</div>

## 从这里开始

<div class="grid cards" markdown>

-   :material-graph:{ .lg .middle } **Scene 级扩展**

    ---

    Render Textures、Filter Graph、Outline、Camera FX。

    [:octicons-arrow-right-24: 查看 Scene 扩展](scene-extensions.md)

-   :material-vector-polyline:{ .lg .middle } **扩展着色器节点**

    ---

    Scene Color、GLSL Function、SDF、OKLab 等节点说明。

    [:octicons-arrow-right-24: 查看扩展节点](extended-nodes.md)

-   :material-tree:{ .lg .middle } **NPR Tree 工作流**

    ---

    NPR Input / Refraction / Image Sample 与内置资产。

    [:octicons-arrow-right-24: 查看 NPR 工作流](npr-workflow.md)

-   :material-tune-vertical:{ .lg .middle } **界面与设置**

    ---

    Performance、材质状态、灯光组、Shadow Map Scale。

    [:octicons-arrow-right-24: 查看界面设置](interface-guide.md)

-   :material-download:{ .lg .middle } **下载与版本**

    ---

    正式包、SHA256 校验、更新摘要与历史版本。

    [:octicons-arrow-right-24: 打开下载页](release.md)

-   :material-book-open-page-variant:{ .lg .middle } **文档范围**

    ---

    说明当前分支相对官方 5.2.0 LTS **已落地** 的 NPR / Eevee 扩展。

    [:octicons-arrow-right-24: GitHub 变更记录](https://github.com/bb-yi/blender/blob/main/blender-npr-release-changelog.md)

</div>

## 功能索引

<div class="npr-feature-grid" markdown>

<div class="npr-feature-card" markdown>
### 1. Scene 级 Eevee 扩展
- [`Render Textures`](scene-extensions.md#1-render-textures)
- [`Filter Graph`](scene-extensions.md#2-filter-graph)
- [`Native Camera FX Outputs`](scene-extensions.md#3-native-camera-fx-outputs)
- [`Eevee Outline`](scene-extensions.md#4-eevee-outline)
- [Scene Color / AOV / Filter Pass](scene-extensions.md#2-filter-graph)
</div>

<div class="npr-feature-card" markdown>
### 2. 着色器节点
- [`GLSL Function`](extended-nodes.md#glsl-function) / [`GLSL Script Expression`](extended-nodes.md#glsl-script-expression)
- [`Scene Color`](extended-nodes.md#scene-color) · [`Filter Mask`](extended-nodes.md#filter-mask)
- [`Portal In/Out`](extended-nodes.md#portal-in-portal-out) · [`Outline Control`](extended-nodes.md#outline-control)
- [`SDF`](extended-nodes.md#sdf-primitive) · [`OKLab Color Ramp`](extended-nodes.md#oklab-color-ramp)
- [完整节点列表 →](extended-nodes.md)
</div>

<div class="npr-feature-card" markdown>
### 3. NPR Tree 工作流
- [`NPR Input`](npr-workflow.md#npr-input)
- [`NPR Refraction`](npr-workflow.md#npr-refraction)
- [`Image Sample`](npr-workflow.md#image-sample)
- [`For Each Light`](npr-workflow.md#for-each-light)
- [内置节点组资产](npr-workflow.md#5-npr)
</div>

<div class="npr-feature-card" markdown>
### 4. 界面与设置
- [`Eevee Performance`](interface-guide.md#1-eevee-performance)
- [材质预览 / 剔除 / 渲染状态](interface-guide.md#2)
- [灯光组 · Shadow Map Scale](interface-guide.md#5-eevee-lightgroup-id)
- [启动图版本标识](interface-guide.md#7)
- [骨骼 Outliner 显示](interface-guide.md#9-outliner)
</div>

</div>

!!! warning "Eevee 专用"
    发布包可以运行 Cycles，但本文介绍的 NPR Port 扩展功能需要 **Eevee 渲染引擎**，不支持在 Cycles 中使用。
