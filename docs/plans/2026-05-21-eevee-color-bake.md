# Eevee Color Bake GPU/DRW 实现说明

**状态：** 已在 `feat/eevee-color-bake` 上落地 GPU/DRW UV-space 单路径实现。旧的 CPU 节点白名单求值器不保留，也不作为 fallback。

**目标：** 补齐 Eevee 下的 `object.bake(type='EMIT')` 工作流，把它作为 Eevee-only Color Bake 使用。当前语义是烘焙 Eevee 材质图在 UV 空间可局部求出的 scene-linear 着色结果，不是 Cycles 完整 bake pass 矩阵，也不是最终相机可见色。

**公开入口：** 继续使用现有 Python API 和通用 Bake API：

```python
bpy.ops.object.bake(type="EMIT")
```

## 实现路径

Eevee Color Bake 只有一条实现链路：

```text
object.bake(type='EMIT')
  -> 通用 Bake API 生成 BakePixel / BakePrimitive / BakeDifferential / BakeImage
  -> Eevee RenderEngineType.bake 回调
  -> GPU context + DRW custom pipeline
  -> UV-space mesh raster pass
  -> Eevee GPUMaterial / NPR 本地着色求值
  -> GPU readback 到 RenderResult Combined pass
  -> 通用 Bake API 处理 margin 与 image 写回
```

关键点：

- 通用 Bake API 仍负责 image texture target、有效像素、UV island、margin 和最终图片写回。
- Eevee bake 回调只负责校验、建立 GPU/DRW 绘制环境、提交 UV 空间 draw、读回颜色。
- 材质不在 CPU 上解释节点。纹理、程序节点、Node Group、GLSL Function、Shader Info、`Shader to RGB` 和本地 NPR Tree 都走现有 Eevee GPUMaterial 编译路径。
- bake shader 输出 scene-linear RGBA 到 `Combined` bake pass；色彩空间转换继续交给现有 image/bake 写回层。
- 该功能是独立 bake pipeline。正常 viewport/render 不提交 `MAT_PIPE_BAKE_COLOR`，不会增加普通 Eevee 帧渲染时间。

## 支持范围

- Render engine：`BLENDER_EEVEE`。
- Bake type：只接受 `SCE_PASS_EMIT`，在 Eevee 中解释为 Color Bake。
- Target：`scene.render.bake.target == IMAGE_TEXTURES`。
- Geometry：mesh object 自身 bake。
- 图像：沿用现有 active image texture target 规则，支持多材质、多 image、UV island、margin；UDIM/tile 目标由通用 Bake API 的 `BakeImage` 分发机制承接。
- 普通材质：
  - Emission。
  - 普通 BSDF / Principled / Diffuse 直接接 Material Output 时输出局部 Eevee 着色结果。
  - `Shader to RGB`：通过 bake 专用 `closure_to_rgba()` 使用同一套局部 closure -> color 求值。
  - Image Texture、Checker、Noise、Voronoi 等可由 Eevee GPUMaterial 编译的本地纹理和程序节点。
  - UV Map、Texture Coordinate、Mapping、ColorRamp、Math、Mix、Reroute。
  - Tangent Space Normal Map、Tangent 节点 UV Map 模式、World To Tangent 等依赖 UV tangent attribute 的本地材质逻辑。
  - Node Group。
  - GLSL Function，包括不依赖屏幕输入的本地颜色逻辑和 light helper。
- Eevee/NPR 本地颜色：
  - 本地 NPR Tree `nodetree_npr()` 输出。
  - Shader Info 本地光照相关输出。
  - GLSL Function light helper 和 NPR foreach light 所需的 Eevee light/lightprobe 资源。

## 光照语义

