# Eevee Color Bake 实现计划

**目标：** 在 Eevee 中补齐现有 Blender Bake 工作流的本地材质颜色烘焙能力。V1 只做 Eevee-only `EMIT` / Color Bake，不追求 Cycles 全 bake 类型一一对应。

**架构：** 复用 Blender 通用 Bake API 负责目标图像发现、UV 像素覆盖、有效像素 mask、margin 扩展和最终图像输出；新增 Eevee bake 回调，在 UV 空间把 mesh 三角形 rasterize 到目标图，同时执行真实 Eevee 材质 GPU 路径，允许受支持的本地 NPR 颜色逻辑和 Eevee light/shadow/lightprobe 资源参与。

**技术栈：** C++、Blender `RenderEngineType.bake`、Draw Manager custom pipeline、Eevee material pipeline、GLSL create-info、Python UI、release-case 后台验证。

---

## Summary

- V1 实现为 Eevee `EMIT` / Color Bake pass，而不是 Cycles bake 的完整复制。
- 公开入口继续使用现有 operator：`bpy.ops.object.bake(type='EMIT')`。
- Eevee 注册 `RenderEngineType.bake` 回调；非 V1 支持的 bake type 必须明确报错。
- 烘焙方式是 UV 空间 rasterization，不做 CPU Base Color 假烘焙，也不读取相机最终画面。
- 继续使用通用 Bake API 的 image target、多 image、UV island、margin 和输出逻辑，降低对现有系统的侵入。

## 支持范围

- Render engine：只支持 `BLENDER_EEVEE`。
- Bake type：只支持 `SCE_PASS_EMIT`，在 Eevee 中解释为 Color Bake。
- Target：只支持 `scene.render.bake` 的 image texture target，遵循现有 active image per material 规则。
- Geometry：只支持 mesh object 自身烘焙。
- 材质：
  - 普通 Eevee surface material。
  - Emission / 本地材质颜色输出。
  - Eevee surface shader 中可正常编译的 texture、UV、attribute 等本地节点。
  - `Shader Info`。
  - `GLSL Function` light helper。
  - 不依赖屏幕/GBuffer 输入的本地 NPR Tree 颜色处理。
- 输出：
  - 通过现有 `Combined` bake result 输出 scene-linear RGBA float。
  - 支持多材质、多目标 image。
  - 支持现有有效像素 mask 和 margin 扩展。

## 不支持范围

- 所有非 `EMIT` bake type，包括 `COMBINED`、Diffuse、Glossy、Transmission、AO、Normal、Position、Roughness、UV 和 pass filters。
- Selected-to-active、cage、multires、高低模 ray projection，以及 Cycles 风格真实 ray bake 语义。
- 非 image target、vertex color target、volume、curve、point cloud、world、Filter-domain material。
- `NPR Input` 屏幕/GBuffer 读取、`NPR Refraction`、Input AOV、Screen Space Info、Scene Color、Render Texture feedback、back-buffer 读取和最终视图后处理。
- 透明排序一致性、最终相机可见色匹配。
- Camera / Window / Screen 坐标在 V1 bake 中不保证有有意义的结果。

遇到不支持场景必须在输出图像前明确失败。黑图、半成品图或默认材质 fallback 都不算可接受结果。

## 外部实现参考

实时渲染器常见的材质烘焙模型是：

- 把 UV 展开的三角形绘制到 render target。
- 对每个 fragment 执行真实材质 shader。
- 把 render target 读回目标贴图。

Unity RenderTexture material blit、Unreal glTF exporter 的 material baking、Substance mesh-map baking 都符合这个方向。Blender Eevee 这里不能简单画 fullscreen quad，因为材质求值还需要原始 mesh position、normal、UV、attribute 以及 Eevee light/shadow/lightprobe 资源。

## 实现任务

### Task 1: 增加 Eevee Bake 入口

**文件：**
- 修改：`source/blender/draw/engines/eevee/eevee_engine.cc`
- 新增：`source/blender/draw/engines/eevee/eevee_bake.hh`
- 新增：`source/blender/draw/engines/eevee/eevee_bake.cc`
- 修改：`source/blender/draw/CMakeLists.txt`

