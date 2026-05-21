# Eevee Color Bake GPU/DRW 实现说明

**状态：** 已在 `feat/eevee-color-bake` 上落地 GPU/DRW UV-space 单路径实现。旧的 CPU 节点白名单求值器不再保留，也不作为 fallback。

**目标：** 补齐 Eevee 下的 `object.bake(type='EMIT')` 工作流，把它作为 Eevee-only Color Bake 使用。它烘焙的是材质图在 UV 空间可本地求出的颜色，不是 Cycles 完整 bake pass 矩阵，也不是最终相机可见色。

**公开入口：** 继续使用现有 Python API 和通用 Bake API：

```python
bpy.ops.object.bake(type="EMIT")
```

## 实现路线

Eevee Color Bake 只有一条实现链路：

```text
object.bake(type='EMIT')
  -> 通用 Bake API 生成 BakePixel / BakePrimitive / BakeDifferential / BakeImage
  -> Eevee RenderEngineType.bake 回调
  -> GPU context + DRW custom pipeline
  -> UV-space mesh raster pass
  -> Eevee GPUMaterial / NPR 本地颜色求值
  -> GPU readback 到 RenderResult Combined pass
  -> 通用 Bake API 处理 margin 与 image 写回
```

关键点：

- 通用 Bake API 仍负责 image texture target、有效像素、UV island、margin 和最终图片写回。
- Eevee bake 回调只负责校验、建立 GPU/DRW 绘制环境、提交 UV 空间 draw、读回颜色。
- 材质不在 CPU 上解释节点。纹理、程序节点、Node Group、GLSL Function、Shader Info 和本地 NPR Tree 都走现有 Eevee GPUMaterial 编译路径。
- bake shader 输出 scene-linear RGBA 到 `Combined` bake pass，色彩空间转换继续交给现有 image/bake 写回层。

## 支持范围

- Render engine：`BLENDER_EEVEE`。
- Bake type：只接受 `SCE_PASS_EMIT`，在 Eevee 中解释为 Color Bake。
- Target：`scene.render.bake.target == IMAGE_TEXTURES`。
- Geometry：mesh object 自身 bake。
- 图像：沿用现有 active image texture target 规则，支持多材质、多 image、UV island 和 margin；UDIM/tile 目标由通用 Bake API 的 `BakeImage` 分发机制承接。
- 普通材质颜色：
  - Emission。
  - Principled Base Color 与 Emission。
  - Image Texture。
  - Checker、Noise、Voronoi 等可由 Eevee GPUMaterial 编译的本地程序纹理。
  - UV Map、Texture Coordinate、Mapping、ColorRamp、Math、Mix、Reroute。
  - Tangent Space Normal Map、Tangent 节点 UV Map 模式、World To Tangent 等依赖 UV tangent attribute 的本地材质逻辑。
  - Node Group。
  - GLSL Function，包括不依赖屏幕输入的本地颜色逻辑。
- Eevee/NPR 本地颜色：
  - 本地 NPR Tree `nodetree_npr()` 颜色输出。
  - Shader Info 本地光照相关输出。
  - GLSL Function light helper 和 NPR foreach light 所需的 Eevee light/shadow/lightprobe 资源。

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
- Camera / Window / Screen 坐标语义只在材质本地求值范围内可用，不保证等同最终视图。

## 主要修改文件

源码仓库：`blender_5_1_port_mainfix`

- `source/blender/draw/engines/eevee/eevee_bake.cc`
  - 重写为 GPU/DRW bake 调度器。
  - 删除 CPU `BakeEvalContext`、节点递归、灯光 CPU 估算和常量材质缓存。
  - 校验 Eevee Color Bake 支持范围。
  - 为 bake 专用 VBO 补齐 `CD_TANGENT` 输入：按 GPUMaterial 请求的 UV layer 调用 `bke::mesh::calc_uv_tangents()`，上传 `float4` tangent/sign，供现有 Eevee material shader 使用。
  - 为每个 `BakeImage` 创建 offscreen color target，提交 UV-space draw，readback 到 `Combined` pass。
  - 显式启用 GPU context，并用 `DRWContext::CUSTOM` 包住 custom pipeline。

- `source/blender/draw/engines/eevee/eevee_material_shared.hh`
  - 新增 `MAT_PIPE_BAKE_COLOR`。

- `source/blender/draw/engines/eevee/eevee_shader.cc`
  - 为 bake pipeline 创建专用 material shader variant。
  - bake pipeline 复用 surface graph，并在 NPR 材质中接入本地 `nodetree_npr()`。
  - bake pipeline 绑定 Shader Info、GLSL light helper、NPR foreach light 所需资源。
  - 固定定义 `LIGHT_ITER_FORCE_NO_CULLING`，避免 UV-space bake 缺少普通视图 culling 数据导致 light iteration 为空。

