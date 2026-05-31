# Blender 5.1 NPR Port 新增功能与使用说明

## 项目介绍

`Blender 5.1 NPR Port` 是一个 NPR 特化的 `Blender` 分支。在融合 `Goo Engine` 和 `4.4 NPR-prototype` 特色节点之外，还额外加入了一批面向 `Eevee` 的扩展节点、滤镜工作流和界面增强。

大部分功能是 `Eevee` 专用，不支持 `Cycles`。

## 文档范围

这份文档说明当前 `Blender 5.1 NPR Port` 相比官方 `Blender 5.1` 已经加入、并且当前分支内实际存在的 NPR / Eevee 扩展功能，以及它们的基本使用方法。

## 5.1.2 更新重点

- 合并官方 `Blender 5.1.2` 修复与版本更新
- 新增 `GLSL Script Expression` 节点，可用单行 GLSL 表达式快速生成自定义标量、向量或颜色输出
- `GLSL Function` 的 `@glsl_meta v1` 新增 `label`，可为输入、输出和 `sampler2D` 插口设置节点界面显示名
- `Eevee Performance` 视图新增阴影和探针归因分组：`Shadow Contexts`、`Shadow Lights`、`Probe Costs`
- 新增 `Native Camera FX Outputs`，可在 `View Layer` 中把 Eevee 原生 `Motion Blur` 和 `Depth of Field` 应用到指定通道
- 恢复独立 `OKLab Color Ramp` 节点，普通 `Color Ramp` 保持原有 RGB / HSV / HSL 工作流
- 修复 `GLSL Function` 的 `vec4` 输入在刷新或编译路径中丢失 `w` 分量的问题
- 修复 `Scene Color` 的 `Position` 源在偏移采样时和其他场景缓冲不一致的问题
- 修复透明 / `Blended` Forward 层在未显式写入 AOV 时覆盖后方 AOV 的问题
- 更新 NPR Port 启动画面

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

- [`Render Textures`](scene-extensions.md#1-render-textures)
- [`Filter Materials`](scene-extensions.md#2-filter-materials)
- [`Eevee Outline`](scene-extensions.md#4-eevee-outline)
- [`Native Camera FX Outputs`](scene-extensions.md#3-native-camera-fx-outputs)
- [`Filter` 域下可用的 `AOV Input / AOV Output`](scene-extensions.md#2-filter-materials)

### 2. 着色器节点

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

### 3. NPR Tree 工作流

- [`NPR Input`](npr-workflow.md#npr-input)
- [`NPR Refraction`](npr-workflow.md#npr-refraction)
- [`Image Sample`](npr-workflow.md#image-sample)
- [`For Each Light`](npr-workflow.md#for-each-light)
- [内置节点组资产](npr-workflow.md#5-npr)

### 4. 界面与设置

- [`Eevee Performance` Outliner 视图](interface-guide.md#1-eevee-performance)
- [`Eevee Performance` 的阴影 / 探针成本归因](interface-guide.md#1-eevee-performance)
- [材质预览控制](interface-guide.md#2)
- [材质剔除模式](interface-guide.md#3)
- [材质 `ZTest / Stencil / Color Write / Depth Write`](interface-guide.md#4-surface)
- [灯光组管理](interface-guide.md#5-eevee-lightgroup-id)
- [太阳光 `Shadow Map Scale`](interface-guide.md#6-shadow-map-scale)
- [启动图版本标识](interface-guide.md#7)
- [骨骼 Outliner 显示控制](interface-guide.md#8-outliner)

!!! warning "Eevee 专用"
    所有 NPR Port 功能都需要 **Eevee 渲染引擎**。不支持 `Cycles`。