**要求：**
- 在 Eevee `RenderEngineType` 中注册 `eevee_bake`。
- 只接受 `SCE_PASS_EMIT`。
- 对当前 `BakeImage` 尺寸和 layer name 调用 `RE_engine_begin_result()`。
- 把 Eevee bake 结果写入 `Combined` pass。
- 用 `RE_engine_end_result()` 结束，让 `render_result_to_bake()` 把像素复制到 `BakeTargets.result`。
- 不支持输入使用 `RE_engine_set_error_message()` 和 report 明确失败。

**注意：**
- 通用 Bake API 已经创建 `BakePrimitive` 和 `BakeDifferential` pass，不要重写 target image 和 margin 系统。
- bake 回调会按 `BakeImage` 调用；使用 `engine->bake.image_id` 和 `engine->bake.targets->images[image_id]`。
- bake 回调不能调用普通相机 `render_frame()` 路径。

### Task 2: 增加 Eevee 材质 Bake Pipeline

**文件：**
- 修改：`source/blender/draw/engines/eevee/eevee_material_shared.hh`
- 修改：`source/blender/draw/engines/eevee/eevee_material.hh`
- 修改：`source/blender/draw/engines/eevee/eevee_material.cc`
- 修改：`source/blender/draw/engines/eevee/eevee_shader.cc`
- 修改：`source/blender/draw/engines/eevee/eevee_shader.hh`
- 修改：`source/blender/draw/engines/eevee/eevee_pipeline.hh`
- 修改：`source/blender/draw/engines/eevee/eevee_pipeline.cc`

**要求：**
- 新增普通本地颜色输出的 bake material pipeline。
- 新增本地 `nodetree_npr()` 输出的 bake NPR material pipeline。
- bake pipeline 必须同步编译，或等待 queued shader 编译完成后再继续。
- bake 期间不能使用 default/error material 作为临时 fallback，否则会静默输出错误贴图。
- 按材质需求绑定 Eevee 资源：
  - material shader 必需的 global UBO 和 utility texture。
  - `Shader Info` 和 GLSL light helper 需要的 `eevee_light_data`。
  - shadow-aware helper 需要的 `eevee_shadow_data`。
  - Shader Info / ambient helper 需要的 `eevee_lightprobe_data`。

**注意：**
- 复用现有材质资源和 sampler reserved-slot 规则，不新增临时 sampler slot。
- 扩展 `material_texture_reserved_slot_last()` 和 create-info amend 逻辑以覆盖 bake pipeline。
- shader UUID packing 必须包含新 pipeline，不能和现有 pipeline alias。
- Filter-domain material 应直接报不支持，不能返回空 pass 或默认 pass。

### Task 3: 在 UV 空间绘制 Mesh 三角形

**文件：**
- 新增：`source/blender/draw/engines/eevee/shaders/eevee_geom_mesh_bake_vert.glsl`
- 新增：`source/blender/draw/engines/eevee/shaders/eevee_surf_bake_frag.glsl`
- 新增：`source/blender/draw/engines/eevee/shaders/eevee_surf_bake_npr_frag.glsl`
- 新增：`source/blender/draw/engines/eevee/shaders/infos/eevee_surf_bake_infos.hh`
- 修改：`source/blender/draw/engines/eevee/shaders/infos/eevee_material_infos.hh`
- 修改：`source/blender/draw/engines/eevee/shaders/CMakeLists.txt`
- 修改：`source/blender/draw/CMakeLists.txt`

**要求：**
- vertex shader 使用 bake UV 写 clip-space position。
- 材质求值仍使用原始 mesh surface 数据：
  - object/world position
  - normal 和 true normal
  - UV 和 custom attributes
  - 可用时的 tangent data
- fragment shader 输出 scene-linear RGBA 到 offscreen color target，再拷贝进 bake `Combined` pass。
- 每次只绘制 material slot 映射到当前 `BakeImage` 的三角形。

