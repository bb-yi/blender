# Blender 5.1 NPR Port 发布更新日志

这份文档汇总当前已经发布到 GitHub 的 `Blender 5.1 NPR Port` Release 说明，按发布时间从新到旧整理。

## 待发布草稿

### 当前版本相对 `v5.1.0-npr-port-win64-710a1b4934d6`

#### 新增功能
- `Shader Info` 节点新增 `Soft Filtered` 阴影模式。
- World 面板新增 `Environment Lighting > Exclude Collection`。
  - 可指定一个集合，让其中物体不接收 Eevee 世界环境和 light probe 的环境照明。

#### 修复与改进
- 修复 `Shader To RGB` 在 `NPR / deferred` 路径下的兼容问题，补齐 `closure_to_rgba` 缺失实现。
- 修复 GPU shader preprocessor 的若干稳定性问题。
- 调整部分 Eevee shader create info 配置，减少特定 shader 走预处理器路径时的不稳定问题。

## 发布列表

| 发布时间 | Tag | 构建哈希 |
| --- | --- | --- |
| 2026-03-25 | `v5.1.0-npr-port-win64-710a1b4934d6` | `710a1b4934d6` |
| 2026-03-23 | `v5.1.0-npr-port-win64-66ed2fb9cad6` | `66ed2fb9cad6` |
| 2026-03-22 | `v5.1.0-npr-port-win64-19c826ccb5ee` | `19c826ccb5ee` |
| 2026-03-22 | `v5.1.0-npr-port-win64-0b5a1dd68c06` | `0b5a1dd68c06` |
| 2026-03-21 | `v5.1.0-npr-port-win64-e3f8fa33c23f` | `e3f8fa33c23f` |
| 2026-03-19 | `v5.1.0-npr-port-win64-c4b7253e825b` | `c4b7253e825b` |

## 2026-03-25

### `v5.1.0-npr-port-win64-710a1b4934d6`

发布链接：https://github.com/bb-yi/blender/releases/tag/v5.1.0-npr-port-win64-710a1b4934d6

#### 新增功能
- 新增 `Scene Time` 节点，提供 `Frame`、`Seconds`、`Timeline`、`Scaled Frame` 输出。
- `Scene Color` 节点新增 `Position` 源，可在 `Filter Materials` 中读取世界空间位置。
- 新增 `World Environment` 节点，可直接采样 Eevee 世界环境颜色。
- 新增 `Light Probe Color` 节点，可直接读取反射探头与环境漫反射探头结果。
- 新增 `Bevel` 节点，在 Eevee 中输出近似倒角法线。
- `Curvature` 节点新增 `Bevel Normal` 输出。

#### 修复与改进
- `Filter Materials` 新增 `Execution Stage`，支持 `Before Volume Fog`、`Before Depth of Field`、`Before Composite`。
- 移除之前的 `Overlay Inputs` 方案，统一为 `Filter Materials` 执行阶段控制。
- `Portal In / Portal Out` 调整到 `Layout` 菜单下。
- 补充相关测试与使用文档。

#### Blender 信息
- **版本**：`Blender 5.1.0`
- **分支**：`npr-port-5.1`
- **构建哈希**：`710a1b4934d6`
- **平台**：`Windows x64`
- **构建系统**：`CMake`

## 2026-03-23

### `v5.1.0-npr-port-win64-66ed2fb9cad6`

发布链接：https://github.com/bb-yi/blender/releases/tag/v5.1.0-npr-port-win64-66ed2fb9cad6

#### 新增功能
- `Scene Color` 节点新增 `Shadow` 源，可在 `Filter Materials` 中读取场景阴影。
- `Shader Info` 节点保留 `Stable` 稳定阴影模式，可直接输出稳定阴影灰度。

#### 修复与改进
- 移除 `Soft Stable` 及相关代码，`Shader Info` 阴影模式仅保留 `Built-in` 与 `Stable`。
- 梳理并清理 Eevee 场景阴影过滤链路，补充对应回归测试。

