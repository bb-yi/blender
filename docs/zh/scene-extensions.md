# Scene 级 Eevee 扩展

## 1. Render Textures

#### 功能说明

`Render Textures` 是场景级的 Eevee 额外渲染纹理系统。

它允许场景预先维护最多 `4` 个 Render Texture 槽位。每个槽位都可以指定一个相机和一个输出类型，把该相机视角下的场景结果先渲染成纹理，再在普通物体材质中通过 `Render Texture` 节点采样。

#### 面板入口

`Scene Properties > Render Textures`

<div align="center">
  <img src="images/SnowShot_2026-03-28_04-37-13.png" alt="Render Textures" style="border-radius: 10px;">
  <br>
</div>

#### 可配置内容

每个 `Render Texture` 条目当前支持：

- `Name`
- `Enabled`
- `Source`
  - `Color`：捕获最终 Eevee 颜色
  - `Depth`：捕获线性深度
  - `Normal`：捕获法线
- `Camera`
- `Resolution X / Y`
- `Update Mode`
  - `Every Sample`
  - `Every Frame`
  - `Manual`
- `Format`
  - `RGBA16F`
  - `RGBA32F`
  - `R16F`
  - `R32F`

#### 基本使用方法

1. 打开 `Scene Properties > Render Textures`。
2. 新建一个 `Render Texture` 条目。
3. 选择 `Source`、`Camera`、分辨率、刷新模式和格式。
4. 在普通物体材质里添加 `Add > Texture > Render Texture` 节点。
5. 在节点面板中选择对应的 `Render Texture` 条目。
6. 使用节点输出的 `Color` / `Alpha` 参与后续材质计算。

## 2. Filter Materials

#### 功能说明

它是一套场景级的 Eevee 全屏滤镜栈。每个条目都是一个 `Filter` 域材质，按列表顺序依次对当前帧进行处理。

#### 面板入口

`Scene Properties > Filter Materials`

<div align="center">
  <img src="images/SnowShot_2026-03-28_04-41-53.png" alt="Filter Materials" style="border-radius: 10px;">
  <br>
</div>

节点树入口：着色器节点编辑器 > 着色器类型 > `Filter`

<div align="center">
  <img src="images/SnowShot_2026-03-28_04-42-42.png" alt="Filter Shader Type" style="border-radius: 10px;">
  <br>
</div>

#### 基本使用方法

1. 打开 `Scene Properties > Filter Materials`。
2. 新建一个条目，或者直接点 `New Filter Material`。
3. 选中的材质必须是 `Filter` 域材质。
4. 打开 Shader Editor，把顶部 `Shader Type` 切换到 `Filter`。
5. 在滤镜材质里使用 `Scene Color` 读取场景数据，也可以配合 `Filter Object Info` 或 `Filter Mask` 做对象驱动控制。
6. 如果需要读取已有自定义通道，可使用 `AOV Input`。
7. 如果需要把滤镜中间结果或最终结果额外写入自定义通道，可使用 `AOV Output`。
8. 通过 `Execution Stage` 选择滤镜执行位置。

#### 重要说明

- `Scene Color` 节点的默认采样坐标为 `Texture Coordinate` 节点的 `Window` 输出
- 支持 `AOV Input`
- 支持 `AOV Output`，可以先写出命名 AOV，再继续把结果送到 `Filter Output`
- `Execution Stage` 目前提供三个位置：
  - `Before Volume Fog`
  - `Before Depth of Field`
  - `Before Composite`

## 3. Native Camera FX Outputs

#### 功能说明

`Native Camera FX Outputs` 是 View Layer 级的 Eevee 原生后期输出系统。它可以把指定的渲染通道抽出后，单独套用 Eevee 的 `Motion Blur` 和 / 或 `Depth of Field`，再以新的 Render Pass 输出。

这适合为描边、AOV、深度、法线、光照分量等通道生成带相机运动模糊或景深的版本，用于合成器、后续滤镜或外部后期流程。

#### 面板入口

`View Layer Properties > Passes > Native Camera FX Outputs`

#### 可配置内容

每个输出条目支持：

- `Name`：生成的 Render Pass 名称
- `Enabled`：是否生成该输出
- `Source`：要处理的来源通道
- `Shader AOV`：当 `Source` 为 `Shader AOV` 时选择具体 AOV 名称
- `Motion Blur`：套用 Eevee 原生运动模糊
- `Depth of Field`：套用 Eevee 原生景深

#### Source 支持项

- `Depth`
- `Normal`
- `Position`
- `Vector`
- `Diffuse Light`
- `Diffuse Color`
- `Specular Light`
- `Specular Color`
- `Volume Light`
- `Emission`
- `Environment`
- `Shadow`
- `Ambient Occlusion`
- `Transparent`
- `Shader AOV`
- `Outline`

#### 基本使用方法

1. 切换到 `Eevee` 渲染引擎。
2. 打开 `View Layer Properties > Passes > Native Camera FX Outputs`。
3. 新建一个输出条目。
4. 设置 `Name` 和 `Source`。
5. 按需要启用 `Motion Blur`、`Depth of Field`，或同时启用两者。
6. 在合成器或后续流程中读取同名 Render Pass。

#### 重要说明

- `Motion Blur` 仍需要场景 / View Layer 中启用 Eevee 运动模糊
- `Depth of Field` 仍使用当前相机的景深设置
- `Shader AOV` 来源必须选择 View Layer 中已经存在的 AOV 名称
- 如果条目出现无效状态，通常是名称冲突、来源 AOV 不存在，或超过当前可用输出数量
- 描边通道可作为 `Outline` 来源输出，并可单独获得带景深或运动模糊的版本

## 4. Eevee Outline

#### 功能说明

`Eevee Outline` 是场景级描边总开关，用于控制当前 NPR Port 内置的屏幕空间描边系统。

#### 面板入口

`Render Properties > Outline`

描边 Render Pass 入口：

`View Layer Properties > Passes > Data > Outline`

#### 行为说明

- 默认开启，保持 `Outline Control` 节点和 `Outline` Render Pass 的正常行为
- 关闭后，`Outline Control` 节点不会影响 Combined 渲染结果
- 关闭后，即使 View Layer 中启用了 `Outline` Render Pass，也不会输出描边内容
- 该开关用于快速回到与未启用描边系统时一致的 Eevee 渲染结果
- 当 `Outline` Render Pass 未开启时，描边结果会直接合成进 `Combined`
- 当 `Outline` Render Pass 开启时，可在合成器或后续流程中单独读取描边结果
- 该功能依赖材质中的 `Outline Control` 节点实际写入描边参数；没有节点输出时不会自动生成描边

#### 建议补图

- `images/placeholder_eevee_outline.png`
  - 建议内容：`Render Properties > Outline` 面板
- `images/placeholder_outline_render_pass.png`
  - 建议内容：`View Layer Properties > Passes > Data > Outline` 位置，或合成器读取 `Outline` pass 的示例
