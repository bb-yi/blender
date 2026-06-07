## 2026-03-19  c4b7253e825b

#### 新增功能
- 移植 NPR 分支，将原 NPR 原型分支能力完整迁移到 Blender 5.1
- 添加四张可用的 `Render Texture`
- 添加 `Filter Materials`，支持多个滤镜材质

#### 修复与改进
- 无

## 2026-03-21  e3f8fa33c23f

#### 新增功能
- 新增 `Shader Portal In` / `Portal Out` 节点，可在同一节点树内通过名称转发连接，减少长距离拉线，整理材质图更方便
- 新增 `Portal Out` 跳转按钮，可从 `Portal Out` 直接定位到对应的 `Portal In` 节点
- 新增 Eevee `Render Info` 节点，补充 Eevee 渲染状态相关的着色器输入
- 新增 `Screen Derivative` 节点，可在单个节点内切换 `DDX` / `DDY`，支持 `Float` / `Vector` / `Color` 模式

#### 修复与改进
- 修复 `Filter Materials` 在对象 / 材质槽赋值与预览路径上的问题，避免错误参与普通材质流程
- 改进 `Render Texture` 深度输出与 Scene 面板 UI，便于管理 `Render Textures` / `Filter Materials`
- Portal 节点补齐视口与运行时追踪逻辑，保持节点树更新和显示更稳定

## 2026-03-22  0b5a1dd68c06

#### 新增功能
- 调整 `Render Info` 节点的 `Frag Coord` 输出顺序，将其放到最上方
- 将 `Render Info` 节点的 `Frag Coord.xy` 改为 `0..1` 范围的屏幕 UV
- 保留 `Frag Coord.z` 作为窗口空间深度输出

#### 修复与改进
- 无

## 2026-03-22  19c826ccb5ee

#### 新增功能
- `Curvature` 与 `Raycast` 节点现在可以在 `NPR Tree` 中使用
- 新增 `Blender 5.1 NPR Port` 使用说明文档

#### 修复与改进
- 为 `Curvature` 节点补充 `Local` 选项，用于区分局部与全局深度采样
- 调整 `Scene Color` 的默认采样坐标逻辑，使其更贴近相机视图与最终渲染结果
- 将 `Portal In / Portal Out` 调整到 `Layout` 菜单分类下

## 2026-03-23  66ed2fb9cad6

#### 新增功能
- `Scene Color` 节点新增 `Shadow` 源，可在 `Filter Materials` 中读取场景阴影
- `Shader Info` 节点保留 `Stable` 稳定阴影模式，可直接输出稳定阴影灰度

#### 修复与改进
- 移除 `Soft Stable` 及相关代码，`Shader Info` 阴影模式仅保留 `Built-in` 与 `Stable`
- 梳理并清理 Eevee 场景阴影过滤链路，补充对应回归测试

## 2026-03-25  710a1b4934d6

#### 新增功能
- 新增 `Scene Time` 节点，提供 `Frame`、`Seconds`、`Timeline`、`Scaled Frame` 输出
- `Scene Color` 节点新增 `Position` 源，可在 `Filter Materials` 中读取世界空间位置
- 新增 `World Environment` 节点，可直接采样 Eevee 世界环境颜色
- 新增 `Light Probe Color` 节点，可直接读取反射探头与环境漫反射探头结果
- 新增 `Bevel` 节点，在 Eevee 中输出近似倒角法线
- `Curvature` 节点新增 `Bevel Normal` 输出

#### 修复与改进
- `Filter Materials` 新增 `Execution Stage`，支持 `Before Volume Fog`、`Before Depth of Field`、`Before Composite`
- 移除之前的 `Overlay Inputs` 方案，统一为 `Filter Materials` 执行阶段控制
- `Portal In / Portal Out` 调整到 `Layout` 菜单下
- 补充相关测试与使用文档

## 2026-03-26  15baa38c1e60

#### 新增功能
- `Shader Info` 节点新增 `Soft Filtered` 阴影模式
- World 面板新增 `Environment Lighting > Exclude Collection`，可指定集合让其中物体不接收 Eevee 世界环境和 light probe 的环境照明

