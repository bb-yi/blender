# Eevee 渲染管线参考

## 文档目的

这份文档用于说明当前 `Blender 5.1 NPR Port` 中，`Eevee` 在一帧渲染时大致会经过哪些步骤、每个步骤负责做什么，以及哪些地方属于原生 `Eevee`，哪些地方是当前分支额外增加的 NPR 扩展。

这份文档面向：

- 功能开发
- 性能分析工具设计
- 渲染问题排查

主要对应代码：

- `source/blender/draw/engines/eevee/eevee_instance.cc`
- `source/blender/draw/engines/eevee/eevee_view.cc`
- `source/blender/draw/engines/eevee/eevee_pipeline.cc`

---

## 一、先理解三个层级

要看懂 `Eevee` 的渲染流程，先把它分成三个层级：

### 1. Sync

把场景、对象、材质、灯光、探针、相机等数据收集起来，整理成这一帧真正要提交给 GPU 的渲染数据。

### 2. Per-Sample Render

真正执行一次 sample 的渲染，包括：

- world capture
- probe capture
- render textures
- main view
- lookdev

### 3. Output / Readback

把 sample 结果显示到 viewport，或者在最终渲染时读回 render passes / AOV，写入 `RenderResult`。

---

## 二、不同运行模式下，“一帧”的含义不同

虽然都叫“一帧”，但不同模式下含义并不完全一样。

### 1. Viewport

对应：

- `Instance::draw_viewport()`

通常一次 UI 重绘只会执行一次 `render_sample()`，如果采样还没完成，会继续请求 redraw。

### 2. Viewport Image Render

对应：

- `Instance::draw_viewport_image_render()`

会循环执行 `render_sample()`，直到 viewport 采样结束。

### 3. Final Render

对应：

- `Instance::render_frame()`

会循环执行多个 `render_sample()`，结束后再做：

- `film.cryptomatte_sort()`
- `render_read_result()`

所以对于最终渲染，一张图并不一定只对应一次 sample，而通常是：

- 一次 `render_sync`
- 多次 `render_sample`
- 一次 `readback`

---

## 三、顶层总流程

从逻辑上看，一张图的完整流程可以整理成：

1. `render_sync()`
2. 循环执行 `render_sample()`
3. `render_read_result()`

而一次 `render_sample()` 内部又会依次做：

1. `capture_view.render_world()`
2. `lookdev.rotate_world()`
3. `capture_view.render_probes()`
4. `render_textures.render()`
5. `main_view.render()`
6. `lookdev_view.render()`
7. `motion_blur.step()`

---

## 四、Sync 阶段做了什么

对应：

- `Instance::begin_sync()`
- `Instance::object_sync()`
- `Instance::end_sync()`

这一段的本质是：

- 从 `Depsgraph` 和 `Scene` 收集数据
- 为各模块准备 pass、shader、buffer、uniform
- 遍历场景对象并分发到各自模块

### 1. begin_sync

大致顺序：

1. `world.sync()`
2. `materials.begin_sync()`
3. `velocity.begin_sync()`
4. `lights.begin_sync()`
5. `shadows.begin_sync()`
6. `volume.begin_sync()`
7. `pipelines.begin_sync()`
8. `cryptomatte.begin_sync()`
9. `sphere_probes.begin_sync()`
10. `light_probes.begin_sync()`
11. `depth_of_field.sync()`
12. `raytracing.sync()`
13. `motion_blur.sync()`
14. `hiz_buffer.sync()`
15. `main_view.sync()`
16. `film.sync()`
17. `render_textures.begin_sync()`
18. `filter_materials.begin_sync()`
19. `ambient_occlusion.sync()`
20. `volume_probes.sync()`
21. `lookdev.sync()`

这一段主要负责：

- 初始化当前帧的模块状态
- 决定哪些系统要参与本帧渲染
- 准备 view、film、buffer、pipeline 参数

### 2. object_sync

