# Blender 5.1 NPR Port 新增功能与使用说明

## 项目介绍

`Blender 5.1 NPR Port` 是一个 NPR 特化的 `Blender` 分支。在融合 `Goo Engine` 和 `4.4 NPR-prototype` 特色节点之外，还额外加入了一批面向 `Eevee` 的扩展节点、滤镜工作流和界面增强。

大部分功能是 `Eevee` 专用，不支持 `Cycles`。

## 文档范围

这份文档说明当前 `Blender 5.1 NPR Port` 相比官方 `Blender 5.1` 已经加入、并且当前分支内实际存在的 NPR / Eevee 扩展功能，以及它们的基本使用方法。

## 节点一览

<div align="center">
    <img src="images/SnowShot_2026-03-28_07-50-39.png" alt="Shader Nodes" style="border-radius: 10px;">
    <br>
    <sub>着色器节点</sub>
</div>

<div align="center">
    <img src="images/SnowShot_2026-03-28_04-23-33.png" alt="NPR Tree Nodes" style="border-radius: 10px;">
    <br>
    <sub>NPR Tree 节点（部分着色器节点也可在 NPR Tree 中使用）</sub>
</div>

<div align="center">
    <img src="images/SnowShot_2026-03-28_04-30-16.png" alt="Filter Nodes" style="border-radius: 10px;">
    <br>
    <sub>滤镜节点</sub>
</div>

<div class="grid cards" markdown>

- **Scene 级扩展**

    ---

    从场景级 Eevee 扩展开始了解核心功能。

    [查看 Scene 级扩展](scene-extensions.md)

- **扩展节点**

    ---

    深入查看新增节点的用途和接线方式。

    [查看扩展节点](extended-nodes.md)

- **NPR 工作流**

    ---

    了解 NPR Tree 的挂接方式和专用节点。

    [查看 NPR 工作流](npr-workflow.md)

- **界面与设置**

    ---

    查看额外的界面选项与工作流补充。

    [查看界面与设置](interface-guide.md)

</div>

## 主要功能分类

### 1. Scene 级 Eevee 扩展

- `Render Textures`
- `Filter Materials`
- `Eevee Outline`
- `Filter` 域下可用的 `AOV Input / AOV Output`

### 2. 着色器节点

- `Filter Object Info`
- `Filter Mask`
- `Scene Color`
- `Render Info`
- `Scene Time`
- `Screen Derivative`
- `Portal In / Portal Out`
- `Outline Control`
- `Screenspace Info`
- `World Environment`
- `Light Probe Color`
- `World To Tangent`
- `GLSL Function`
- `Image to Closure`
- `Basis Transform`
- `Twirl`
- `Water Ripples`
- `Hex Grid Texture`
- `SDF Primitive`
- `SDF Operator`
- `SDF Vector Operator`
- `Bevel`
- `Curvature`
- `Shader Info`
- `Light Info`
- `Color Ramp` 的 `OKLab` 模式

### 3. NPR Tree 工作流

- `NPR Input`
- `NPR Refraction`
- `Image Sample`
- `For Each Light`
- 内置节点组资产

### 4. 界面与设置

- `Eevee Performance` Outliner 视图
- 材质预览控制
- 材质剔除模式
- 灯光组管理
- 启动图版本标识
- 骨骼 Outliner 显示控制

!!! warning "Eevee 专用"
    所有 NPR Port 功能都需要 **Eevee 渲染引擎**。不支持 `Cycles`。
