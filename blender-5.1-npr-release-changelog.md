# Blender 5.1 NPR Port 发布更新日志

这份文档汇总当前已经发布到 GitHub 的 `Blender 5.1 NPR Port` Release 说明，按发布时间从新到旧整理。

## 待发布草稿

### 当前版本相对 `v5.1.0-npr-port-win64-15baa38c1e60`

#### 新增功能
- 新增 `World To Tangent` 节点。
  - 位置：`Add > Utilities > Vector > World To Tangent`
  - 可将世界空间方向向量转换为当前表面的切线空间方向，支持指定 `UV Map` 作为切线基底。
- `Shader Info` 节点新增整数 `Lightgroup` 属性。
  - 只有灯光 `Lightgroup ID` 与节点一致时，该灯光才会参与 `Shader Info` 的直接光照与阴影计算。
- Eevee 灯光数据新增 `Lightgroup ID` 属性。
  - 位置：`Light Data > Light > Lightgroup ID`
  - 默认组为 `0`，与 `Shader Info` 的默认分组逻辑保持一致。
- 首选项新增 `Material Selector Previews` 开关。
  - 位置：`Preferences > Editing > Objects > Materials`
  - 可控制材质下拉列表是否渲染材质预览图，关闭后改为普通材质图标以减少列表展开时的预览渲染开销。

#### 修复与改进
- 修复 `Shader Info > Ambient Lighting` 在 Volume 光照探头场景下使用面法线取样的问题。
  - 现在会使用平滑后的表面法线做 probe 取样，减少模型表面出现“平直着色/分面感”的间接光照结果。
- 调整 `Shader Info` 灯光组界面与兼容逻辑。
  - 节点端改为整数分组输入，同时保留旧字符串属性用于兼容旧文件。
- 调整启动图右上角版本标识。
  - 在版本号后追加 `npr post` 和构建日期，便于区分当前自定义构建。
- 更新相关功能文档。
  - 补充 `World To Tangent` 与 `Material Selector Previews` 的使用说明。

## 发布列表

| 发布时间 | Tag | 构建哈希 |
| --- | --- | --- |
| 2026-03-31 | `v5.1.0-npr-port-win64-60afe891f178` | `60afe891f178` |
| 2026-03-30 | `v5.1.0-npr-port-win64-d1ab893d1f16` | `d1ab893d1f16` |
| 2026-03-29 | `v5.1.0-npr-port-win64-1eba32610ca6` | `1eba32610ca6` |
| 2026-03-25 | `v5.1.0-npr-port-win64-710a1b4934d6` | `710a1b4934d6` |
| 2026-03-23 | `v5.1.0-npr-port-win64-66ed2fb9cad6` | `66ed2fb9cad6` |
| 2026-03-22 | `v5.1.0-npr-port-win64-19c826ccb5ee` | `19c826ccb5ee` |
| 2026-03-22 | `v5.1.0-npr-port-win64-0b5a1dd68c06` | `0b5a1dd68c06` |
| 2026-03-21 | `v5.1.0-npr-port-win64-e3f8fa33c23f` | `e3f8fa33c23f` |
| 2026-03-19 | `v5.1.0-npr-port-win64-c4b7253e825b` | `c4b7253e825b` |

## 2026-03-31

### `v5.1.0-npr-port-win64-60afe891f178`

发布链接：https://github.com/bb-yi/blender/releases/tag/v5.1.0-npr-port-win64-60afe891f178

#### 新增功能
- 新增 Goo Engine 风格的 `SDF Primitive` / `SDF Operator` 节点。

#### 修复与改进
- 修复 `NPR Tree` 动画数据无法正确记录与切换的问题，恢复关键帧对应 `Action` 的创建与使用。
- 修复 `NPR Tree` 节点参数驱动器无法正常生效的问题。

#### Blender 信息
- **版本**：`Blender 5.1.0`
- **分支**：`npr-port-5.1`
- **构建哈希**：`60afe891f178`
- **平台**：`Windows x64`
- **构建系统**：`CMake`

## 2026-03-30

### `v5.1.0-npr-port-win64-d1ab893d1f16`

发布链接：https://github.com/bb-yi/blender/releases/tag/v5.1.0-npr-port-win64-d1ab893d1f16

#### 修复与改进
- 修复部分 `NPR` / 金属材质在光照探针与延迟路径中的错误取样问题，减少异常高亮与颜色错误。
- 修复未实际使用的贴图采样路径仍占用 sampler 的问题，降低 `too many samplers in shader` 报错概率。
- 修复 `LIGHT_LINKING_UPDATE` 缺失导致的控制台报错，以及 `World -> Object Light Linking` 依赖关系构建失败问题。
- 修复当前 Windows 发布构建中的 RNA 节点运行时依赖问题，恢复 5.1 port 包的稳定打包流程。

#### Blender 信息
- **版本**：`Blender 5.1.0`
- **分支**：`npr-port-5.1`
- **构建哈希**：`d1ab893d1f16`
- **平台**：`Windows x64`
- **构建系统**：`CMake`

## 2026-03-29

### `v5.1.0-npr-port-win64-1eba32610ca6`

发布链接：https://github.com/bb-yi/blender/releases/tag/v5.1.0-npr-port-win64-1eba32610ca6

#### 新增功能
- 新增 `Basis Transform` 节点。

#### 修复与改进
- 修复 `Environment Lighting > Exclude Collection` 在当前构建中不生效的问题。
- 修复 Eevee 平面反射探针、球形光照探针场景下 `NPR Input` 部分输出异常发黑的问题。
- 改进反射、折射以及世界环境参与时的 probe radiance 读取逻辑，减少 `NPR Tree` 在探针中的黑屏和错误取样问题。
- 修复自发光材质添加 `NPR Tree` 后发光部分变黑的问题。
- 修复 `Render Info > Frag Coord` 在相机视图下仍按窗口尺寸计算的问题。
- 修复 `NPR Groups` 中重复添加同一资产节点组时生成 `.001` 数据块的问题，现在会复用已有节点组数据块。
- 移除 `Curvature` 节点的 `Bevel Normal` 输出。
- 移除 `Filter Materials` 中 `Scene Color` 的 `Shadow` 选项及相关底层实现。
- 从添加菜单中隐藏 `Light Probe Color` 节点。
- 调整 `Shader Info` 阴影模式逻辑，仅保留默认与平滑模式。

#### Blender 信息
- **版本**：`Blender 5.1.0`
- **分支**：`npr-port-5.1`
- **构建哈希**：`1eba32610ca6`
- **平台**：`Windows x64`
- **构建系统**：`CMake`

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