#### 修复与改进
- 修复 `Shader To RGB` 在 `NPR / deferred` 路径下的兼容问题，补齐 `closure_to_rgba` 缺失实现
- 修复 GPU shader preprocessor 的若干稳定性问题
- 调整部分 Eevee shader create info 配置，减少特定 shader 走预处理器路径时的不稳定问题

## 2026-03-28  ce1f9ce8ea45

#### 新增功能
- 新增 `World To Tangent` 节点
- 新增 `Basis Transform` 节点
- `Shader Info` 节点新增 `Lightgroup` 属性
- Eevee 灯光数据新增 `Lightgroup ID`
- World 面板新增 `Environment Lighting > Exclude Collection`
- 首选项新增 `Material Selector Previews` 开关
- 更新启动画面右上角版本标识

#### 修复与改进
- 修复 `Shader Info > Ambient Lighting` 在 Volume 光照探头场景下使用面法线取样的问题
- 改进 `Screenspace Info`
- 调整 `Shader Info` 阴影模式逻辑
- 优化 `Shader Info` 的软阴影过滤路径
- 移除 `Curvature` 节点的 `Bevel Normal` 输出
- 移除 `Filter Materials` 中 `Scene Color` 的 `Shadow` 源及相关底层实现
- 从添加菜单中隐藏 `Light Probe Color` 节点
- 调整 `Basis Transform` 节点界面
- 修复 Eevee 平面反射探针、球形光照探针场景下 `NPR Input` 部分输出异常发黑的问题
- 改进反射、折射以及世界环境参与时的 probe radiance 读取逻辑，减少 `NPR Tree` 在探针中的黑屏和错误取样问题
- 调整 `Render Info` 的 `Frag Coord` 输出逻辑
- 更新 `Blender 5.1 NPR Port` 功能说明文档
- 补充 `World To Tangent` 使用说明
- 补充 `Basis Transform` 使用说明
- 补充 `Material Selector Previews` 使用说明，并整理节点截图与使用说明

## 2026-03-29  1eba32610ca6

#### 新增功能
- 无

#### 修复与改进
- 修复 `Environment Lighting > Exclude Collection` 在当前构建中不生效的问题
- 修复自发光材质添加 `NPR Tree` 后发光部分变黑的问题
- 修复 `NPR Groups` 中重复添加同一资产节点组时生成 `.001` 数据块的问题，现在会复用已有节点组数据块

## 2026-03-30  d1ab893d1f16

#### 新增功能
- 无

#### 修复与改进
- 修复部分 `NPR` / 金属材质在光照探针与延迟路径中的错误取样问题，减少异常高亮与颜色错误
- 修复未实际使用的贴图采样路径仍占用 sampler 的问题，降低 `too many samplers in shader` 报错概率
- 修复 `LIGHT_LINKING_UPDATE` 缺失导致的控制台报错，以及 `World -> Object Light Linking` 依赖关系构建失败问题
- 修复当前 Windows 发布构建中的 RNA 节点运行时依赖问题，恢复 5.1 port 包的稳定打包流程

## 2026-03-30  60afe891f178

#### 新增功能
- 新增 Goo Engine 风格的 `SDF Primitive` / `SDF Operator` 节点

#### 修复与改进
- 修复 `NPR Tree` 动画数据无法正确记录与切换的问题，恢复关键帧对应 `Action` 的创建与使用
- 修复 `NPR Tree` 节点参数驱动器无法正常生效的问题

## 2026-03-31  5278339af64e

#### 新增功能
- 新增 Goo Engine 风格的 `SDF Vector Operator` 节点

#### 修复与改进
- 修复部分带 `NPR Tree` 的材质在光照探头反射中的异常采样路径问题，改善错误高亮和颜色异常
- 修复 Windows 控制台部分中文 / UTF-8 输出显示异常的问题

## 2026-04-01  8348696759c2

#### 新增功能
- 新增 `Filter Materials` 专用的 `Filter Object Info` 节点，可读取指定对象的位置、旋转、缩放和视口颜色
- 新增 `Pose Bone` 的 `Hide in Outliner` 控制，可按骨骼整理 Outliner 显示层级

#### 修复与改进
- 无

## 2026-04-02  467ad44a6f37

#### 新增功能
- 新增 `Filter Object Mask` 节点，可在 `Filter Materials` 域中快速对物体蒙版

#### 修复与改进
- 修复 `World Environment` 节点在 `NPR Tree` 中无法正确使用的问题