- 普通 Shader/BSDF Surface 烘焙的是局部 Eevee 着色结果。没有场景灯光且 world 为黑时，普通 BSDF 输出接近黑色，这是预期行为。
- Emission 仍直接输出自发光颜色，不依赖场景灯光。
- 场景灯光的颜色、强度、位置、法线、粗糙度、探针等会参与局部求值。
- bake pass 在读取材质前会渲染 world probe，使 Diffuse/BSDF 能得到 scene world 的环境光。
- V1 支持点光、聚光和太阳光的保守 shadow-map bake。Color Bake 会在 bake 回调内部同步可见 shadow caster，按 bake target bounds 标记 receiver 需要的 shadow pages，并在 bake shader 中执行 `shadow_eval()`。这条路径只在执行 Eevee Color Bake 时运行，不增加普通 viewport/render 的每帧成本。
- 阴影语义仍是 Eevee 局部 shadow-map 结果，不等同 Cycles selected-to-active/cage 阴影、透明阴影排序、屏幕空间阴影或最终相机视图逐像素一致性。
- 视角相关效果只代表 bake pass 的局部求值上下文，不等同最终相机视图。

## 不支持范围

这些场景必须在写出错误图片前失败，并给出明确错误：

- 非 `EMIT` bake type：`COMBINED`、Diffuse、Glossy、Transmission、AO、Normal、Position、Roughness、UV 以及 Cycles pass filter 矩阵。
- Selected-to-active、高低模 ray projection、cage、multires 和 Cycles 风格真实 ray bake 语义。
- 非 image texture target、vertex color target、volume、curve、point cloud、world bake。
- 依赖屏幕或历史帧的节点和路径：
  - `NPR Input` 的 GBuffer/screen 读取。
  - `Scene Color`。
  - `Screen Space Info`。
  - `Render Texture` feedback。
  - `NPR Refraction` / back-buffer。
  - Input AOV / Output AOV。
  - Filter-domain output。
- 后处理、视图合成、最终可见色、透明排序与屏幕层合成一致性。

## 主要修改文件

源码仓库：`blender_5_1_port_mainfix`

- `source/blender/draw/engines/eevee/eevee_bake.cc`
  - GPU/DRW bake 调度器。
  - 校验 Eevee Color Bake 支持范围。
  - bake 场景同步 `OB_LAMP`、`OB_LIGHTPROBE` 和可见 mesh/curve/point cloud shadow caster。
  - 根据 bake target world bounds 配置临时正交 bake camera，稳定太阳光 cascade 覆盖范围。
  - 为 bake receiver bounds 执行保守 shadow page usage tagging。
  - 为 bake 专用 VBO 补齐 `CD_TANGENT` 输入。
  - 为每个 `BakeImage` 创建 offscreen color target，提交 UV-space draw，readback 到 `Combined` pass。
  - draw 前执行 bake 专用 `capture_view.render_world()`，保证 world probe 环境光可用于局部 BSDF 求值。
  - 显式启用 GPU context，并用 `DRWContext::CUSTOM` 包住 custom pipeline。
- `source/blender/draw/engines/eevee/eevee_shader.cc`
  - 为 bake pipeline 创建专用 material shader variant。
  - `MAT_PIPE_BAKE_COLOR` 复用 surface graph，并为 BSDF/`Shader to RGB` 定义 `LIGHT_CLOSURE_EVAL_COUNT`。
  - bake pipeline 绑定 Shader Info、GLSL light helper、NPR foreach light 所需资源。
- `source/blender/draw/engines/eevee/shaders/eevee_surf_bake_color_frag.glsl`
  - bake 专用 `closure_to_rgba()` 复用 Eevee forward lighting，把 closure bins 转成 scene-linear radiance。
  - 普通材质输出局部 shading 结果。
  - NPR 材质用局部 shading 结果填充本地输入后输出 `nodetree_npr()`。
  - 不读取 deferred combine、radiance、hiz、prepass、back-buffer 或后处理输入。
- `source/blender/draw/engines/eevee/shaders/infos/eevee_surf_bake_infos.hh`
  - 注册 bake color surface create-info，并补齐 light shader texture/uniform 资源。
  - 对 bake material 启用 local light no-cull 迭代，避免 UV-space 像素与普通相机 tile culling 不一致导致漏光。
- `source/blender/draw/engines/eevee/shaders/eevee_light_eval_lib.glsl`
  - `MAT_BAKE_COLOR` 下恢复 shadow map attenuation；普通渲染 shader 不受影响。
