# Eevee Performance Profiler Implementation Plan

## Goal

参考文档：

- `docs/eevee-render-pipeline-reference.md`

在当前 `Blender 5.1 NPR Port` 中增加一个面向 `Eevee` 的性能检查工具，能够回答两类问题：

1. `哪里慢`
   - 哪个渲染阶段最耗时
   - 哪个模块占比最高
   - 哪些尖峰是偶发的

2. `为什么慢`
   - 当前场景启用了哪些重功能
   - 当前帧有哪些原生 Eevee 或 NPR 扩展路径参与了渲染
   - 哪类系统最有可能是瓶颈来源

本工具不只覆盖 NPR Port 额外添加的功能，也覆盖原生 `Eevee` 主流程。

---

## Recommended Strategy

不要把这个功能拆成：

- 先做“阶段耗时面板”
- 再做“原因分析器”

更优的做法是：

- 先做统一的 `Telemetry Core`
- 第一版 UI 先显示阶段耗时
- 同时采集场景特征快照
- 再基于同一份数据补原因分析规则

这样不会在第二阶段重构数据结构，也不会让“哪里慢”和“为什么慢”分裂成两套系统。

---

## Product Shape

### Tool Name

`Eevee Performance Profiler`

### Phase 1 User-Facing Scope

- 在 viewport 中开启性能采样
- 显示当前帧和最近若干帧平均的 CPU 阶段耗时
- 显示原生 Eevee 与 NPR 扩展的场景特征快照
- 给出轻量的性能原因提示

### Future Scope

- GPU 时间分析
- 细分到更小 pass / sub-pass
- 资源编译与贴图加载尖峰识别
- 单功能开关对比分析

---

## Architecture

### Core Principle

第一版先做 `CPU Stage Profiling`，但从数据结构上预留未来 `GPU Profiling` 的扩展位。

### Runtime Data Model

#### 1. TelemetrySession

负责 viewport 级的性能采样状态：

- 是否启用
- 采样模式
- 平均帧数窗口
- 当前记录缓冲
- 最近若干帧历史

#### 2. FrameRecord

每一帧一条：

- `frame_index`
- `view_kind`
  - `Main View`
  - `Capture View`
  - `Lookdev`
- `total_cpu_ms`
- `stage_samples`
- `feature_snapshot`
- `flags`
  - `shader_compile_spike`
  - `texture_loading_spike`
  - `probe_update_spike`

#### 3. StageSample

每个阶段一条：

- `stage_id`
- `stage_label`
- `cpu_ms`
- `cpu_ms_avg`
- `percent_of_frame`
- `call_count`
- `notes`

未来可补：

- `gpu_ms`
- `gpu_ms_avg`

#### 4. FeatureSnapshot

每帧或低频刷新一次：

- `has_ao`
- `has_dof`
- `has_motion_blur`
- `has_volume`
- `has_subsurface`
- `has_raytrace`
- `has_cryptomatte`
- `shadowed_light_count`
- `sphere_probe_count`
- `planar_probe_count`
- `volume_probe_count`
- `render_texture_count`
- `filter_material_count`
- `npr_material_count`
- `raycast_material_count`
- `glsl_function_material_count`

---

## Phase 1: Stage Profiling Scope

### A. Main View Stages

主视图建议先采这些阶段：

- `RenderBuffers.Acquire`
- `PlanarProbes.SetView`
- `Lights.SetView`
- `Volume.Prepass`
- `Background`
- `Deferred`
- `Filter.BeforeVolumeFog`
- `Volume.Compute`
- `Volume.Resolve`
- `AmbientOcclusion`
- `Forward`
- `Filter.BeforePostFX`
- `MotionBlur`
- `Filter.BeforeDepthOfField`
- `DepthOfField`
- `Filter.BeforeComposite`
- `Film.Accumulate`

这些阶段入口主要在：

- `source/blender/draw/engines/eevee/eevee_view.cc`

### B. Auxiliary Views

附加视图建议采：

- `World.Capture`
- `Probe.Capture`
- `Lookdev`

### C. Module-Level Drilldown

第一版可以只对几个高价值模块提供更细阶段：

#### Depth of Field

- `Setup`
- `Tile Prepare`
- `Background Convolution`
- `Foreground Convolution`
- `Hole Fill`
- `Resolve`

#### Volume

- `Scatter`
- `Integration`
- `Resolve`

#### Shadow

- `TilemapSetup`
- `CasterUpdate`
- `TilemapUpdate`
- `ShadowFilter`

#### Raytrace

- `TileClassify`
- `RayGenerate`
- `Trace.Screen`
- `Trace.Planar`
- `Trace.Fallback`
- `Denoise`

---

## Phase 1: Feature Snapshot Scope

第一版除了时间，也要一起采下面这些“解释上下文”：

### Original Eevee Features

- 是否启用 AO
- 是否启用 DOF
- 是否启用 Motion Blur
- 是否启用 Volume
- 是否启用 Raytrace / Screen Trace
- 是否启用 Subsurface
- 是否启用 Cryptomatte
- 当前阴影灯数量
- 当前探针数量

