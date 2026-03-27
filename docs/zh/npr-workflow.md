# 三、NPR Tree 工作流

## 概述

`NPR Tree` 是一个基于节点的非真实感渲染管道。它支持自定义的线条渲染、卡通着色和艺术效果。

### 主要特性
- 使用 `For Each Light` 进行逐光源处理
- 视口中的实时预览
- 内置节点组用于效果（阴影、卡通等）
- 完整的着色器控制

---

## NPR Input Node

**用途：** NPR Tree 处理的主要输入

**输出（共 9 个）：**
- `Base Color` - 材质基础颜色
- `Metallic` - 金属属性
- `Roughness` - 表面粗糙度
- `Normal` - 表面法线
- `Alpha` - 透明度
- `Emission` - 自发光颜色
- `IOR` - 折射率
- `Light Direction` - 光源方向
- `Specular` - 镜面反射强度

---

## For Each Light Node

**用途：** 为场景中的每个光源进行渲染处理

**输出：**
- `Light Index` - 当前灯光 ID（从 0 开始）
- `Light Direction` - 从表面指向光源的方向
- `Light Distance` - 到光源的距离
- `Light Color` - 光颜色值
- `Light Energy` - 光强度

**使用模式：**
```
NPR Input → For Each Light → [计算逐光源效果]
                                → 合并结果 → 输出
```

**常见用途：**
- 具有多种光调的卡通着色
- 逐光源单元着色
- 多通道照明效果

---

## Other NPR Nodes

### Image Sample（图像采样）
- 在 NPR Tree 中采样纹理
- 输入：Image（图像）、UV 坐标
- 输出：Color（颜色）、Alpha（透明度）

### NPR Refraction（NPR 折射）
- 在 NPR 管道中控制折射
- 参数：IOR、Roughness

### 内置节点组
- `Cavity` - 基于阴影的着色
- `Kuwahara Filter` - 艺术风格平滑
- `Curvature Shading` - 基于边缘的效果
- `Shading Models` - 预先构建的卡通 / 风格化着色器

---

## Quick Workflow Example

### 简单的卡通着色设置

1. 创建带有 `Principled BSDF` 的材质
2. 添加 `NPR Tree` 节点
3. 在 NPR Tree 中（Ctrl+Tab）：
   - 连接 `NPR Input` 到 `For Each Light`
   - 采样光照属性
   - 应用节点组中的卡通着色
   - 合并所有灯光贡献
4. 输出结果到材质
5. 在视口或最终渲染中渲染

**提示：** 用不同的灯光配置测试可获得最佳效果。

!!! warning
NPR Tree is Eevee-exclusive. Not available in Cycles.