**注意：**
- 不能把 edit-UV batch 当作最终方案。它把 active UV alias 成 `pos`，但不足以保留 world-space 节点和法线相关节点需要的原始 surface context。
- 优先做专用 bake vertex input 或轻量 bake batch，同时保留 UV position 和原始 surface attributes。
- 继续依赖 rasterization derivatives，使 texture filtering、bump、normal map 更接近 Eevee 材质渲染。

### Task 4: 接入 DRW Custom Pipeline

**文件：**
- 修改：`source/blender/draw/engines/eevee/eevee_bake.cc`
- 修改：`source/blender/draw/engines/eevee/eevee_instance.hh`
- 修改：`source/blender/draw/engines/eevee/eevee_instance.cc`

**要求：**
- 后台 bake 必须进入有效 GPU / Draw Manager context。
- 参考 light bake 模式：`DRWContext::CUSTOM`、`DRW_custom_pipeline_begin()`、sync/render、`DRW_custom_pipeline_end()`。
- 绘制前同步 scene lights、shadows、light probes、材质资源和目标物体。
- 等待 queued material shader / texture 编译完成后重新 sync，再执行 bake draw。

**注意：**
- `RE_bake_engine()` 会直接调用 engine bake 回调，不会自动包完整 `DRW_render_to_image()` final-render 路径。
- 普通 Eevee camera render path 依赖 camera frame、film、GBuffer、post-processing，不适合作为 bake 路径。
- 建议新增独立 material-bake mode，不要把所有逻辑硬塞进 `is_light_bake`，除非实现时能证明不会造成语义混乱。

### Task 5: 增加验证和错误门禁

**文件：**
- 修改：`source/blender/editors/object/object_bake_api.cc`
- 修改：`source/blender/gpu/GPU_material.hh`
- 修改：`source/blender/nodes/shader/nodes/node_shader_npr_input.cc`
- 可能修改：`source/blender/nodes/shader/nodes/node_shader_npr_refraction.cc`
- 可能修改：`source/blender/nodes/shader/nodes/node_shader_input_aov.cc`

**要求：**
- Eevee bake 在 engine 执行前拒绝不支持的 bake type。
- 拒绝 selected-to-active、cage、multires、vertex-color target、非 mesh object、Filter-domain material。
- 拒绝使用屏幕依赖节点的材质：
  - `NPR Input`
  - `NPR Refraction`
  - Input AOV
  - Screen Space Info
  - Scene Color / Render Texture feedback
- 错误信息要指出具体不支持功能。

**注意：**
- `NPR Refraction`、AOV、Screen Space Info 已经有 material flag。`NPR Input` 需要新增 material flag，或在 bake 前扫描 node tree。
- 验证必须足够早，避免用户拿到 stale 或半写入图像。

### Task 6: 增加最小 Eevee Bake UI

**文件：**
- 修改：`scripts/startup/bl_ui/properties_render.py`
- 避免修改 Cycles addon UI，除非只是复用共享 helper。

**要求：**
- 当 engine 为 `BLENDER_EEVEE` 时，在 Render Properties 显示一个小型 Eevee Bake panel。
- Bake 按钮调用 `object.bake(type='EMIT')`。
- 只显示 image texture target 和 margin 所需的现有 bake output 控件。
- 不暴露 Cycles 完整 bake type 矩阵。

**注意：**
- Python 用户仍然可以手动调用其他 `bpy.ops.object.bake(type=...)`；这些类型必须被验证层或 Eevee bake 回调拒绝。

### Task 7: 增加 Release Test Case

**文件：**
- 新增：`test/release/cases/eevee_color_bake/case.json`
- 新增：`test/release/cases/eevee_color_bake/README.md`
- 新增：`test/release/cases/eevee_color_bake/run.py`