`object_sync()` 会遍历场景里当前可见、可渲染的对象，并按类型分发：

- `OB_LAMP` -> 灯光同步
- `OB_MESH` -> 网格同步
- `OB_POINTCLOUD` -> 点云同步
- `OB_VOLUME` -> 体积同步
- `OB_CURVES` -> 曲线同步
- `OB_LIGHTPROBE` -> probe 同步

它的作用是：

- 把对象变成 draw data / resource handle / material passes
- 告诉阴影、体积、probe、材质系统哪些对象参与本帧

### 3. end_sync

`end_sync()` 负责把前面收集好的信息真正收束成可以渲染的状态：

- 结束材质同步
- 结束 velocity、volume、shadow、light 等模块同步
- 判断需要哪些静态 shader
- 结束 sampling、subsurface、film、pipelines、render_textures、filter_materials、probes
- 上传 uniform data

本质上，这一步结束后：

- 当前帧需要的 pass 都准备好了
- 当前帧需要的 shader / resource 都已经可用或进入等待队列了

---

## 五、Per-Sample Render：一次 sample 内做了什么

对应：

- `Instance::render_sample()`

如果当前 viewport 已经采样完成，那么这次不会继续真正渲染，而是直接：

- `film.display()`
- `lookdev.display()`

如果还需要继续 sample，则顺序如下。

### 1. sampling.step

更新当前 sample 计数和 jitter 状态。

它影响：

- TAA
- accumulation
- motion blur 的 sample 逻辑

### 2. capture_view.render_world

更新世界环境捕获。

主要用途：

- world probe
- probe 输入
- 某些世界环境相关路径

### 3. lookdev.rotate_world

更新 lookdev 相关的世界旋转状态。

### 4. capture_view.render_probes

更新反射 probe / sphere probes。

它通常不是每帧都重，但一旦 probe 需要更新，可能产生明显尖峰。

### 5. render_textures.render

这是当前分支新增的步骤。

作用：

- 渲染场景级 `Render Textures`
- 为后续材质节点中的 `Render Texture` 采样提供输入

### 6. main_view.render

主视图真正渲染，绝大多数真正的表面着色、滤镜、景深、体积、前向透明都在这里发生。

### 7. lookdev_view.render

如果启用了 lookdev reference spheres，会在这里额外渲染预览球。

### 8. motion_blur.step

完成 sample 后推进运动模糊内部状态。

---

## 六、Main View 是整条管线的核心

对应：

- `ShadingView::render()`

这一段可以看成“真正的一帧画面是怎么被拼出来的”。

### Step 1. update_view

更新主视图矩阵和 jitter view：

- camera projection
- overscan
- pixel jitter
- DOF jitter

### Step 2. RenderBuffers.acquire

申请这一帧需要的主缓冲：

- depth
- combined color
- vector
- 其他 render pass 相关纹理

### Step 3. planar_probes.set_view

给平面 probe 设置当前视图，保证后续 probe 路径和主视图缓冲一致。

### Step 4. 建立 framebuffer

建立：

- `combined_fb`
- `prepass_fb`
- `gbuffer_fb`

这些 framebuffer 决定后续每一步渲染往哪里写。

### Step 5. GBuffer.acquire

分配延迟渲染所需的 `GBuffer`：

- header
- normal
- closure

这是 deferred 管线的基础。

### Step 6. 清理本帧缓冲

包括：

- 清 `vector`
- 清 `object_id`
- 清 `prepass_normal`
- 清 `combined color`
- 清 `depth`

### Step 7. background.clear

先把背景清掉。

### Step 8. lights.set_view

设置灯光与当前视图相关的参数。

### Step 9. hiz_buffer.set_source

指定 HiZ 使用的深度源。

这一步主要是给：

- raytrace
- 屏幕空间路径
- 某些优化和调试功能

### Step 10. volume.draw_prepass

体积预处理。

### Step 11. background.render

渲染世界背景。

