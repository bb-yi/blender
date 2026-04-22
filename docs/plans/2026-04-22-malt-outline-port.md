# BlenderMalt 描边方案移植到 EEVEE 5.1 NPR

## Context

BlenderMalt 插件实现了一套完整的 NPR 描边系统，基于屏幕空间后处理方案。当前 EEVEE 5.1 NPR 版本已有 NPRModule、G-buffer、Filter Materials 等基础设施，但**没有专用的描边 Pass**。目标是将 Malt 的描边核心算法移植到 EEVEE 的延迟渲染管线中。

---

## BlenderMalt 描边方案总结

### 核心架构：三阶段屏幕空间后处理

**阶段1 — G-buffer 预渲染 (PrePass)**
- 输出 `t_normal_depth` (RGBA32F: xyz=世界法线, w=深度)
- 输出 `t_id` (RGBA16UI: 物体ID + 3个自定义ID通道)

**阶段2 — 线条检测 + 宽度计算 (MainPass 材质输出)**
- 每个材质的 Pixel Shader 输出 `Line Color` (vec4) 和 `Line Width` (float)
- 核心检测函数 `line_detection_2()` (Line.glsl:154-251)：
  - 十字采样4邻域像素
  - **深度不连续**：平面距离差 / `pixel_world_size_at(depth)`，距离稳定
  - **法线不连续**：`dot(normal, sampled_normal)` 折痕检测
  - **ID边界**：物体/自定义ID不同则标记边界
  - 深度模式：NEAR(轮廓线)、FAR(内边缘)、ANY(两者)
- 宽度计算 `line_width_2()` (NPR_Filters.glsl)：
  - 三种宽度单位：Pixel / Screen(随分辨率缩放) / World(世界空间恒定)

**阶段3 — 线条膨胀 + 合成 (LineRender)**
- `line_expand()` (Line.glsl:264-339)：暴力搜索 max_width×max_width 邻域
  - 亚像素AA：`clamp(offset_width/2.0 - offset_length, 0, 1)`
  - 深度排序：近处线条覆盖远处
  - Alpha混合到颜色缓冲

**附加功能：**
- 曲率线检测 (Curvature.glsl)
- JumpFlood 宽线优化 (JumpFlood.glsl) — O(log n) 替代暴力搜索

---

## EEVEE 5.1 NPR 现有基础设施

- `NPRModule` (eevee_npr.cc/hh) — 已集成到 Instance
- NPR G-buffer：法线(slot 6)、header(slot 7)、closure(slot 8)、radiance(slot 9)
- NPR Pass 在延迟光照之后运行，可访问深度/法线/位置/颜色
- `Image Sample` 节点支持像素偏移采样（可做邻域读取）
- Filter Materials 系统可做全屏后处理
- **无专用描边 Pass**

### Object ID 现状

`object_id` 已存在于 G-buffer 中，通过 `gbuffer_additional_info_pack()` 打包在 normal 层的最后一层 rg 通道（r=thickness, g=object_id），以 unorm16 格式存储。`eevee_surf_deferred_frag.glsl:106` 写入 `gbuf_data.object_id = resource_id`。

`resource_id` 是 draw manager 每帧分配的 draw call 索引，帧间不稳定（物体增删时会变），但**描边只需要同一帧内相邻像素 ID 是否相同**，不需要跨帧一致。Subsurface 模块也用同一个 `resource_id`。

但 G-buffer 在 Filter Materials 运行前已 release。Subsurface 模块遇到同样问题时自己维护了独立的 `object_id_tx_` (GPU_R16UI)。

**精度注意**：G-buffer 中使用 `gbuffer_object_id_unorm16_pack()`（`float(id & 0xFFFF) / 65535.0`）可以精确表示 0-65535 的 ID。但如果直接用 half float 存 `float(id)`，只能正确表示到 1024（`gbuffer_object_id_f16_pack()` 的注释明确说明了这一点）。

---

## 移植方案（最终版）：NPR Outline 输出 + 内置管线 Pass

### 核心思路