### NPR Port Features

- `Filter Materials` 总条目数
- 各执行阶段滤镜条目数
- `Render Textures` 总条目数
- `NPR Tree` 材质数量
- `GLSL Function` 材质数量
- `RAYCAST` 材质数量

---

## UI Plan

### Entry

建议放在：

- `N Panel > Eevee > Performance`

可选后续：

- `Viewport Overlay > Eevee Performance`

### Phase 1 UI Layout

#### Section 1: Controls

- `Enable Profiling`
- `Capture Mode`
  - `Off`
  - `Continuous`
  - `Next Frame`
- `Average Window`
  - `1`
  - `8`
  - `30`

#### Section 2: Summary

- `Total CPU Frame Time`
- `Top Slowest Stages`
- `Current FPS` 或 `viewport frame time`

#### Section 3: Stage List

每行：

- 阶段名
- 当前帧 `ms`
- 平均 `ms`
- 占比 `%`

默认按耗时倒序。

#### Section 4: Feature Snapshot

- 原生 Eevee 开关状态
- NPR 扩展开关状态
- 灯光 / 探针 / 滤镜 / NPR / GLSL Function 统计

#### Section 5: Inspector Hints

只做轻量提示，不做复杂专家系统：

- `Depth of Field is currently the biggest cost`
- `3 Filter Materials are active before composite`
- `Raycast materials may be increasing prepass cost`
- `Probe capture updated this frame`

---

## CPU Timing Implementation

### Timing Model

使用轻量 CPU 计时 scope：

- 进入阶段时记录 `start`
- 离开阶段时累计 `elapsed`
- 写入当前 `FrameRecord`

建议实现一个小型 RAII helper：

- `ScopedTelemetrySample`

接口大致类似：

- `ScopedTelemetrySample profiler(session, StageId::Deferred);`

### Important Rule

采样默认关闭。

关闭时：

- 不创建记录
- 不做字符串格式化
- 尽量只保留极轻量分支判断

---

## Reason Inspector Rules

第一版只做最小规则集：

- 如果 `DepthOfField` 高且 `has_dof = true`
  - 提示景深为当前主要性能来源
- 如果 `Filter.BeforeComposite` 高且 `filter_material_count > 0`
  - 提示滤镜链可能是主要瓶颈
- 如果 `Deferred` 高且 `raycast_material_count > 0`
  - 提示 raycast 材质可能增加 prepass / object id 写入开销
- 如果 `World.Capture` 或 `Probe.Capture` 高
  - 提示 probe / world capture 本帧更新较重
- 如果 `Volume.Compute` 高且 `has_volume = true`
  - 提示体积是当前主要性能来源

---

## Suggested File Touches

### Phase 1 Core

- `source/blender/draw/engines/eevee/eevee_instance.hh`
- `source/blender/draw/engines/eevee/eevee_instance.cc`
- `source/blender/draw/engines/eevee/eevee_view.hh`
- `source/blender/draw/engines/eevee/eevee_view.cc`

### Phase 1 Module Hooks

- `source/blender/draw/engines/eevee/eevee_depth_of_field.cc`
- `source/blender/draw/engines/eevee/eevee_volume.cc`
- `source/blender/draw/engines/eevee/eevee_shadow.cc`
- `source/blender/draw/engines/eevee/eevee_raytrace.cc`
- `source/blender/draw/engines/eevee/eevee_material.cc`
- `source/blender/draw/engines/eevee/eevee_filter_material.cc`
- `source/blender/draw/engines/eevee/eevee_render_texture.cc`

### UI / RNA

- 新增一个 Debug / Eevee 性能设置入口
- 可能涉及：
  - `makesrna`
  - `space_view3d` overlay 或侧栏 panel
  - `draw/engines/eevee` 的 runtime debug state

---

## Recommended Development Order

### Step 1

实现 `Telemetry Core`

- `FrameRecord`
- `StageSample`
- `FeatureSnapshot`
- 环形历史缓冲

### Step 2

只在 `eevee_view.cc` 接主阶段计时

- 先不细分模块
- 先让 Top Stages 能跑起来

### Step 3

补 `Feature Snapshot`

- 原生 Eevee 开关
- NPR 扩展计数

### Step 4

补 N 面板 UI

- 总帧时间
- Top Stages
- Feature Snapshot

### Step 5

补 `Inspector Hints`

- 只写高价值规则

### Step 6

补模块级细分

- `DOF`
- `Volume`
- `Shadow`
- `Filter Materials`

---

## Development Flow

下面这部分是更适合真正开工时执行的“开发流程版”，按阶段推进，不建议跳步。

### Phase A: Groundwork

目标：

- 确认最终统计对象
- 锁定第一版只做 `CPU` 计时
- 建立统一的数据结构，避免后续返工

工作内容：

1. 定义 `RuntimeMode`
   - `Viewport`
   - `Viewport Image Render`
   - `Final Render`
   - `Bake`

2. 定义 `ViewKind`
   - `MainView`
   - `CaptureWorld`
   - `CaptureProbes`
   - `RenderTextures`
   - `Lookdev`
   - `ReadResult`

