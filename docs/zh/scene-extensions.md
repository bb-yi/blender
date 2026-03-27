# 一、Scene 级 Eevee 扩展

## 1. Render Textures

### 功能说明

`Render Textures` 是场景级的 Eevee 额外渲染纹理系统。

它允许场景预先维护最多 `4` 个 Render Texture 槽位，每个槽位都可以指定一个相机和一个输出类型，把该相机视角下的场景结果先渲染成纹理，再在普通物体材质中通过 `Render Texture` 节点采样。

### 面板入口

`Scene Properties > Render Textures`

<div align="center">
    <img src="images/SnowShot_2026-03-28_04-37-13.png" alt="alt text" style="border-radius: 10px;">
    <br>
</div>

### 可配置内容

每个 Render Texture 条目当前支持：




    - `Color`：捕获最终 Eevee 颜色

    - `Depth`：捕获线性深度

    - `Normal`：捕获法线




    - `Every Sample`：每次采样时更新

    - `Every Frame`：每帧更新一次

    - `Manual`：手动更新


    - `RGBA16F`：16位浮点颜色，平衡质量和性能

    - `RGBA32F`：32位高精度颜色，要求更高显存

    - `R16F`：16位浮点深度值

    - `R32F`：32位高精度深度值

### 基本使用方法

1. 打开 `Scene Properties > Render Textures`。

2. 新建一个 `Render Texture` 条目。

3. 选择 `Source`、`Camera`、分辨率、刷新模式和格式。

4. 在普通物体材质里添加 `Add > Texture > Render Texture` 节点。

5. 在节点面板中选择对应的 `Render Texture` 条目。

6. 使用节点输出的 `Color` / `Alpha` 参与后续材质计算。

## 2. Filter Materials

### 功能说明

它是一套场景级的 Eevee 全屏滤镜栈。每个条目都是一个 `Filter` 域材质，按列表顺序依次对当前帧进行处理。

### 面板入口

`Scene Properties > Filter Materials`

<div align="center">
    <img src="images/SnowShot_2026-03-28_04-41-53.png" alt="alt text" style="border-radius: 10px;">
    <br>
</div>

节点树入口: 着色器节点编辑器 > 着色器类型 > Filter
<div align="center">
    <img src="images/SnowShot_2026-03-28_04-42-42.png" alt="alt text" style="border-radius: 10px;">
    <br>
</div>

### 基本使用方法

1. 打开 `Scene Properties > Filter Materials`。

2. 新建一个条目，或者直接点 `New Filter Material`。

3. 选中的材质必须是 `Filter` 域材质。

4. 打开 Shader Editor，把顶部 `Shader Type` 切换到 `Filter`。

5. 在滤镜材质里使用 `Scene Color` 读取场景数据，用 `Filter Output` 输出结果。

6. 通过 `Execution Stage` 选择滤镜执行位置。

### 重要说明




    - `Before Volume Fog`：在体积雾处理前执行

    - `Before Depth of Field`：在景深前执行

    - `Before Composite`：在合成器前执行