- **NPR 材质端**：新增 `Outline Control` 独立副作用节点（类似 AOV Output），不需要连接到任何 Output 节点。任何材质类型（NPR/BSDF/Principled）的节点树中都可以放置。节点提供所有描边参数（颜色、宽度、深度阈值、法线阈值、自定义 Outline ID），全部逐材质逐像素。
- **管线端**：surface pass（deferred + NPR）将描边参数写入 2 个专用纹理（不占 AOV）。NPR pass 之后、Filter Materials 之前，自动执行内置的两步描边 pass（detect → expand）。用户不需要手动创建 Filter Material。
- **Outline ID**：用户可设置自定义 Outline ID（整数），相同 ID 的物体之间不产生 ID 边界。默认值 0 表示自动使用 `resource_id`（每个物体自动不同）。写入时 `outline_id = (custom_id > 0) ? custom_id : resource_id + 1`（+1 避免与 clear 值 0 冲突）。以 unorm16 编码写入专用纹理。

### 数据流

```
任意材质节点树 (NPR / BSDF / Principled)
┌──────────────────────────────────┐
│                                  │
│  Outline Control 节点 (独立副作用) │
│  ┌────────────────────────┐      │
│  │ Line Color    [黑色]    │      │
│  │ Line Width    [2.0]    │      │
│  │ Depth Thresh  [0.1]    │      │
│  │ Normal Thresh [0.5]    │      │
│  │ Outline ID    [0]      │      │
│  └────────────────────────┘      │
│  (无需连接到 Output 节点)         │
│                                  │
│  Material Output / NPR Output    │
│  (正常着色逻辑不受影响)            │
└──────────────────────────────────┘
        │
        │ surface pass 写入专用纹理
        ▼
┌─────────────────────────────────────┐
│ outline_params_tx[0] (RGBA16F):     │
│   RGBA = line_color (含 alpha)      │
│ outline_params_tx[1] (RGBA16F):     │
│   R = line_width, G = depth_thresh  │
│   B = normal_thresh, A = outline_id │
└─────────────────────────────────────┘
        │
        │ 内置管线 Pass (自动触发)
        ▼
┌─────────────────────────────────┐
│ Pass 1: Detect (边缘检测)        │
│  读取: depth_tx, outline_params  │
│  执行: line_detection_2()        │
│  输出: edge_seed_tx (RGBA16F)    │
│    RGB=line_color, A=line_width  │
│    (color alpha 从 params_tx[0]  │
│     延迟到 expand 阶段读取)       │
│                                 │
│ Pass 2: Expand (线条膨胀)        │
│  读取: edge_seed_tx, depth_tx,  │
│        outline_params_tx[0]     │
│  执行: line_expand()             │
│  合成: color.a *= params_tx[0].a │
│  输出: alpha-over 到 combined    │
└─────────────────────────────────┘
```

### 优势

1. **用户体验最好** — 材质里放一个 Outline Control 节点即可，不需要手动建 Filter Material
2. **逐材质完整控制** — 颜色、宽度、阈值、自定义 ID 全部逐材质逐像素
3. **不占 AOV** — 专用纹理，不影响用户的 AOV 配置
4. **自定义 ID 灵活** — Outline ID=0 自动使用 resource_id；相同 ID 的物体之间不产生边界
5. **两步法性能可控** — detect 和 expand 是独立的 fullscreen pass，与 Malt 原架构一致
6. **自动触发** — 检测到场景中有材质使用了 Outline 输出，自动执行描边 pass

### 需要新增的内容

#### 1. NPR Output 节点扩展 + Outline Control 节点

**NPR Output 节点**新增 `Outline` 输入 socket（类型：Shader/Closure，或自定义 struct）。

**Outline Control 节点**（新建）：
- 输入：Line Color (vec4), Line Width (float), Depth Threshold (float), Normal Threshold (float), Outline ID (int, 默认0=自动使用resource_id)
- 输出：Outline (连接到 NPR Output)
- GPU 代码生成：将参数打包写入 `outline_params_img[0]` 和 `outline_params_img[1]`

**改动文件：**
- 新建 `node_shader_outline_control.cc` — Outline Control 节点定义
- 新建 `gpu_shader_material_outline_control.glsl` — 参数打包写入
- 修改 `node_shader_output_npr.cc` — 添加 Outline 输入 socket
- 修改 `gpu_shader_material_output_npr.glsl` — 处理 Outline 输入
- `NOD_static_types.h` — 注册节点类型
- `node_shader_register.cc` — 注册

#### 2. 专用描边纹理

新建两个全屏纹理，每帧开始时 clear（line_width=0 表示该像素不参与描边）：
- `outline_params_tx_[0]` (RGBA16F)：RGBA = line_color（含 alpha 透明度），默认 clear 为 (0,0,0,0)
- `outline_params_tx_[1]` (RGBA16F)：R = line_width, G = depth_thresh, B = normal_thresh, A = outline_id (unorm16 编码，0=自动使用 resource_id)，默认 clear 为 (0,0,0,0)