## 2026-04-02  a9e9f05159f1

#### 新增功能
- 无

#### 修复与改进
- 修复内存溢出与材质编译错误
- 修复 `World Environment` 在 `NPR Tree` 中的世界探针采样
- 修复 `Screenspace Info` 环境回退使用错误探针的问题
- 为已移除的 `Scene Color: SHADOW` 与 `Shader Info: STABLE` 增加旧文件兼容迁移

## 2026-04-02  82f40b4916fc

#### 新增功能
- 新增 `GLSL Function` 节点，可从 `Text` 数据块或外部 GLSL 脚本中选择函数并自动生成节点接口
- `GLSL Function` 节点支持 `@glsl_meta v1` 注释块，可声明参数默认值、范围与子类型

#### 修复与改进
- 修复 `Scene Time` 节点在视口播放时的更新时间同步问题
- 为 `GLSL Function` 节点增加刷新按钮，修改 GLSL 脚本后可直接刷新函数列表与接口
- 补充 `GLSL Function` 转换指南与 `Filter Object Info` 使用说明
- 修复 `Curvature` 节点的 `Local` 模式在 `NPR Tree` 中无法使用的问题

## 2026-04-04  df6e0a0de02a

#### 新增功能
- 修改 `Filter Mask` 使用逻辑，支持单物体、物体列表、集合方式创建蒙版

#### 修复与改进
- 无

## 2026-04-06  0ea564538b5

#### 新增功能
- 新增 Outliner `Eevee Performance` 视图，可查看 Eevee 性能统计与分析信息
- 修改`GLSL Function` 节点的图像采样逻辑,现可以使用闭包输入程序化纹理作为图像

#### 修复与改进
- `GLSL Function` 的 `@glsl_meta v1` 新增 `hide_value`属性
- 增大`Filter Mask`的最大物体上限为`512`

## 2026-04-08  cb0590afaf9c

#### 新增功能
- 滤镜材质支持AOV输出,支持AOV修改后再输出
- `NPR Tree`中`Curvature`节点新增`View`模式,可以让视图和渲染的rim宽度保持一致

#### 修复与改进
- 修复Mac下Eevee shader兼容问题
- 修复`NPR Tree`中`Curvature`节点`Local`模式无效的问题


## 2026-04-11  ce87f014fbda

#### 新增功能


#### 修复与改进
- 修改glsl节点Sample2D的对程序化纹理的内部降级方式,提高glsl代码兼容性
- `glsl Function`节点的`Meta信息`的矢量输入接口添加`color`类型,允许将矢量输入映射为颜色接口

## 2026-04-12  7267281d6575

#### 新增功能
- `Color Ramp`节点新增`OKLab`模式，并将原独立`OKLab Color Ramp`节点合并到`Color Ramp`中
- 新增 Goo Engine 风格的`Twirl`节点
- 新增 Goo Engine 风格的`Water Ripples`节点
- 新增 Goo Engine 风格的`Hex Grid Texture`节点

#### 修复与改进
- 修复`2D vector XYZ socket`的RNA崩溃问题
- 修复删除节点时空引用崩溃问题
- 修复`shader info`节点的`阴影`输出在使用多张贴图时为黑的问题


## 2026-04-12  98b7a261efd7

#### 新增功能
- 材质剔除模式新增`正面剔除`
- `shader info`节点新增`布林冯高光`输出

#### 修复与改进
- 修复删除节点时的空指针崩溃问题
- 修复打包脚本的python环境缺失问题


## 2026-04-13  4d45f17f5fe3

#### 新增功能

#### 修复与改进
- `light info`节点可以在`NPR Tree`添加了
- 为`NPR Tree`的选择框添加过滤
- 修复`AOV`通道不同类型交替添加时覆盖的BUG
- 为`glsl Function`节点在`filter`材质添加支持


## 2026-04-19  cf37c8c56d62

#### 新增功能
- 合并升级blender 5.1.1修改

#### 修复与改进
- 修复属性节点无法输出物体级属性
- 刷新`glsl Function`节点时时不重置参数


## 2026-04-23  48b2f70e89b6

#### 新增功能
- 添加`Refraction Layer`控制

