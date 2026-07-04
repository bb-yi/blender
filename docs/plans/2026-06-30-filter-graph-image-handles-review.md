# Filter Graph Image Handle 方案评审

评审对象:用户提交的《Filter Graph Image Handle 架构与实现计划》
评审日期:2026-06-30
评审基线代码:`blender_5_1_port` 当前 `feat/scene-color-handle-image-sample` HEAD
  - `eevee_filter_material.cc` (725 行,线性 stack)
  - `eevee_filter_material_frag.glsl` (TEX_HANDLE_NULL/RP_COLOR/RP_VALUE/SCENE)
  - `DNA_scene_types.h::SceneFilterMaterial` (ListBase 节点,含 execution_stage)
  - `eevee_view.cc` 4 处 `render_stage()` 调用点
  - release cases 已用到 913

---

## 一、总体判断

**核心架构是对的,可以推进;但计划在 5 个关键点上有真实风险,需要在动工前定下来,否则会在第三/四阶段返工。**

核心架构正确的部分:
1. **Image-handle DAG,不传颜色、不 per-input copy** —— 这是对的选择。每个 Filter Material invocation 把输入拷贝到 Texture2DArray 的方案在 N×M 规模下会爆,descriptor table 是唯一合理的路。
2. **Stage Output 节点对接现有 4 个 execution_stage** —— 与 `eevee_view.cc` 现有调用点干净对齐,不引入新 stage。
3. **阶段顺序 Data→Shader→Executor→Optimize→Legacy** —— 顺序正确,无法跳过数据模型先做执行器。
4. **fan-out cache + dead-node skip + cycle 检测** —— 优化集合正确,fan-out cache 尤其关键,否则共享上游子图会重复执行 N 次。

---

## 二、关键风险与问题(必须动工前解决)

### 风险 1:Filter Material 节点的"动态 socket 从 material 同步"是跨树耦合陷阱

**问题**:计划写"Filter Material 节点动态输入 socket 从该 material 的 Filter Graph Input 节点签名同步"。这意味着 Filter Graph 树里的 Filter Material 节点的 socket 结构,由另一棵树(Material 的 shader tree)里的 Filter Graph Input 节点决定。这是 Blender 已知的硬问题(类似旧版 group node socket sync,但是跨树)。

具体陷阱:
- 同步触发 `ntree.update` → 是否会再次触发同步?递归。
- undo/redo 时两棵树必须在同一个 undo step 内一致。
- Material 缺失/无效时,socket 是 stale 还是清空?
- socket 增删后 `ensure_topology_cache()` 必须重跑,否则下游 `directly_linked_links()` 拿到失效指针(本项目在 versioning 里已经因为这个崩过,见 MEMORY 记录)。
- 用户在 Filter Graph 里正在连一根线到 "Input 3",此时 Material 端删了 Input 3,链接悬挂。

**建议**:不要做"动态增删 socket"。改为以下任一:

- **方案 A(推荐)**:Filter Material 节点固定有 N 个 Image 输入 socket(默认 N=8,常量 `FILTER_GRAPH_INPUT_MAX`),全部隐藏。Material 的 Filter Graph Input 节点声明使用前 K 个,Graph executor 只绑定前 K 个,UI 根据 Material 声明显示前 K 个 socket 的名字。**只同步可见性和 label,不同步 socket 存在性**。这避开了所有跨树 socket mutation 问题。
- **方案 B**:Filter Material 节点只有一个 multi-input "Inputs" socket,每条 link 携带一个 input index(通过节点面板配置)。Graph executor 按拓扑读 link 顺序映射到 material 的 input 列表。完全无 socket 同步。

方案 A 更符合现有节点交互直觉,推荐。

### 风险 2:"保留 legacy stack + 新 graph executor"双路径会埋下长期维护负担

**问题**:计划写"有 Filter Graph Output 的 stage 使用 graph,未配置 graph 的 stage 继续走旧 stack"。这意味着 `FilterMaterialModule::render_stage()` 内部要分叉两条执行路径,而且 `entries_`(线性)和 graph 执行计划要共存。

更深的矛盾:第五阶段又说"把旧 filter stack 按 stage 和列表顺序转换为等价线性 Filter Graph"。如果最终要转换,为什么还要保留双路径?