- `source/blender/draw/engines/eevee/shaders/eevee_geom_bake_mesh_vert.glsl`
  - 使用 bake UV 生成 clip-space position。
  - 保留原始 mesh position、normal、UV 和 attribute 供材质节点使用。

- `source/blender/draw/engines/eevee/shaders/eevee_surf_bake_color_frag.glsl`
  - 普通材质输出本地 surface color/emission。
  - NPR 材质输出本地 `nodetree_npr()` 结果。
  - 不读取 deferred combine、radiance、hiz、prepass、back-buffer 或后处理输入。
  - `init_globals()` 后恢复 mesh 插值法线，避免 UV 空间 winding 影响 local light helper。

- `source/blender/draw/engines/eevee/shaders/infos/eevee_surf_bake_infos.hh`
  - 注册 bake color surface create-info。

- `source/blender/draw/engines/eevee/shaders/infos/eevee_geom_infos.hh`
- `source/blender/draw/engines/eevee/shaders/infos/eevee_material_infos.hh`
- `source/blender/draw/engines/eevee/shaders/CMakeLists.txt`
- `source/blender/draw/CMakeLists.txt`
  - 注册 bake geometry/material shader 和 datatoc。

- `source/blender/draw/engines/eevee/eevee_nodetree_lib.glsl`
- `source/blender/gpu/shaders/gpu_shader_codegen_lib.glsl`
- `source/blender/gpu/shaders/material/gpu_shader_material_principled.glsl`
- `source/blender/gpu/shaders/material/gpu_shader_material_shader_info.glsl`
  - 补齐 bake shader 需要的本地颜色、NPR 和 Shader Info/light helper 输出。
  - `MAT_BAKE_COLOR` 下将 shader `FrontFacing` 固定为 true，使 Normal Map、Bump、Geometry Backfacing 等本地节点按源 mesh 正面求值，不受 UV-space 三角形 winding 影响。

## 实现注意事项

- bake 回调不是普通 Eevee render path。它不能依赖 camera render 的 GBuffer、film、postprocess 或 final combine。
- bake draw 必须绑定 `inst.lights.bind_resources(sub)`，不能直接绑定 raw `light_buf_`。raw light buffer 的顺序与 light iteration 期望的 culled light buffer 顺序不同，会导致 Shader Info/light helper 结果错误。
- Shader 编译和纹理准备必须完成后再读回结果。不能用 default/error material 当作临时 fallback。
- `.glsl`、shader create-info 或相关 `.hh` 改动后，若安装树结果没有变化，优先检查 stale datatoc / unity object，必要时执行 mainfix `clean-unity install`。
- Tangent support 只负责 mesh/UV tangent space。没有可解析 UV map 的 tangent-space material attribute 会明确失败；不引入 CPU 节点求值 fallback。

## Release 测试

外层 release case：

```text
E:\blender_bulid_test\blender_npr_bulid\test\release\cases\eevee_color_bake
```

覆盖的正向场景：

- 纯 Emission。
- Principled Base Color / Emission。
- 多材质多 image。
- Image Texture。
- Checker + UV Map + Mapping + ColorRamp。
- Node Group。
- GLSL Function 本地颜色。
- 本地 NPR Tree 颜色。
- Shader Info 灯光响应，灯光强度提高后 bake 像素变亮。
- Tangent Space Normal Map 光照响应，证明 tangent-space material attribute 被 GPU/DRW bake batch 正确上传并参与 shader 求值。

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
E:\blender_bulid_test\blender_npr_bulid\install_windows_x64_vc17_Release_5_1_port_mainfix\blender.exe --background --factory-startup --python test\release\cases\eevee_color_bake\run.py
```

完整 release 测试：

```powershell
.\run_release_tests.bat --no-pause
```

日常 Eevee bake 修改只要求 no-Cycles mainfix 构建和 `eevee-color-bake` 专项通过；完整 release suite 中的 Cycles case 只在发布/打包需要 `with-cycles` 构建时运行。

## 验收标准

- Eevee 下 `bpy.ops.object.bake(type='EMIT')` 能把本地材质颜色写入目标 image texture。
- 普通纹理、程序节点、UV/Mapping、Node Group 和 GLSL Function 不再被 CPU 白名单限制。
- 本地 NPR Tree 颜色能覆盖 surface color 并被烘焙。
- Shader Info/light helper 能读取 Eevee light 资源，灯光变化会反映到 bake 结果。
- Tangent-space material attributes 能在 Eevee bake 中参与 GPU 材质求值，Tangent Space Normal Map 的光照结果会随 tangent-space normal 改变。
- 不支持的屏幕空间、AOV、Filter-domain、refraction 和非 Emit 场景明确失败。
- 现有 Cycles bake 行为不改变。
- mainfix 安装树专项 release case 通过；完整 release 测试留到需要 `with-cycles` 发布构建时执行。