**写入时机**：Outline Control 节点在 surface pass 中通过 `imageStore` 写入。需要在两个 pass 中都绑定 outline_params_img：
- `eevee_surf_deferred_frag.glsl`（普通 deferred pass）— 非 NPR 材质的 Outline Control 写入
- `eevee_surf_deferred_npr_frag.glsl`（NPR pass）— NPR 材质的 Outline Control 写入
- 使用 image slot 6、7（当前 deferred pass 用到 slot 5，OpenGL 4.2 最低保证 8 个）

**改动文件：**
- `eevee_renderbuffers.hh/cc` — 添加 `outline_params_tx_[2]`，每帧 clear
- `eevee_pipeline.cc` — deferred pass 和 NPR pass 都绑定 `outline_params_img`
- `eevee_surf_deferred_frag.glsl` — 普通 deferred pass 支持 Outline Control 写入
- `eevee_surf_deferred_npr_frag.glsl` — NPR pass 支持 Outline Control 写入 + outline_id
- `shaders/infos/eevee_deferred_info.hh` — ShaderCreateInfo 添加 image binding (slot 6, 7)

#### 3. 内置两步描边 Pass

插入点：`eevee_view.cc` 中 `deferred.render()` 返回后、`gbuffer.release()` 之前。法线纹理在此时仍然有效（`gbuffer.release()` 中 `normal_tx.release()` 当前被注释掉）。

**Pass 1: Outline Detect**
- 读取：`depth_tx`, `gbuf.normal_tx`, `outline_params_tx_[0]`, `outline_params_tx_[1]`
- 执行：`line_detection_2()` — 深度/法线/ID 边缘检测
- 输出：`edge_seed_tx` (RGBA16F) — RGB = line_color.rgb, A = line_width（仅边缘像素有值）
- 注意：line_color.alpha 不存入 edge_seed，留到 expand 阶段从 outline_params_tx[0].a 读取

**Pass 2: Outline Expand**
- 读取：`edge_seed_tx`, `outline_params_tx_[0]`（读 color alpha）, `depth_tx`, combined buffer
- 执行：`line_expand()` — 邻域搜索 + 膨胀 + AA + 深度排序
- 合成：最终 alpha = expand_alpha × outline_params_tx[0].a，alpha-over 到 combined buffer

**改动文件：**
- 新建 `eevee_outline.hh/cc` — OutlineModule 类（轻量，参考 SubsurfaceModule 模式）
- 新建 `eevee_outline_detect_frag.glsl` — 移植 `line_detection_2()`
- 新建 `eevee_outline_expand_frag.glsl` — 移植 `line_expand()`
- 新建 `shaders/infos/eevee_outline_info.hh` — ShaderCreateInfo
- `eevee_instance.hh/cc` — 添加 `OutlineModule outline` 成员
- `eevee_view.cc` — 渲染流程中插入 `outline.render()`
- `eevee_shader.hh/cc` — 注册 Shader 枚举
- `CMakeLists.txt` — 添加文件

#### 4. 自动触发机制

`OutlineModule::init()` 中扫描场景材质，检测是否有 NPR 材质使用了 Outline 输出。如果没有，跳过描边 pass（零开销）。

类似 `FilterMaterialModule::init()` 扫描 `SH_NODE_SCENE_COLOR` 的做法。

---

## 分阶段实施

### Phase 1：专用纹理 + NPR Outline 输出（4-6 天）

| 步骤 | 内容 | 改动量 |
|------|------|--------|
| 1.1 | `eevee_renderbuffers.hh/cc` 添加 `outline_params_tx_[2]`，每帧 clear (line_width=0) | 小 |
| 1.2 | 新建 `node_shader_outline_control.cc` + GLSL — Outline Control 节点（参考 AOV Output 的 `GPU_NODE_TAG` 防剪枝机制） | 中 |
| 1.3 | 修改 `node_shader_output_npr.cc` — NPR Output 添加 Outline 输入 | 小 |
| 1.4 | `eevee_pipeline.cc` — deferred pass 和 NPR pass 都绑定 `outline_params_img` (slot 6, 7) | 中 |
| 1.5 | `eevee_surf_deferred_frag.glsl` + `eevee_surf_deferred_npr_frag.glsl` — 两个 pass 都支持 Outline Control 写入，outline_id=0 时自动使用 resource_id | 中 |
| 1.6 | 验证：可视化 outline_params_tx 确认颜色/宽度/阈值/ID 数据正确 | 测试 |