当前分支这里有一个很重要的差异：

- `NPR refraction` 需要读取已经合成好的背景
- 所以背景在 deferred 前就被画到 `combined_tx`

### Step 12. deferred.render

这是最核心、通常也最贵的一步。

这里会做：

- 主表面的 deferred shading
- GBuffer 相关流程
- light evaluation
- combine
- NPR 路径

对于大多数不透明表面，这里是主成本来源。

### Step 13. gbuffer.release

deferred 主阶段结束后释放 GBuffer。

### Step 14. Filter.BeforeVolumeFog

如果 `Filter Materials` 在 `Before Volume Fog` 有条目，这里会执行。

这是当前分支新增的阶段。

### Step 15. volume.draw_compute

体积计算。

### Step 16. volume.draw_resolve

把体积结果 resolve 到主图像。

### Step 17. ambient_occlusion.render_pass

执行 AO。

### Step 18. forward.render

执行 forward 路径。

这里主要处理：

- 前向透明
- 一些不走 deferred 的路径

### Step 19. Debug / Overlay Draw

当前视图可能额外画一些 debug / display 信息：

- lights debug
- hiz debug
- shadows debug
- volume probes viewport draw
- sphere probes viewport draw
- planar probes viewport draw

这些更多是调试和可视化用，不属于主表面着色本体。

### Step 20. Filter.BeforePostFX

如果当前分支的 `Filter Materials` 在 `Before PostFX` 有条目，这里会执行。

### Step 21. render_postfx

进入后处理阶段。

### Step 22. film.accumulate

把这次 sample 的结果累积到最终 film。

### Step 23. 释放临时资源

包括：

- shadow filter
- render buffers
- postfx temporary texture

---

## 七、PostFX 阶段内部怎么走

对应：

- `ShadingView::render_postfx()`

进入 postfx 后，逻辑是：

### 1. 判断是否需要 postfx

只有以下任一启用时才会真正走：

- `Motion Blur`
- `Depth of Field`
- `Filter.BeforeDepthOfField`
- `Filter.BeforeComposite`

### 2. 准备 postfx 临时纹理

如果需要 postfx pass，会申请 `postfx_tx` 作为中间结果。

### 3. motion_blur.render

先做运动模糊。

### 4. Filter.BeforeDepthOfField

如果当前分支有这个执行阶段的滤镜，这里执行。

### 5. depth_of_field.render

执行景深。

### 6. Filter.BeforeComposite

如果当前分支有这个执行阶段的滤镜，这里执行。

### 7. 返回最终 postfx 输出

最终返回一个纹理，交给 `film.accumulate()`。

---

## 八、Capture View 做了什么

对应：

- `CaptureView::render_world()`
- `CaptureView::render_probes()`

### 1. render_world

作用：

- 更新世界环境捕获
- 渲染 cubemap 六个面
- remap 到 octahedral projection
- 更新 world probe 输入

这一步通常只在世界或 probe 相关内容变化时才重。

### 2. render_probes

作用：

- 更新 sphere probes
- 为每个 probe 渲染 cubemap 六面
- 用 probe pipeline 处理 surface capture
- 写回 probe atlas

这一步一旦 probe 需要更新，往往会带来明显耗时尖峰。

---

## 九、Lookdev View 做了什么

对应：

- `LookdevView::render()`

它只在启用了 reference spheres 时才执行。

作用：

- 用单独 view 渲染 lookdev sphere
- 把预览球叠加显示出来

这不是主场景本体渲染成本，而是附加成本。

---

## 十、Final Render 结束后还会做什么

对应：

- `Instance::render_frame()`
- `Instance::render_read_result()`

### 1. render_frame

最终渲染图片时会：

1. 循环执行 `render_sample()`
2. 在 sample 之间：
   - 更新进度
   - `GPU_flush()`
   - `GPU_render_step()`
3. 全部 sample 完成后：
   - `film.cryptomatte_sort()`
   - `render_read_result()`