#### 修复与改进
- 修复`npr Tree`在材质启用`置换`时不起作用的bug
- 优化`GLSL Function`节点UI
- 修复Eevee球形探针cubemap视图参数错误导致的断言问题


## 2026-04-26  f7bfa8ed58b8

#### 新增功能
- 新增描边系统,可快速简易的启用描边

#### 修复与改进
- 补全`VC++`运行库
- 修复`曲率`节点在`局部`模式下和`物体属性`冲突
- 实验特性中显示`PR 145849`开关


## 2026-04-27  f4d1f2d656ea

#### 新增功能
- 新增描边系统，可快速简易地启用描边

#### 修复与改进
- 补全 `VC++` 运行库
- 修复 `曲率` 节点在 `局部` 模式下和 `物体属性` 冲突
- 实验特性中显示 `PR 145849` 开关
- 修复使用Vulkan删除或重建带 `Outline Control` 的节点后切换 `Rendered` 时的崩溃


## 2026-04-30  c6888a3c78a2

#### 新增功能
- 添加深度偏移输出
- 添加快速跳转文档和报告BUG按钮

#### 修复与改进
- 当 `glsl Function` 节点引用外部文件打包时,会把文件保存到blender内
- `glsl Function` 节点参数支持折叠面板
- `glsl Function` 节点参数支持接口注释
- 修复 `NPR Tree` 中 `Attribute(Object)` 在大量实例场景下可能崩溃或变黑的问题，改回官方动态 Object Attribute 读取路径
- 修复 `Render Texture` 与 Object Attribute 的 SSBO 绑定槽冲突，避免同一 NPR 材质同时使用时输出异常
- 修复 `UBO` 溢出崩溃
- 修复shader编译与播放并发崩溃
- 修复内置`outline`的遮挡问题和透明度衰减
- 修复启用 Depth Offset 后 NPR/AOV 输出未按深度预通道剔除的问题，避免被遮挡背面继续泄漏到 AOV。


## 2026-05-02  750ad2ff489f

#### 新增功能
- Image to Closure允许将2d纹理解释为3d纹理并在glsl Function采样
- Outline Control 新增 `ID Edge` 开关，可控制 Outline ID / 物体 / 材质边缘线是否参与描边。

#### 修复与改进
- 优化glsl Function节点警告信息
- 修复depth offset之后视图叠加层被遮挡
- 修复前景物体背后存在其他描边物体时，前景描边显得更宽或更深的问题。


## 2026-05-03  b4cf066c51cc

#### 新增功能
- 允许标记`freestyle`边作为`outline`
- `image to closure`节点添加色彩空间选项
- 世界环境添加`NPR Tree`支持

#### 修复与改进
- 修复切换色彩空间时3D类型的`image to closure`崩溃
- 修复图片切换色彩空间时崩溃
- 修复`NPR Tree`为空时快捷键切换无反应
- 修复`合成器`中使用背景图使用`图像`时崩溃

## 2026-05-05  a68d36495a32

#### 新增功能
- 视口通道添加深度预览

#### 修复与改进
- 修复 Cycles 烘焙贴图崩溃
- 修复透射材质影响法线深度描边线
- 合并 PR #18
- 修复打包产物错误
- 修复 Crypto 通道未开启时依然启用的问题

## 2026-05-10  2ac8e6d73b9

#### 新增功能
- 添加 `Light Probe Color` 节点，可以获取光照探头的反射 `cubemap`
- `Light Probe Color` 节点新增粗糙度控制，并补充渲染回归测试

#### 修复与改进
- 修复 `GLSL Function` 灯光访问 helper 在透明、AOV、深度等路径生成非法 GLSL 的问题，避免 NVIDIA 编译器报 `unexpected ')'`
- 修复 `Shader Info` helper 同类未使用变量 fallback 的 GLSL 语法错误


## 2026-05-11  9a151ea3f31

#### 新增功能
- 无

#### 修复与改进
- 修复透明透射材质路径下的 AOV 读取异常
- 修复 Eevee 视口历史缓冲在场景更新、阴影更新和软阴影变化后的刷新问题，减少视口残留和错误复用

## 2026-05-18  f58072364072