#### Blender 信息
- **版本**：`Blender 5.1.0`
- **分支**：`npr-port-5.1`
- **构建哈希**：`66ed2fb9cad6`
- **平台**：`Windows x64`
- **构建系统**：`CMake`

## 2026-03-22

### `v5.1.0-npr-port-win64-19c826ccb5ee`

发布链接：https://github.com/bb-yi/blender/releases/tag/v5.1.0-npr-port-win64-19c826ccb5ee

#### 新增功能
- `Curvature` 与 `Raycast` 节点现在可以在 `NPR Tree` 中使用。
- 新增 `Blender 5.1 NPR Port` 使用说明文档。

#### 修复与改进
- 为 `Curvature` 节点补充 `Local` 选项，用于区分局部与全局深度采样。
- 调整 `Scene Color` 的默认采样坐标逻辑，使其更贴近相机视图与最终渲染结果。
- 将 `Portal In / Portal Out` 调整到 `Layout` 菜单分类下。

#### Blender 信息
- **版本**：`Blender 5.1.0`
- **分支**：`npr-port-5.1`
- **构建哈希**：`19c826ccb5ee`
- **平台**：`Windows x64`
- **构建系统**：`CMake`

### `v5.1.0-npr-port-win64-0b5a1dd68c06`

发布链接：https://github.com/bb-yi/blender/releases/tag/v5.1.0-npr-port-win64-0b5a1dd68c06

#### 新增功能
- 调整 `Render Info` 节点的 `Frag Coord` 输出顺序，将其放到最上方。
- 将 `Render Info` 节点的 `Frag Coord.xy` 改为 `0..1` 范围的屏幕 UV。
- 保留 `Frag Coord.z` 作为窗口空间深度输出。

#### Blender 信息
- **版本**：`Blender 5.1.0`
- **分支**：`npr-port-5.1`
- **构建哈希**：`0b5a1dd68c06`
- **平台**：`Windows x64`
- **构建系统**：`CMake`

## 2026-03-21

### `v5.1.0-npr-port-win64-e3f8fa33c23f`

发布链接：https://github.com/bb-yi/blender/releases/tag/v5.1.0-npr-port-win64-e3f8fa33c23f

本次发布基于提交 `e3f8fa33c23f`，主要补充了 Blender 5.1 NPR Port 在材质节点与 Eevee 工作流上的新能力。

#### 新增功能
- 新增 Shader Portal In / Portal Out 节点：在同一节点树内通过名称转发连接，减少长距离拉线，整理材质图更方便。
- 新增 Portal Out 跳转按钮：可从 Portal Out 直接定位到对应的 Portal In 节点。
- 新增 Eevee Render Info 节点：补充 Eevee 渲染状态相关的着色器输入。
- 新增 Screen Derivative 节点：在单个节点内切换 `DDX` / `DDY`，支持 Float / Vector / Color 模式。

#### 修复与改进
- 修复 Filter Materials 在对象/材质槽赋值与预览路径上的问题，避免错误参与普通材质流程。
- 改进 Render Texture 深度输出与 Scene 面板 UI，便于管理 Render Textures / Filter Materials。
- Portal 节点补齐了视口与运行时追踪逻辑，保持节点树更新和显示更稳定。

#### Blender 信息
- 版本：`Blender 5.1.0`
- 分支：`npr-port-5.1`
- 构建哈希：`e3f8fa33c23f`
- 平台：`Windows x64`
- 构建系统：`CMake`

## 2026-03-19

### `v5.1.0-npr-port-win64-c4b7253e825b`

发布链接：https://github.com/bb-yi/blender/releases/tag/v5.1.0-npr-port-win64-c4b7253e825b

- 移植 NPR 分支
- 添加 RT
- 添加滤镜材质

#### Blender 信息
- 版本：`Blender 5.1.0`
- 分支：`npr-port-5.1`
- 构建哈希：`c4b7253e825b`
- 平台：`Windows x64`
- 构建系统：`CMake`