### 2. render_read_result

这里会把 GPU 上的结果读回 CPU，并写入：

- `RenderPass`
- `AOV`
- `RenderResult`

它包括：

- 遍历已启用的 render pass
- 读取对应 pass 纹理
- 读取 AOV
- 修正未启用 vector pass 的默认值

这一步不是“画图”，但对最终渲染的总耗时同样重要。

---

## 十一、当前分支相比原生 Eevee 额外增加了哪些关键步骤

这个分支除了原生 `Eevee` 外，还多了几条会直接影响一帧耗时的路径：

### 1. Render Textures

对应：

- `render_textures.render()`

作用：

- 先从指定相机渲染额外场景纹理
- 再供材质中的 `Render Texture` 节点采样

### 2. Filter Materials

对应：

- `Filter.BeforeVolumeFog`
- `Filter.BeforePostFX`
- `Filter.BeforeDepthOfField`
- `Filter.BeforeComposite`

作用：

- 在 Eevee 主渲染链不同位置插入全屏滤镜 pass

### 3. NPR Deferred Path

在 `deferred.render()` 内部，这个分支额外支持：

- `NPR Tree`
- `NPR foreach light`
- `NPR refraction`

这些都会直接影响主表面阶段的耗时结构。

### 4. GLSL Function / Raycast / NPR Tree

这些功能会间接影响：

- 材质同步
- prepass
- object id 写入
- deferred / npr 路径

所以虽然它们不一定单独形成一个 pass，但会改变整体耗时分布。

---

## 十二、从性能分析角度，最值得监控的阶段

如果后面要做性能分析工具，建议至少先监控这些大阶段：

- `Sync`
- `CaptureWorld`
- `CaptureProbes`
- `RenderTextures`
- `MainView`
- `Lookdev`
- `ReadResult`

然后在 `MainView` 里继续细分：

- `RenderBuffers.Acquire`
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

这套拆法同时适用于：

- 原生 `Eevee`
- 当前 `NPR Port`

---

## 十三、简化版顺序表

如果只想记最核心的一句话版本，可以这样理解：

### 渲染前

1. 同步世界、材质、灯光、阴影、体积、探针、相机、film、后处理模块
2. 遍历对象，收集网格、曲线、体积、灯光、probe
3. 结束同步并上传 uniform / pass 状态

### 一次 sample

1. 更新 sample / jitter
2. 更新世界捕获
3. 更新 probe 捕获
4. 更新 Render Textures
5. 渲染主视图
6. 渲染 lookdev
7. 推进 motion blur

### 主视图内部

1. 更新视图矩阵
2. 分配 render buffers / gbuffer
3. 清理本帧缓冲
4. 渲染背景
5. 渲染 deferred 主表面
6. 执行滤镜与体积
7. 执行 AO 与 forward
8. 执行 postfx
9. 累积到 film

### 最终输出

1. 如果是 viewport，直接 display
2. 如果是 final render，读回 render pass / AOV

---

## 十四、这份文档适合怎么用

这份文档最适合拿来做三件事：

1. 设计性能分析工具
   - 决定阶段切点放哪里

2. 排查“哪个步骤可能慢”
   - 判断问题是在 sync、capture、main view 还是 readback

3. 判断新增功能应该插在管线哪里
   - 例如某个新功能应该是 main view 前、postfx 前还是 final readback 前

---

## 十五、各阶段的 CPU / GPU 属性与可计时性

如果后面要做性能分析工具，不能只问“这一段是不是 GPU 阶段”，还要同时回答：

- 这一段主要是 `CPU` 还是 `GPU`
- 这一段适不适合做 `CPU` 计时
- 这一段适不适合做 `GPU` 计时

因为很多阶段虽然发生在渲染流程里，但本质上并不是“真正的 GPU 绘制成本”。

### 一、三种大类

#### 1. CPU 为主阶段

典型特征：

