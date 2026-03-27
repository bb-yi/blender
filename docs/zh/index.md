# Blender 5.1 NPR Port - 新增功能与使用说明

## 项目介绍

`Blender 5.1 NPR Port` 是一个NPR特化的Blender分支，在融合了 `Goo Engine` 和 `4.4 NPR-prototype` 特色节点之外还额外添加一些实用特色节点。

大部分功能是 `Eevee` 专用，不支持 `Cycles`。

## 文档范围

这份文档说明当前 `Blender 5.1 NPR Port` 相比官方 `Blender 5.1` 已经加入、并且当前分支内实际存在的 NPR / Eevee 扩展功能，以及它们的基本使用方法。

## 主要功能分类

### 1. Scene 级 Eevee 扩展
- **Render Textures** - 场景级额外渲染纹理系统
- **Filter Materials** - 全屏滤镜栈

### 2. 着色器节点（20+ 个新节点）
- Render Info、Scene Time、Screen Derivative
- Portal In/Out、Screenspace Info
- World Environment、Bevel、Curvature
- Shader Info、Light Info、Scene Color

### 3. NPR Tree 工作流
- NPR Input/Output 节点
- 逐光源处理
- 内置节点组资产

### 4. 界面与设置
- 材质预览控制
- 世界环境配置
- 灯光组管理

!!! warning "Eevee 专用"
    所有 NPR Port 功能都需要 **Eevee 渲染引擎**。不支持 Cycles。

**准备好探索了吗？** 查看下面的章节了解更多！

---

## 快速链接

- [Scene 级扩展](scene-extensions.md) - Render Textures & Filter Materials
- [扩展节点](extended-nodes.md) - 20+ 个新着色器节点
- [NPR 工作流](npr-workflow.md) - NPR Tree 处理流程
- [界面指南](interface-guide.md) - 设置与故障排除