**建议**:**统一执行器,删掉双路径**。具体:
- `begin_sync()` 时,如果 `scene->eevee.filter_graph == nullptr` 但 `filter_materials` 非空,**runtime 合成一个线性 graph**(不写 DNA,内存中临时构建:Scene Color → 每个按列表顺序的 Filter Material → Output(对应 stage))。
- `render_stage()` 永远走 graph executor,即使 graph 是合成的。
- 旧 `entries_` 路径删除,只保留 `FilterPassEntry` 结构用于 graph node 编译缓存。

好处:
1. 只有一套执行代码,只有一套优化(texture pool、dead-node skip),不会出现"graph 路径快但 legacy 路径慢"的体验分裂。
2. 不需要 .blend versioning 转换(本项目 versioning 已经因为迭代器失效崩过两次,见 MEMORY)。旧文件加载即合成,不污染 DNA。
3. 第五阶段从"转换"降级为"已自动完成",省一整个阶段。

代价:合成 graph 有微小开销(每帧 begin_sync 重建,可缓存)。可接受。

### 风险 3:Graph 中间纹理的格式与 extent 没定义清楚,与现有降采样优化冲突

**问题**:计划只说"中间纹理写入 texture pool",没回答:
- 中间纹理用什么格式?Scene Color 是 RGBA16F,但 Filter Material 可能输出只有 color 没有 alpha,或输出 mask(R8)。
- 中间纹理什么 extent?全分辨率?还是跟随上游 Filter Material 的内部降采样?

**与现有降采样优化的冲突**:MEMORY 记录 Filter Pass 降采样方向 A 已实现(`extent >> max_downsample_level` 运行 filter pass,blit 上采样回全分辨率)。在线性 stack 里,每个 entry 输出都 blit 回全分辨率,下游 entry 读全分辨率。**在 graph 里,如果 Filter Material A 内部降采样到 1/4,Filter Material B(下游)直接读 A 的 1/4 输出,可以省掉 blit** —— 这是 graph 相对线性 stack 的真实性能优势。但这也意味着:
- TextureHandle 需要携带 extent/level 信息。
- 下游采样必须知道源 extent,UV 映射要校正。
- Graph Output 边界才需要 blit 回 stage 全分辨率。

**建议**:
- **v1(第三阶段)**:Graph 内所有中间纹理强制全分辨率、统一 RGBA16F(或与 stage input 同格式)。禁止 graph 内 Filter Material 使用 Image Downsample 节点(检测到就报节点错误)。这一刀切让 v1 简单可验证。
- **v2(后续)**:支持 graph 内 per-node 降采样,TextureHandle 携带 level,graph-aware blit elision。这才是 graph 的性能卖点,但不要在 v1 做。

### 风险 4:TEX_HANDLE_FILTER_GRAPH_INPUT 的 GPU 绑定方式没定,直接关系能不能跑起来

**问题**:计划写"virtual handle descriptor table""不拷贝到 Texture2DArray",但没说具体绑定机制。三个候选:

| 方案 | 机制 | v1 可行性 |
|---|---|---|
| A. SSBO of TextureHandle + double dispatch | shader 读 SSBO 拿真实 handle,再走 TextureHandle_eval | 简单但每次采样多一次间接 dispatch,慢 |
| B. 32 个独立 sampler2D slot,shader switch on tex.index | bind 32 个 texture slot,unused 绑 1x1 null tex | 简单、跨后端、绑定开销小;但占 texture binding budget |
| C. descriptor indexing(sampler2D inputs[],nonuniform) | Vulkan `VK_EXT_descriptor_indexing` | 最优,但依赖后端支持,descriptor set churn |

**风险**:方案 C 是理想,但 Blender Vulkan 后端的 descriptor indexing 支持需要验证。方案 B 在 v1 最稳,但 32 个 slot 会撞 per-stage texture binding budget(Vulkan 通常 32-64,现有 filter pass 已经绑了 scene_color/depth/rp_color/rp_value/cryptomatte/utility/render_textures ~10 个)。