- 遍历对象
- 构建材质 pass
- 同步灯光 / 阴影 / probe 状态
- 更新 uniform / resource

这类阶段很适合做 `CPU` 计时，但通常不适合做 `GPU` 计时。

#### 2. GPU 为主阶段

典型特征：

- draw / dispatch / full-screen pass
- shaded surface pass
- postfx
- shadow / probe / volume / filter 这类真正的渲染与计算

这类阶段同时适合做：

- `CPU` 计时
- `GPU` 计时

不过两者测出来的数值含义不同：

- `CPU` 计时更接近“提交这一段命令花了多久”
- `GPU` 计时更接近“显卡真正执行这段工作花了多久”

#### 3. CPU/GPU 混合阶段

典型特征：

- 结果读回
- GPU flush
- render step
- 等待 shader 编译
- 等待纹理加载

这类阶段虽然可以做 `CPU` 计时，但不适合简单地归类成“GPU 渲染阶段耗时”。

因为它们经常包含：

- CPU 等待 GPU
- CPU 等待驱动
- CPU 等待资源就绪

---

### 二、按当前主流程拆分

下表是当前 `Eevee` 主流程里最重要阶段的属性概览。

| 阶段 | 主要归属 | 适合 CPU 计时 | 适合 GPU 计时 | 说明 |
|---|---|---|---|---|
| `begin_sync` | CPU | 是 | 否 | 模块状态初始化、参数准备 |
| `object_sync` | CPU | 是 | 否 | 遍历对象并收集 draw data |
| `end_sync` | CPU | 是 | 否 | 收束同步结果、结束各模块 sync |
| `render_sync` 总计 | CPU | 是 | 否 | 最适合作为同步总成本 |
| `sampling.step` | CPU | 是 | 否 | 更新 sample / jitter |
| `capture_view.render_world` | GPU 主 | 是 | 是 | 世界 cubemap / world capture |
| `capture_view.render_probes` | GPU 主 | 是 | 是 | probe capture，常见尖峰来源 |
| `render_textures.render` | GPU 主 | 是 | 是 | 当前分支新增 Render Texture 预渲染 |
| `main_view.render` 总计 | GPU 主 | 是 | 是 | 主视图总成本 |
| `update_view` | CPU | 是 | 否 | 更新矩阵、jitter、overscan |
| `RenderBuffers.acquire` | CPU 主 | 是 | 不建议 | 更偏资源准备，不是主要绘制阶段 |
| `planar_probes.set_view` | CPU 主 | 是 | 不建议 | 设置视图状态 |
| `GBuffer.acquire` | CPU 主 | 是 | 不建议 | 资源准备 |
| 缓冲 clear | GPU 主 | 是 | 可做但优先级低 | 通常可以并入大阶段 |
| `background.render` | GPU 主 | 是 | 是 | 渲染世界背景 |
| `deferred.render` | GPU 主 | 是 | 是 | 主表面延迟渲染核心阶段 |
| `Filter.BeforeVolumeFog` | GPU 主 | 是 | 是 | 当前分支新增全屏滤镜 |
| `volume.draw_prepass` | GPU 主 | 是 | 是 | 体积预处理 |
| `volume.draw_compute` | GPU 主 | 是 | 是 | 体积计算大头 |
| `volume.draw_resolve` | GPU 主 | 是 | 是 | 体积合成 |
| `ambient_occlusion.render_pass` | GPU 主 | 是 | 是 | AO pass |
| `forward.render` | GPU 主 | 是 | 是 | forward / transparent 路径 |
| `Filter.BeforePostFX` | GPU 主 | 是 | 是 | PostFX 前滤镜 |
| `motion_blur.render` | GPU 主 | 是 | 是 | 运动模糊 |
| `Filter.BeforeDepthOfField` | GPU 主 | 是 | 是 | DOF 前滤镜 |
| `depth_of_field.render` | GPU 主 | 是 | 是 | 景深 |
| `Filter.BeforeComposite` | GPU 主 | 是 | 是 | Composite 前滤镜 |
| `film.accumulate` | GPU 主 | 是 | 是 | sample 累积 |
| `lookdev_view.render` | GPU 主 | 是 | 是 | lookdev 附加视图 |
| `render_read_result` | 混合 | 是 | 否 | pass / AOV 读回和写入 RenderResult |
| `GPU_flush` | 混合 | 是 | 否 | 同步点，不是独立渲染模块 |
| `GPU_render_step` | 混合 | 是 | 否 | 更像提交 / 执行边界，不应当作渲染 pass |