### Phase 2：内置两步描边 Pass（6-8 天）

| 步骤 | 内容 | 改动量 |
|------|------|--------|
| 2.1 | 新建 `eevee_outline.hh/cc` — OutlineModule 类 | 中 |
| 2.2 | 新建 `eevee_outline_detect_frag.glsl` — 移植 `line_detection_2()`，读取 depth_tx + gbuf.normal_tx + outline_params | 中 |
| 2.3 | 新建 `eevee_outline_expand_frag.glsl` — 移植 `line_expand()`，从 outline_params_tx[0].a 读取 color alpha | 中 |
| 2.4 | 新建 `eevee_outline_info.hh` — ShaderCreateInfo | 小 |
| 2.5 | `eevee_instance.hh/cc` + `eevee_view.cc` — 注册模块，插入点在 `deferred.render()` 之后、`gbuffer.release()` 之前 | 小 |
| 2.6 | `eevee_shader.hh/cc` + `CMakeLists.txt` — 注册 Shader | 小 |
| 2.7 | 自动触发机制 — 扫描材质检测 Outline Control 节点使用（参考 FilterMaterialModule 扫描 SH_NODE_SCENE_COLOR 的做法） | 小 |
| 2.8 | 测试：简单场景验证轮廓线效果，检查深度排序、AA、Outline ID 边界 | 测试 |

Phase 1+2 = MVP，完整可用的描边系统。

### Phase 3：性能优化 — JumpFlood（3-5 天）

- Outline Expand pass 内部优化：max_width <= 10 用暴力法，> 10 切换 JumpFlood
- Compute Shader 实现，ping-pong 纹理

---

## 难度评估总结（最终版）

| 阶段 | 难度 | 工时 | 依赖 |
|------|------|------|------|
| Phase 1: 专用纹理 + Outline 节点 | ⭐⭐ 中等 | 4-6天 | 无 |
| Phase 2: 内置两步描边 Pass | ⭐⭐⭐ 中高 | 6-8天 | Phase 1 |
| Phase 3: JumpFlood | ⭐⭐ 中等 | 3-5天 | Phase 2 |
| **总计** | | **13-19天** | |

**整体难度：中等偏高。** 比纯 Filter Material 方案多了 OutlineModule 和节点系统改动，但用户体验大幅提升，且避免了 edge seed 编码的 hack。

---

## 与其他方案对比

| | 原方案 (OutlineModule) | AOV + Filter Material | 最终方案 (Outline 输出 + 内置 Pass) |
|---|---|---|---|
| C++ 新增文件 | 5+ | 4 | 6 (module + 节点 + shaders) |
| 管线改动 | 大 | 极小 | 中（新模块 + 2个 fullscreen pass） |
| 逐材质控制 | Phase 3 才有 | Phase 1 就有 | Phase 1 就有 |
| 阈值控制 | 全局 | 全局 | 逐材质逐像素 |
| 用户操作 | 面板调参 | NPR 连 AOV + 建2个 Filter | NPR 连 Outline Control 节点 |
| Object ID 精度 | R16UI 完美 | R16F 需 unorm16 | RGBA16F A通道 unorm16 自定义ID |
| Edge Seed 传递 | 专用纹理 | alpha 通道 hack | 专用 edge_seed_tx |
| 总工时 | 23-34 天 | 11-16 天 | 13-19 天 |

---

## 主要风险

1. **膨胀 Pass 性能（最大瓶颈）**：`line_expand()` 的 O(n²) 邻域搜索，每像素 fetch 数 = (max_width+1)² × 4 张纹理。max_width=10 时 1080p 约 1B fetch，max_width=20 时 4K 约 14.6B fetch。Phase 3 的 JumpFlood 在 max_width > 5 时是必要优化
2. **Expand Pass 无 early-out**：远离边缘的像素仍做完整搜索。优化方案：tile-based 预判（8×8 tile 标记有无 edge seed），无边缘 tile 直接跳过
3. **resource_id = 0 冲突**：`resource_id` 从 0 开始分配，outline_id=0 时写入 resource_id 可能为 0，与 clear 值冲突。解决：写入 `resource_id + 1`，保留 0 给 "无描边" 像素
4. **NPR Output 节点改动**：新增 Outline socket 需要修改节点定义和 GPU 代码生成，需要理解 Blender 节点系统的 socket 注册机制
5. **法线纹理生命周期**：detect pass 依赖 `gbuf.normal_tx`，当前 `gbuffer.release()` 中 `normal_tx.release()` 被注释掉所以安全。但如果上游修复该 TODO，法线会被释放。备选方案：detect pass 从深度重建法线
6. **Outline Control 节点的 GPU 代码生成**：需要确保参数能正确传递到 `imageStore` 调用。参考 AOV Output 的实现模式（`GPU_material_add_output_link_aov()` + `GPU_NODE_TAG_AOV`），需要新增 `GPU_NODE_TAG_OUTLINE` 标记防止节点被剪枝
7. **Image binding slot 数量**：deferred pass 新增 2 个 image slot (6, 7)，总共 7 个。OpenGL 4.2 最低保证 8 个，刚好在限制内。需要运行时检查 `GPU_max_images()` 确认
8. **显存占用**：3 张 RGBA16F 全屏纹理，1080p 约 48MB，4K 约 192MB。对 NPR 渲染可接受