**要求：**
- 测试放在外层 workspace release-test 系统中，不作为临时文件塞进源码仓库。
- 使用安装树 Blender 后台运行。
- 创建 UV plane，烘焙纯 emission 材质，断言中心像素颜色。
- 两个 material slot 分别烘焙到两个 image，断言每个 image 收到正确颜色。
- `Shader Info` 或 `GLSL Function` light helper 材质：改变灯光颜色/强度后，断言 bake 像素变化。
- 本地 NPR color 材质：断言阈值/混合输出符合预期。
- 负向测试：unsupported bake type 和 screen-dependent NPR input 必须产生明确错误。

**注意：**
- `case.json` 和 README 必须描述真实语义通过条件，不能只写“脚本返回 0”。
- 输出图像和日志放到 release case 的 out/log 路径，不放源码根目录。

## Commit Plan

按以下顺序拆提交：

```powershell
git -C blender_5_1_port_mainfix commit -m "docs: plan eevee color bake"
git -C blender_5_1_port_mainfix commit -m "feat: add eevee color bake callback"
git -C blender_5_1_port_mainfix commit -m "feat: add eevee uv-space bake pipeline"
git -C blender_5_1_port_mainfix commit -m "feat: support eevee local npr color bake"
git -C blender_5_1_port_mainfix commit -m "test: cover eevee color bake"
git -C blender_5_1_port_mainfix commit -m "ui: expose eevee color bake panel"
```

第一条提交只包含本文档。源码实现从后续提交开始。

## Verification

从外层 workspace 根目录构建：

```powershell
.\build_ninja_sccache_poll.bat mainfix install --no-pause
```

使用 mainfix 安装树验证：

```powershell
E:\blender_bulid_test\blender_npr_bulid\install_windows_x64_vc17_Release_5_1_port_mainfix\blender.exe --background --factory-startup --python test\release\cases\eevee_color_bake\run.py
```

shader/header 改动后，在信任失败或成功前必须检查：

- 没有残留 `cmake.exe` 或 `ninja.exe` 进程。
- 安装树 `blender.exe --version` 对应当前分支。
- 如果改了 `.glsl`、`.hh` 或 shader create-info，检查 generated shader source 和相关 `bf_draw` / `bf_gpu` object 时间戳。
- 如果输出黑、透明或崩在无关 Eevee 代码，优先怀疑 stale draw/GPU object，再判断逻辑设计。

## 风险和规避

- **UV batch 风险：** edit-UV batch 能把 UV 当 `pos`，但不足以做真实材质求值。使用专用 bake geometry path 保留原始 surface 数据。
- **GPU context 风险：** bake callback 是直接调用，不是 final-render 包装。Eevee bake 路径内部必须建立 DRW custom pipeline。
- **Shader 编译风险：** deferred compilation fallback 会静默烘焙错误颜色。必须等待编译完成并重新 sync。
- **Sampler slot 风险：** `Shader Info` 和 GLSL light helper 依赖 Eevee 固定资源槽。扩展现有 reserved-slot 逻辑，不做局部临时绑定。
- **Screen-data 风险：** V1 无法重建 NPR screen/GBuffer 输入。必须明确拒绝。
- **构建陈旧风险：** 当前 workspace 已知存在 stale object 和 datatoc 问题。信任 runtime 前检查 install-tree hash 和 object 时间戳。

## Acceptance Criteria

- Eevee 下 `bpy.ops.object.bake(type='EMIT')` 能为简单 emission 材质写出正确图像。
- 多材质、多 image bake 分别写入正确目标图。
- 支持的 light-dependent 材质在改变灯光后 bake 结果随之变化。
- 支持的本地 NPR color 材质能烘焙 NPR color 结果。
- 不支持的 pass type 和 screen-dependent 材质给出明确错误。
- 现有 Cycles bake 行为不变。
- release case 使用 mainfix 安装树通过。

## Assumptions

- 基线分支是 `blender_5_1_port_mainfix` 中的 `npr-port-5.1`。
- V1 是本地 Eevee/NPR color bake，不是最终可见色 bake。
- V1 不新增 DNA/RNA bake 设置。
- 完整 Cycles bake parity 需要在 V1 之后另起设计。