---

### 三、最值得先做 CPU 计时的阶段

第一版 CPU profiler 最值得先统计：

#### 顶层

- `render_sync`
- `capture_view.render_world`
- `capture_view.render_probes`
- `render_textures.render`
- `main_view.render`
- `lookdev_view.render`
- `render_read_result`

#### Main View 内部

- `background.render`
- `deferred.render`
- `Filter.BeforeVolumeFog`
- `volume.draw_compute`
- `volume.draw_resolve`
- `ambient_occlusion.render_pass`
- `forward.render`
- `Filter.BeforePostFX`
- `motion_blur.render`
- `Filter.BeforeDepthOfField`
- `depth_of_field.render`
- `Filter.BeforeComposite`
- `film.accumulate`

这些阶段的好处是：

- 边界清晰
- 用户容易理解
- 后续补 GPU 计时时也能复用

---

### 四、最值得以后补 GPU 计时的阶段

如果后面要上 GPU 时间分析，最优先的是这些真正的渲染 / 计算阶段：

- `capture_view.render_world`
- `capture_view.render_probes`
- `render_textures.render`
- `background.render`
- `deferred.render`
- `Filter.*`
- `volume.draw_prepass`
- `volume.draw_compute`
- `volume.draw_resolve`
- `ambient_occlusion.render_pass`
- `forward.render`
- `motion_blur.render`
- `depth_of_field.render`
- `film.accumulate`
- `lookdev_view.render`

它们是最接近“显卡到底把时间花在哪”的地方。

---

### 五、不建议做 GPU 计时的阶段

下面这些阶段即使理论上能包进去，也不建议当成“GPU 阶段耗时”展示：

- `begin_sync`
- `object_sync`
- `end_sync`
- `render_sync`
- `update_view`
- `sampling.step`
- `RenderBuffers.acquire`
- `GBuffer.acquire`
- `planar_probes.set_view`
- `render_read_result`
- `GPU_flush`
- `GPU_render_step`

原因不是它们“不重要”，而是：

- 它们不是独立的 GPU 绘制工作
- 很多时候测到的是等待和同步，不是实际渲染
- 把这些数据直接标成“GPU 开销”会误导分析

---

### 六、做性能工具时的推荐展示方式

为了避免误解，后面做性能工具时建议分开显示：

#### CPU 面板

显示：

- `Sync`
- `Capture`
- `MainView`
- `Readback`
- `Top CPU Stages`

#### GPU 面板

只显示真正的 pass：

- `Deferred`
- `Volume`
- `AO`
- `Forward`
- `Motion Blur`
- `DOF`
- `Filter`
- `Probe Capture`
- `Render Textures`

#### 单独标记的“非纯渲染阶段”

例如：

- `Shader Compile Wait`
- `Texture Loading`
- `Read Result`
- `GPU Flush / Render Step`

这些应该单独分类，不能和真正的绘制 pass 混在一起。

---

### 七、从实现角度的一句话总结

如果后面要实现 `Eevee Performance Profiler`，最合理的第一版是：

- 所有关键阶段都先做 `CPU` 计时
- 数据结构预留 `GPU` 时间字段
- 但只有真正的渲染 / 计算 pass 才在未来补 `GPU` 计时

这样既不会误导，也不会让第一版的实现复杂度失控。
