# Blender 5.1 NPR Port 新增功能与使用说明

## 项目介绍

`Blender 5.1 NPR Port` 是一个 NPR 特化的 `Blender` 分支，在融合了 `Goo Engine` 和 `4.4 NPR-prototype` 特色节点之外，还额外添加了一些实用特色节点。

大部分功能是 `Eevee` 专用，不支持 `Cycles`。

## 文档范围

这份文档说明当前 `Blender 5.1 NPR Port` 相比官方 `Blender 5.1` 已经加入、并且当前分支内实际存在的 NPR / Eevee 扩展功能，以及它们的基本使用方法。

## 节点一览

<div class="grid cards" markdown>

- :fontawesome-solid-rocket:{ .lg .middle } **Scene 级扩展**

    ---

    从场景级 Eevee 扩展开始了解基础功能

    [查看 Scene 级扩展 →](scene-extensions.md)

- :fontawesome-solid-cube:{ .lg .middle } **扩展节点**

    ---

    深入了解各个新增节点的功能

    [查看 扩展节点 →](extended-nodes.md)

- :fontawesome-solid-code-branch:{ .lg .middle } **NPR 工作流**

    ---

    学习 NPR Tree 的工作方式

    [查看 NPR Tree 工作流 →](npr-workflow.md)

- :fontawesome-solid-toolbox:{ .lg .middle } **界面与设置**

    ---

    查看更多界面和工作流选项

    [查看 界面与设置 →](interface-guide.md)

</div>

## 主要功能分类

### 1. Scene 级 Eevee 扩展

- `Render Textures`

- `Filter Materials`

### 2. 着色器节点（20+ 个新节点）

- `Render Info`

- `Scene Time`

- `Screen Derivative`

- `Portal In / Portal Out`

- `Screenspace Info`

- `World Environment`

- `World To Tangent`

- `Basis Transform`

- `Bevel`

- `Curvature`

- `Shader Info`

- `Light Info`

- `Scene Color`

### 3. NPR Tree 工作流

- `NPR Input`

- `NPR Refraction`

- `Image Sample`

- `For Each Light`

- 内置节点组资产

### 4. 界面与设置

- 材质预览控制

- 世界环境配置

- 灯光组管理

!!! warning "Eevee 专用"
    所有 NPR Port 功能都需要 **Eevee 渲染引擎**。不支持 Cycles。