#### 新增功能
- 新增 Eevee `Light Shader Output` 节点体系，支持在灯光节点树中输出自定义灯光颜色与衰减
- `Light Shader Output` 支持前向、延迟 / NPR、体积散射、反射探针、平面探针和体积探针烘焙等 Eevee 路径
- 新增 Eevee 太阳光阴影贴图缩放控制，用于调整 Sun shadow map scale
- 新增材质 `Stencil` 支持，可控制模板测试、模板写入、模板顺序和相关 compare / operation 状态
- 新增材质 `ZTest`、`Color Write`、`Depth Write` 控制，并支持 `ZTest Never`
- `GLSL Function` 的 `glsl_light_get()` 现在会让 `diffuse_color`、`specular_color`、`attenuation` 读取 `Light Shader Output` 评估后的结果

#### 修复与改进
- 修复缩小太阳光阴影贴图分辨率后可能出现的阴影伪影
- 修复 `Light Shader Output` 默认强度、范围缩放、衰减控制、节点 UI 布局和快速路径缓存失效问题
- 修复 `Light Shader Output` 在前向路径、NPR / deferred 路径、probe capture 和 volume bake 中的资源绑定与缓存更新问题
- 修复合并 `Light Shader Output` 后的 RNA 绑定问题
- 修复 Pencil+ 相关的 Image DNA 兼容性和 MSVC 构建下的 RNA ABI 问题
- 修复材质 `Stencil` 合并后的 surface state、MaterialKey 哈希、冗余绘制状态切换和 UI guard 问题
- 修复模板状态与 `ZTest` 组合时的版本迁移、shadow tilemap 未定义行为和材质 key 冲突风险
- 为 `ZTest` 和 `Stencil Order` 增加提示说明，标注容易误用的行为边界
- 补充 `Light Shader Output` 回归测试与 `GLSL Function` 灯光访问渲染测试
- 修复关闭首选项 `Material Selector Previews` 后部分材质选择器、材质预览面板和 Python UI 路径仍会触发材质预览图渲染的问题
- 修复光线追踪折射路径下二级层错误清除描边 / AOV，导致 `ZTest Less Equal` 材质描边消失的问题
- 修复混合渲染方式下 Forward 材质不写入 Normal 渲染通道，导致法线控制描边读取到纯黑法线的问题
- 修复透明 AOV 与 `GLSL Function` 灯光访问组合时 deferred hybrid shader 的 closure eval count 不匹配编译错误

## 2026-05-20  0a040bce012

#### 新增功能
- 合并 Blender 5.1.2 更新，包含官方 5.1.2 修复与版本更新
- 新增 Eevee Native PostFX 输出体系，可在 View Layer 中为不同的通道添加景深以及运动模糊效果

#### 修复与改进
- 更新 NPR Port 启动画面
- 修复描边相机效果读取 seed 数据与 velocity prepass 交互的问题，避免 velocity 阶段错误携带灯光数据
- 修复 `GLSL Function` 的 `vec4` 输入在刷新或编译路径中丢失 `w` 分量的问题，并补充对应 Python 回归测试
- - 恢复独立 `OKLab Color Ramp` 节点，普通 `Color Ramp` 保持 RGB / HSV / HSL 工作流，旧 OKLab 模式文件会迁移回独立节点

## 2026-05-24  c6933607751a

#### 新增功能
- 新增原生 Parallax shader 节点，支持 Plane Offset、Parallax Occlusion、Relief Parallax Mapping、Secant Method Relief Mapping。
- 新增 Eevee Color Bake 工作流，支持颜色属性烘焙目标、Eevee 光照/阴影烘焙、Light Shader Color Bake、Shaded Color Bake、Tangent Color Bake 和 GPU 路径。

#### 修复与改进
- 修复 Eevee 透明抖动阴影在视口中的累积问题。
- 修复 Shader Info 对 Light Shader 评估结果的读取与缓存更新。
- 加强 Eevee TAA soft shadow history reset release test 的视口就绪判定，避免把未渲染的灰色相机框误判为有效画面。

## 2026-05-25  17f03f01909

#### 新增功能
- Eevee `Shadow Pool` 最高可选值扩展到 `8192 MB`，默认值保持 `512 MB`，可支持更大规模多灯阴影场景。
- `Shadow Pool` 设置新增 `2048 MB`、`4096 MB`、`8192 MB` 选项，`GI Irradiance Pool` 仍保持原有上限。