## 性能优化路线

| 优化 | 阶段 | 效果 |
|------|------|------|
| Detect pass: `if (line_width == 0) discard` | Phase 2 | 跳过无描边像素的检测 |
| Expand pass: tile-based early-out | Phase 2 | 跳过远离边缘的像素 |
| JumpFlood 替代暴力搜索 | Phase 3 | max_width > 5 时从 O(n²) 降到 O(log n) |

## 验证方式

- Phase 1: 可视化 outline_params_tx[0] 和 [1]，确认颜色/宽度/阈值/ID 数据正确
- Phase 2: 简单场景（立方体+球体+不同材质）验证轮廓线效果，检查：
  - 深度不连续边缘（物体轮廓）
  - 法线不连续边缘（硬边/折痕）
  - Outline ID 边界（不同 ID 物体交界有边界，相同 ID 无边界）
  - 深度排序（近处线条覆盖远处）
  - 亚像素 AA
  - 半透明描边（line_color.alpha < 1）
  - 非 NPR 材质（Principled BSDF + Outline Control 节点）验证普通 deferred pass 写入
  - 无 Outline Control 节点的材质不产生描边（clear 机制验证）
- Phase 3: max_width=20 对比暴力法 vs JumpFlood 帧时间

## 关键源文件

**Malt 源码（移植参考）：**
- `malt/BlenderMalt/.MaltPath/Malt/Shaders/Filters/Line.glsl` — line_detection_2() + line_expand()
- `malt/BlenderMalt/.MaltPath/Malt/Pipelines/NPR_Pipeline/Shaders/NPR_Pipeline/NPR_Filters.glsl` — line_width_2()

**EEVEE 改动目标：**
- `blender/source/blender/draw/engines/eevee_next/eevee_renderbuffers.hh/cc` — 专用纹理
- `blender/source/blender/draw/engines/eevee_next/eevee_pipeline.cc` — deferred + NPR pass 绑定
- `blender/source/blender/draw/engines/eevee_next/eevee_view.cc` — 渲染流程插入
- `blender/source/blender/draw/engines/eevee_next/eevee_instance.hh/cc` — 模块注册
- `blender/source/blender/draw/engines/eevee_next/eevee_subsurface.cc` — OutlineModule 参考模式
- `blender/source/blender/draw/engines/eevee_next/shaders/eevee_surf_deferred_frag.glsl` — 普通 deferred 片段着色器（需绑定 outline_params_img）
- `blender/source/blender/draw/engines/eevee_next/shaders/eevee_surf_deferred_npr_frag.glsl` — NPR 片段着色器
- `blender/source/blender/draw/engines/eevee_next/shaders/eevee_gbuffer_lib.glsl` — object_id 编码参考
- `blender/source/blender/draw/engines/eevee_next/eevee_gbuffer.hh` — gbuffer release 生命周期
- `blender/source/blender/nodes/shader/nodes/node_shader_output_npr.cc` — NPR Output 节点

**节点系统参考（AOV Output 模式）：**
- `blender/source/blender/nodes/shader/nodes/node_shader_output_aov.cc` — 独立副作用节点参考
- `blender/source/blender/gpu/shaders/material/gpu_shader_material_output_aov.glsl` — AOV 写入 GLSL
- `blender/source/blender/gpu/intern/gpu_node_graph.cc` — GPU_NODE_TAG_AOV 防剪枝机制
- `blender/source/blender/draw/engines/eevee_next/shaders/eevee_renderpass_lib.glsl` — imageStoreFast 参考