3. 定义 `StageId`
   - 主阶段
   - 关键模块细分阶段

4. 定义 `FrameRecord / ViewRecord / StageSample / FeatureSnapshot`

完成标准：

- 不改 UI
- 不接任何具体计时点
- 只把遥测核心数据结构建好

### Phase B: Main Pipeline CPU Timing

目标：

- 先打通“能统计”的最小闭环
- 只接主链，不碰太细的模块内部

工作内容：

1. 在 `render_sync()` 接同步总时间
2. 在 `render_sample()` 接顶层 sample 视图阶段：
   - `CaptureWorld`
   - `CaptureProbes`
   - `RenderTextures`
   - `MainView`
   - `Lookdev`
3. 在 `render_read_result()` 接 readback 时间
4. 在 `ShadingView::render()` 接主视图大阶段

完成标准：

- 能输出每帧总耗时
- 能看到顶层阶段拆分
- 能区分 `Viewport` 和 `Final Render`

### Phase C: Feature Snapshot

目标：

- 让耗时数据带解释上下文

工作内容：

1. 补原生 Eevee 状态快照
2. 补 NPR Port 状态快照
3. 控制刷新频率，避免过多开销

完成标准：

- 每条记录都能附带：
  - Eevee 原生重功能状态
  - NPR 扩展功能计数

### Phase D: First UI

目标：

- 把第一版数据真正展示出来

工作内容：

1. 增加 `N Panel > Eevee > Performance`
2. 增加开关：
   - `Enable`
   - `Capture Mode`
   - `Average Window`
3. 增加展示：
   - 总 CPU 帧时间
   - 最慢阶段排行
   - 完整阶段列表
   - Feature Snapshot

完成标准：

- viewport 中可直接查看 profiler 结果
- 关闭时几乎无开销

### Phase E: Lightweight Inspector

目标：

- 第一版就回答“为什么慢”

工作内容：

1. 加 5 到 10 条高价值规则
2. 把规则结果显示在同一面板里

完成标准：

- 至少能对以下情况给出提示：
  - DOF
  - Volume
  - Filter Materials
  - Probe capture
  - Raycast 材质

### Phase F: Module Drilldown

目标：

- 从“哪个大阶段慢”进一步看到“内部哪个小步骤慢”

工作内容：

1. 细分 `Depth of Field`
2. 细分 `Volume`
3. 细分 `Shadow`
4. 细分 `Filter Materials`
5. 视情况补 `Raytrace`

完成标准：

- 关键大模块可以展开成内部步骤

### Phase G: Final Render Reporting

目标：

- 不只在 viewport 中可看，也能分析最终渲染图片

工作内容：

1. 在 `Final Render` 中汇总：
   - `Frame Total`
   - `Sync`
   - `Samples Total`
   - `Read Result`
2. 输出渲染结束 summary
3. 视需要写入日志或 json

完成标准：

- 最终渲染结束后，能看到每张图的阶段耗时总览

### Phase H: GPU Timing

目标：

- 在 CPU profiler 稳定后，再补真正的 GPU 时间

工作内容：

1. 先只给少数真正值得的阶段补 GPU 时间：
   - `Deferred`
   - `Volume`
   - `AO`
   - `Forward`
   - `Motion Blur`
   - `Depth of Field`
   - `Filter`
   - `Probe Capture`
2. 不对 `Sync / Readback / Flush` 这种阶段做 GPU 时间展示

完成标准：

- UI 中同时能看 `CPU ms` 与 `GPU ms`

---

## Milestones

为了让开发过程更可控，建议按下面的里程碑推进：

### Milestone 1

- `Telemetry Core`
- 顶层主阶段 CPU 计时

### Milestone 2

- Feature Snapshot
- 第一版 N 面板

### Milestone 3

- Inspector Rules
- Final Render Summary

### Milestone 4

- 模块细分
- GPU 时间

---

## Validation Flow

每一阶段都建议按同样顺序验证：

1. `Viewport`
   - 静态场景
   - 播放动画
   - 开关 AO / DOF / Volume / Motion Blur

2. `NPR Port`
   - 打开 `Filter Materials`
   - 打开 `Render Textures`
   - 使用 `NPR Tree`

3. `Final Render`
   - 单 sample
   - 多 sample
   - 开 AOV / pass

4. `Regression`
   - 关闭 profiler 时确认无异常额外开销
   - 开 profiler 时确认不破坏正常渲染结果

---

## Why This Is Better Than “1 Then 3”

如果先只做阶段耗时面板，再补原因分析：

- 数据结构会返工
- 采样点会返工
- UI 会返工

如果先做统一 `Telemetry Core`：

- 方案 1 只是第一个视图
- 方案 3 只是第二个视图
- 后续加 GPU 时间也不会推翻 CPU 结构

---

## Phase 1 Success Criteria

- 能在 viewport 中开启/关闭性能采样
- 能看到主视图总 CPU 帧时间
- 能看到最慢阶段排行
- 能同时看到原生 Eevee 与 NPR 扩展的关键特征快照
- 能给出最基本的性能原因提示
- 关闭采样时几乎不引入额外开销