- `source/blender/draw/engines/eevee/eevee_shadow.cc` / `eevee_shadow.hh`
  - 新增 Color Bake receiver bounds tagging 入口，复用现有透明 bounds usage tagging shader 保守标记 shadow pages。
- `source/blender/draw/engines/eevee/shaders/eevee_geom_bake_mesh_vert.glsl`
  - 使用 bake UV 生成 clip-space position。
  - 保留原始 mesh position、normal、UV 和 attribute 供材质节点使用。
- `source/blender/gpu/shaders/gpu_shader_codegen_lib.glsl`
  - `MAT_BAKE_COLOR` 下将 shader `FrontFacing` 固定为 true，使 Normal Map、Bump、Geometry Backfacing 等本地节点按源 mesh 正面求值，不受 UV-space 三角形 winding 影响。

## 实现注意事项

- bake 回调不是普通 Eevee render path，不能依赖 camera render 的 GBuffer、film、postprocess 或 final combine。
- bake draw 必须绑定 `inst.lights.bind_resources(sub)` 和 `inst.lights.bind_front_light_shader_resources(sub)`，不能直接绑定 raw light buffer。
- Shader 编译和纹理准备必须完成后再读回结果，不能用 default/error material 当作临时 fallback。
- `.glsl`、shader create-info 或相关 `.hh` 改动后，若安装树结果没有变化，优先检查 stale datatoc / unity object，必要时执行 mainfix `clean-unity install`。
- Tangent support 只负责 mesh/UV tangent space。没有可解析 UV map 的 tangent-space material attribute 会明确失败；不引入 CPU 节点求值 fallback。

## Release 测试

外层 release case：

```text
E:\blender_bulid_test\blender_npr_bulid\test\release\cases\eevee_color_bake
```

覆盖的正向场景：

- 纯 Emission。
- Principled Emission。
- 普通 BSDF 无灯光接近黑色。
- 普通 BSDF 在 scene world 环境光增强后变亮。
- 普通 BSDF 在点光下随灯光强度变亮。
- `Shader to RGB` 在点光下随灯光强度变亮。
- 多材质多 image。
- Image Texture 接入 BSDF 后在白光下输出与纹理颜色通道一致的局部着色结果。
- Checker + UV Map + Mapping + ColorRamp。
- Node Group。
- GLSL Function 本地颜色。
- 本地 NPR Tree 颜色。
- Shader Info 灯光响应。
- Tangent Space Normal Map 光照响应。

覆盖的负向场景：

- 非 `EMIT` bake type。
- `NPR Input`。
- `Scene Color`。
- `Screen Space Info`。
- `Render Texture`。
- `NPR Refraction`。
- Input AOV / Output AOV。
- Filter-domain output。

专项验证命令：

```powershell
.\build_ninja_sccache_poll.bat mainfix install --no-pause
.\run_release_tests.bat -BlenderExe install_windows_x64_vc17_Release_5_1_port_mainfix\blender.exe -Name eevee_color_bake --no-pause
```

日常 Eevee bake 修改只要求 no-Cycles mainfix 构建和 `eevee_color_bake` 专项通过；完整 release suite 中的 Cycles case 只在发布/打包需要 `with-cycles` 构建时运行。

## 验收标准

- Eevee 中 `bpy.ops.object.bake(type='EMIT')` 能把本地 Eevee 着色结果写入目标 image texture。
- 普通 Shader/BSDF 和 `Shader to RGB` 输出局部光照结果；无光时普通 BSDF 变暗或为黑。
- 纹理、程序节点、UV/Mapping、Node Group 和 GLSL Function 不再受 CPU 节点白名单限制。
- 本地 NPR Tree 颜色能覆盖 surface color 并被烘焙。
- Shader Info/light helper 能读取 Eevee light 资源，灯光变化会反映到 bake 结果。
- Tangent-space material attributes 能在 Eevee bake 中参与 GPU 材质求值。
- 不支持的屏幕空间、AOV、Filter-domain、refraction 和非 Emit 场景明确失败。
- 现有 Cycles bake 行为不改变。
- mainfix 安装树专项 release case 通过。