**建议**:
- **v1 用方案 B,`FILTER_GRAPH_INPUT_MAX = 8`(不是 32)**。8 个 slot + 现有 ~10 个 = 18,在所有后端安全。32 是 aspirational,留到 descriptor indexing 落地后再放宽。
- shader 端:`TextureHandle_eval` 对 `TEX_HANDLE_FILTER_GRAPH_INPUT` 走 `switch(tex.index) { case 0: return texture(filter_graph_input_0_tx, uv); ... }`,case 数量 = `FILTER_GRAPH_INPUT_MAX` 编译期常量。
- **GPUMaterial cache key 必须包含 `filter_graph_input_count`**?不包含 —— shader 编译一次,绑定 N 个 slot 是 runtime 的事。这是关键:同一个 Material 在不同 graph node 里被复用时,shader 只编译一次,只是 input binding 不同。
- v2 评估方案 C。

### 风险 5:AOV 在 graph 内的写入顺序歧义

**问题**:计划写"同名 AOV 写入按拓扑执行顺序确定"。但拓扑序对**互不连通**的节点是任意的。如果 graph 里 Filter Material A 和 B 互不连接,都写 AOV "X",拓扑序由 `all_nodes()` 迭代顺序决定(=节点创建顺序),这是**用户不可控、不可预测**的。

现有线性 stack 已经处理过这个问题:`FilterPassEntry::conflicting_aov_names` 收集冲突名,做 snapshot 隔离(见 `eevee_filter_material.cc:683-690`)。

**建议**:
- v1:**graph 内禁止多个 AOV 同名 writer**(graph 编译期检测,报节点错误,bypass graph)。这跟现有 `conflicting_aov_names` 逻辑一致。
- 跨 stage 的同名 AOV 允许(stage 间本身有时序)。
- snapshot 隔离机制(graph 内 AOV writer 之前 snapshot rp_color/rp_value)保留,但在 v1 因为禁止同名冲突,实际不会触发 —— 简化实现。

---

## 三、其他需要明确的点(计划没写)

1. **Scene Color graph 节点的语义**:它输出的是"stage 的输入图像"(= 上一个 stage 的输出 / `render_stage(input_tx)` 的 `input_tx`),还是"原始 combined scene color"?现有线性 stack 里 filter material 的 Scene Color = stage input(每个 entry 把上一个 entry 的输出当 scene_color_tx 绑定)。**graph 里应保持一致:Scene Color graph 节点 = stage input**。Depth/Normal/Position 仍来自 `inst_.render_buffers`,不是 stage input。这个区别要在文档里写清楚,否则用户会困惑"为什么 Scene Color 在 BEFORE_POSTFX stage 看到的是 volume fog 后的颜色"。

2. **Filter Graph 是否支持节点组(group)**?计划没提。建议 **v1 不支持** graph 内节点组(明确报错),v2 再加。否则拓扑分析、cycle 检测、socket 透传都要处理 group 展开爆炸复杂度。

3. **Mute 一个 graph 节点**:Filter Material 节点 mute 后行为?建议 = pass-through(选一个 input 透传,默认第一个)。Output 节点 mute = 整个 graph bypass,走合成线性 graph(见风险 2)。Scene Color / AOV Input 节点 mute = 输出透明黑。

4. **多 Filter Material 输出汇入一个 Output**:计划写 Output 接收"stage 最终 Image"(单数)。那用户要在输出前 blend 两个 Filter Material 结果怎么办?**v1 强制 blend 必须在某个 Filter Material 内部完成**(即 graph 是树,不是 DAG-fanin-then-blend)。如果允许 Output 多输入,得定义 blend 算子(over/add/multiply?),v1 别开这个口子。

5. **depsgraph / GPUMaterial 失效**:Material 的 Filter Graph Input 节点数量变化时,Material 的 shader 不需要重编译(见风险 4,shader 与 input count 解耦)。但 **Filter Graph 树本身变化时,要标记 graph 执行计划 dirty**。`begin_sync()` 每帧重建执行计划即可,开销可接受(graph 节点数 << 100)。

6. **节点错误报告通道**:cycle、无效 material、非 FILTER material、未连 Output、同名 AOV 冲突 —— 这些错误怎么报给用户?建议复用现有 `node->flag` 的错误机制(类似 `NODE_MUTED` 旁加一个 `NODE_ERROR` 或复用 `info`/`warning` 系统),并在 Scene 面板显示 graph 校验摘要。**不要只在 console 打印** —— 用户看不到。

---

## 四、测试计划补充

现有测试计划覆盖了功能正确性,缺以下三类:

1. **性能回归断言**:MEMORY 记录 Filter Pass 降采样优化"代码完整但可能没生效"。Graph executor 是大重构,必须加帧时间预算断言(例如"4096 采样场景 graph 路径 vs 合成线性 graph 路径,帧时间差 < 10%")。否则"能跑"但比线性 stack 慢一倍没人发现。

2. **同 Material 多 invocation 的 input table 隔离断言要具体化**:不能只写"不串线"。应该是:构造 graph `Scene Color → FilterMatA(input0=SceneColor) → Out` 和 `Scene Color → FilterMatA(input0=Depth) → Out`,两个 graph 分别渲染得到基准图;再构造 `FilterMatA(input0=SceneColor) → FilterMatA(input0=Depth) → Out`,断言第二张 FilterMatA 的输出 == 第二个单独 graph 的输出(像素 diff < epsilon)。这才能真正抓到 input table 串线 bug。

3. **Release case 编号**:现有最高 913。新 case 应为 `914-filter-graph-image-handle-merge`,不是 `<number>`。

4. **缺一个"graph bypass 回退"测试**:构造一个故意有 cycle 的 graph,断言渲染不崩溃且输出 == 无 graph 时的合成线性 graph 输出(即 graph bypass 后等价于 legacy)。这是风险 2 统一执行器的关键保障。

---

## 五、修订后的阶段建议

基于以上,建议把计划阶段调整为:

| 阶段 | 原计划 | 修订建议 |
|---|---|---|
| 一 | DNA/RNA/节点树注册/UI/保存重载 | 不变。但 **Filter Material 节点用固定 8 隐藏 socket 方案(风险 1 方案 A)**,不要做动态 socket 同步。 |
| 二 | Filter Graph Input 动态 sockets + TEX_HANDLE_FILTER_GRAPH_INPUT + descriptor table | **Filter Graph Input 节点声明 1..8 个命名输出**(用户填名字列表),不做"动态 socket 数量"。GPU 端用方案 B(8 个独立 sampler slot,风险 4)。 |
| 三 | 接入 graph executor,无 graph output 走 legacy | **统一执行器(风险 2)**:无 `filter_graph` 时 begin_sync 合成线性 graph,render_stage 永远走 executor,删掉 `entries_` 双路径。中间纹理全分辨率统一格式(风险 3 v1)。 |
| 四 | texture pool/lifetime/dead-node/fan-out/cycle 报告 | 不变。cycle 检测放 `begin_sync` 编译期(不放 runtime)。AOV 同名冲突直接报错(风险 5)。 |
| 五 | legacy stack → graph 转换 | **删除此阶段**(风险 2 已在阶段三通过合成 graph 解决)。改为"性能回归测试 + 4096 采样压测 + graph bypass 测试"。 |

净效果:省一个阶段,少一套执行路径,少一类 versioning bug,少一类跨树 socket mutation bug。

---

## 六、可立即动工的部分(零风险)

以下不依赖上述争议点的决策,可以先做:

1. `EeveeFilterGraphNodeTree` RNA 注册(`NTreeType` 定义、`ntreeTypeAdd`、ID 遍历 `foreach_id`、blend read/write、copy/free)。
2. DNA:`SceneEEVEE::filter_graph`(bNodeTree * 指针)字段 + versioning(null 初始化)。
3. Scene 面板"Filter Graph"展开入口(打开节点编辑器到对应树)。
4. `test/release/cases/914-filter-graph-image-handle-merge/` 的 `case.json` / `README.md` 骨架(先写测试语义,后填 `run.py`)。

这 4 项在风险 1-5 的决策落地前就可以推进,不阻塞。

---

## 七、结论

**方案可以推进,核心架构是对的。** 但动工前必须先定 5 件事:
1. Filter Material 节点 socket:固定 8 隐藏 slot,不做动态同步。
2. 执行器统一:无 graph 时合成线性 graph,删双路径。
3. v1 中间纹理:全分辨率、统一格式,graph 内禁降采样。
4. GPU 绑定:方案 B + `FILTER_GRAPH_INPUT_MAX = 8`。
5. AOV:graph 内同名 writer 直接报错。

这 5 件事定下来后,阶段三/四不会返工;不定就定下来,会在执行器写到一半时撞墙。