#### 修复与改进
- Eevee 阴影图集会按 GPU texture array layer 能力和实际分配结果自动降级，避免旧显卡、Vulkan/OpenGL 后端限制或显存碎片导致崩溃。
- 改进 OpenGL 纹理真实分配失败检测，让阴影池分配失败可以触发 Eevee 的逐级降级 fallback。
- 增加 Eevee Shadow Pool 压力 release test，覆盖 257 灯场景下 `2048 MB` 负例与 `4096/8192 MB` 通过路径。

## 暂存

#### 新增功能
- Eevee `Performance` 视图新增阴影与光照探头耗时归因，可展开查看 Shadow Contexts、Shadow Lights、Probe Costs 等细分统计，便于定位阴影图集、灯光与 probe 更新开销。
- `GLSL Function` 的 `@glsl_meta v1` 支持 `label` 元数据，可为输入/输出 socket 设置本地化或自定义显示名称。
- 新增 `GLSL Script Expression` 节点，可在 Shader 节点树中直接编写单行 GLSL 表达式，并手动定义输入变量。
- `GLSL Script Expression` 节点补充节点内控件与侧栏面板，变量、表达式和输出类型等设置会分组显示，节点 UI 更易整理。
- World / World NPR 节点树现在可以从添加菜单中直接使用 `GLSL Function` 与 `GLSL Script Expression`。
- `GLSL Function` 新增 Define 控制面板，可在节点 UI 中维护编译期开关，并支持默认折叠显示。
- `GLSL Function` 的 `@glsl_meta v1` 新增 `int_choice` 元数据，整数参数可显示为可交互下拉选项。
- `Image Sample` 节点现在可以在 `Filter Materials` 中添加和使用，便于滤镜材质直接采样图像或 AOV 链路。

#### 修复与改进
- 修复 `GLSL Function` 放入节点组后部分路径无法正确内联或编译的问题，并补充节点组回归测试。
- 修复 `GLSL Function` 经过节点组内联后颜色 alpha 分量丢失的问题。
- 修复混合 Forward 材质会清掉或丢失背后 AOV 数据的问题，保留透明/混合表面后的 AOV 读取结果。
- 修复 `Scene Color` 的 `Position` 输出在 filter pass 中采样 UV 偏移的问题，使位置通道读取与屏幕像素对齐。
- 改进 Eevee 性能分析器归因逻辑，减少材质、阴影和 probe 统计被错误归入其他阶段的情况。
- 修复 `GLSL Script Expression` 节点面板绘制时的崩溃问题，并调整控件折叠与节点内显示逻辑。
- 修复 View Layer 集合开启 Holdout 后，集合内带 `Outline Control` 的材质仍会产生 NPR 描边的问题；Holdout 对象仍参与深度/遮挡，但不再写入材质描边或 marked-edge 描边。
- 修复 World / World NPR 中普通 `Image Texture` 未连接 `Vector` 时固定采样同一 texel 的问题，默认改为使用窗口坐标，避免多张 2D 贴图混合退化为常量颜色。
- 修复 NPR 材质与多贴图混合场景下 texture sampler 槽位重复占用的问题，降低 shader 编译失败和错误采样概率。
- `GLSL Function` 转换指南补充 metadata 要求，减少缺少参数声明时的解析歧义。
- 修复 `GLSL Function` Define 解析边界、面板可见性和节点刷新后 UI 状态不一致的问题。
- 修复 `GLSL Function` 整数下拉选项的交互、RNA 缓存和节点 socket 缓存更新问题。
- 修复最终渲染与视口渲染模式判断混用的问题，避免 filter / NPR 路径按错误上下文执行。
- 修复 `Image Sample` 在滤镜材质中不能从添加菜单使用的问题，并补充 AOV 偏移采样回归测试。
- 修复 Eevee filter material pass 依赖不稳定的问题，减少滤镜材质读取上一阶段结果时的错序、漏同步和 stale resource 风险。

# TODO

- [ ] 提高阴影分辨率
- [ ] 材质debug器,显示信息
- [×] 曲面细分着色器
- [ ] 屏幕光线追踪 GI
- [ ] 平滑AO
- [ ] GLSL通用库
- [×] 次表面
- [ ] 历史缓冲纹理
- [×] gpu实例
- [ ] outline优化
- [ ] 区分自阴影和投射阴影
