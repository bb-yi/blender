# Blender 5.1 NPR Port 新增功能与使用说明

## 文档范围

这份文档说明当前 `Blender 5.1 NPR Port` 相比官方 `Blender 5.1` 已经加入、并且当前分支内实际存在的 NPR / Eevee 扩展功能，以及它们的基本使用方法。

修改日期:2026/3/23
对应构建哈希：0b5a1dd68c06

## 与官方 Blender 5.1 的主要区别

当前这个 5.1 NPR 版本，和官方 Blender 5.1 相比，主要多了四类能力：

1. `Eevee` 的场景级扩展工作流
   - `Render Textures`
   - `Filter Materials`

2.  `Eevee` 的新着色器节点
    - `Render Info`
    - `Screen Derivative`

3. `Goo Engine` 移植节点
   - `Screenspace Info`
   - `Curvature`
   - `Raycast`
   - `Shader Info`
   - `Light Info`

4. `NPR Tree` 工作流与配套节点
   - `NPR Input`
   - `NPR Output`
   - `NPR Refraction`
   - `Image Sample`
   - `For Each Light`
   - 内置的 NPR 节点组资产包
      - `Cavity`
      - `Co-Planar Edge Detection`
      - `Curvature`
      - `Kuwahara`
      - `Shading Models`
      - `Surface Curvature`

6. 界面整理节点
   - `Portal In / Portal Out`


## 一、Scene 级 Eevee 扩展

### 1. Render Textures

#### 功能说明

`Render Textures` 是场景级的 Eevee 额外渲染纹理系统。

它允许场景预先维护最多 `4` 个 Render Texture 槽位，每个槽位都可以指定一个相机和一个输出类型，把该相机视角下的场景结果先渲染成纹理，再在普通物体材质中通过 `Render Texture` 节点采样。


#### 面板入口

`Scene Properties > Render Textures`

#### 可配置内容

每个 Render Texture 条目当前支持：

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

### 2. Filter Materials

#### 功能说明


它是一套场景级的 Eevee 全屏滤镜栈。每个条目都是一个 `Filter` 域材质，按列表顺序依次对当前帧进行处理。

#### 面板入口

`Scene Properties > Filter Materials`

节点树入口:
着色器节点编辑器 > 着色器类型 > Filter

#### 基本使用方法

1. 打开 `Scene Properties > Filter Materials`。
2. 新建一个条目，或者直接点 `New Filter Material`。
3. 选中的材质必须是 `Filter` 域材质。
4. 打开 Shader Editor，把顶部 `Shader Type` 切换到 `Filter`。
5. 在滤镜材质里使用 `Scene Color` 读取场景数据，用 `Filter Output` 输出结果。

#### 重要说明

- `Scene Color` 节点的默认采样坐标为`纹理坐标`节点的`Window`输出
- 支持`AOV`输入


## 二、主要扩展节点

**1. Filter 域节点**

### Scene Color

#### 入口

`Add > Input > Scene Color`

仅在 `Filter` 域下可用。

#### 作用

读取 Eevee 当前场景缓冲，可在节点面板中切换 `Source`：

- `Color`
- `Depth`
- `Normal`

#### 输入输出

- 输入：`Vector`
- 输出：`Color`、`Alpha`


**2. Eevee 通用辅助节点**

### Render Info

#### 入口

`Add > Input > Render Info`

在 `Eevee` 下可用。

#### 输出
- `frag coord`
- `Width`
- `Height`

#### 作用

提供当前 Eevee 渲染窗口的坐标和像素尺寸。

### Screen Derivative

#### 入口

`Add > Utilities > Math > Screen Derivative`

在 `Eevee` 下可用。

#### 功能

获得屏幕之间相邻像素之间的差异：

- `DDX`
- `DDY`
- `DDXY`

其中 `DDXY` 表示 `DDX + DDY`。


### Portal In / Portal Out

#### 入口

- `Add > Layout > Portal In`
- `Add > Layout > Portal Out`

#### 功能说明

这是一组用来整理节点连线的“传送门”节点。

工作方式可以理解为：

- `Portal In`：在当前节点树里存一个有名字、有类型的值
- `Portal Out`：在同一节点树内按名字把这个值取出来继续使用

#### 使用方法

1. 新建一个 `Portal In`。
2. 设置名称和数据类型。
3. 把原本要长距离拉线的值接入 `Portal In`。
4. 在别处添加一个或多个 `Portal Out`。
5. 让 `Portal Out` 使用同名、同类型设置。
6. 直接从 `Portal Out` 输出继续往后连。

#### 其他

- 新建 `Portal In` 时会自动生成唯一名称。
- `Portal Out` 上带有放大镜按钮，可快速跳转到对应的 `Portal In` 位置。

#### 限制

- 只在同一个 shader node tree 内识别。
- 不支持跨节点树。
- 不支持跨节点组自动穿透。
- 同名输入应只保留一个来源。

**3. Eevee 物体材质节点**

### Render Texture

#### 入口

`Add > Texture > Render Texture`

#### 作用

读取前面在场景里配置好的 `Render Textures` 条目。

#### 输入输出

- 输入：`Vector`
- 输出：`Color`、`Alpha`


### Screenspace Info

#### 入口

`Add > Input > Screenspace Info`

#### 输入输出

- 输入：`View Position`
- 输出：`Scene Color`、`Scene Depth`

#### 作用

获得当前的渲染缓冲颜色或深度的内容

#### 使用说明

- 渲染设置中需要打开`Raytracing`
- 材质选项`Render Method`选择`Dithered`
- 材质选项打开`Raytraced Transmission`
- `View Position`默认输入为:`position`变换到摄像机空间,再反转z轴

### Curvature

#### 入口

`Add > Input > Curvature`

在 `Eevee` 下可用，也可以直接在 `NPR Tree` 中使用。

#### 输入

- `Samples`
- `Sample Radius`
- `Thickness`
- `Scale`

#### 输出

- `Scene Curvature`
- `Scene Rim`

#### 面板选项

- `Local`

#### 作用

移植的Goo Engine中的曲率节点,提供曲率和边缘光输出

#### 说明

- `Local` 开启后，会尽量只按当前物体自身的信息计算
- 节点本质上是屏幕空间采样节点，结果会受到当前视角、屏幕分辨率和采样半径影响
- 直接观察时，通常 `Scene Rim` 会比 `Scene Curvature` 更容易看出效果

### Raycast

#### 入口

`Add > Input > Raycast`

在 `Eevee` 下可用，也可以直接在 `NPR Tree` 中使用。

#### 输入

- `Position`
- `Direction`
- `Length`

#### 输出

- `Is Hit`
- `Self Hit`
- `Hit Distance`
- `Hit Position`
- `Hit Normal`

#### 面板选项

- `Only Local`

#### 作用

基于 Eevee 屏幕空间信息发射射线，并返回命中结果。

#### 说明

- `Position` 默认使用世界坐标
- `Direction` 默认使用表面法线方向
- `Only Local` 开启后，会尽量只检测当前物体自身
- 这是屏幕空间节点，命中结果依赖当前视角和可见缓冲

### Shader Info

#### 入口

`Add > Input > Shader Info`

#### 输入

- `World Position`
- `Normal`

#### 输出

- `Diffuse Shading`
- `Shadow`
- `Ambient Lighting`
- `Half-Lambert Factor`

#### 各输出的含义

- `Diffuse Shading`
  - 每个灯光的兰伯特光照之和,再钳制到0-1
- `Shadow`
  - 输出抖动阴影
- `Ambient Lighting`
  - 来自探针 / 环境间接光的环境照明信息
- `Half-Lambert Factor`
  - 每个灯光的半兰伯特光照之和,再钳制到0-1

#### 额外说明

- 当前实现会排除 world sun 对这些输出的干扰，避免 HDRI 或世界环境里的“太阳光”混入直接结果。

### Light Info

#### 入口

`Add > Input > Light Info`

#### 功能说明

读取指定灯光信息

#### 固定输出

- `Color`
- `Power`
- `Type`

其中 `Type` 是整数插槽，含义为：

- `-1`：没有指定灯光
- `0`：Point
- `1`：Sun
- `2`：Spot
- `3`：Area

#### 按灯光类型自动出现的输出

- `Position`
- `Direction`
- `Radius`
- `Spot Size`
- `Sun Angle`

当前版本会根据灯光类型自动隐藏 / 显示相关接口：

- `Point`：`Position`、`Radius`
- `Sun`：`Direction`、`Sun Angle`
- `Spot`：`Position`、`Direction`、`Radius`、`Spot Size`
- `Area`：`Position`、`Direction`、`Radius`

#### 说明

- 如果你要做逐灯处理，应该使用 `NPR Tree` 里的 `For Each Light`。

## 三、NPR Tree 工作流

### 1. 基本概念

`NPR Tree` 是挂在普通物体材质之外的第二套表现节点树，用来对 Eevee 的材质结果做 NPR 风格重组和二次表现。

普通物体材质依然负责基础表面着色，`NPR Tree` 负责额外的 NPR 表现层。

### 2. 挂接方式

1. 在普通物体材质中保留正常的 `Material Output` 和基础表面着色。
2. 选中 `Material Output` 节点。
3. 在它的 `NPR Tree` 属性里新建或指定一个节点组。
4. 需要编辑这棵树时，在 Shader Editor 顶部把 `Shader Type` 切到 `NPR`。

### 3. 说明
 - 材质的渲染方式需要设置为`抖动(延迟渲染)`

### 4. 主要 NPR 节点

除了下面这些专用 NPR 节点以外，`Curvature` 和 `Raycast` 现在也可以直接在 `NPR Tree` 中使用。

### NPR Input

#### 作用

读取 NPR 渲染阶段提供的输入缓冲。

#### 输出

- `Combined Color`
- `Diffuse Color`
- `Diffuse Direct`
- `Diffuse Indirect`
- `Specular Color`
- `Specular Direct`
- `Specular Indirect`
- `Position`
- `Normal`

这些输出本质上更接近图像句柄 / 纹理句柄，适合继续交给 `Image Sample` 做邻域采样，或接到支持这类输入的 NPR 节点上继续处理。

### NPR Refraction

#### 作用

读取折射相关缓冲,类似`Screenspace Info`

#### 输出

- `Combined Color`
- `Position`

### Image Sample

#### 入口

`Add > Utilities > Image Sample`

#### 输入输出

- 输入：`Image`、`Offset`
- 输出：`Color`

#### 作用

对 `NPR Input` / `NPR Refraction` 之类输出的图像句柄做采样。

#### 偏移模式

- `View`：按视空间偏移
- `Pixel`：按像素偏移

### For Each Light

#### 入口

`Add > Utilities > For Each Light`

#### 说明

它会按当前影响表面的灯光逐个执行内部逻辑,每次循环输出一个灯光的信息

#### 内置可用信息

`For Each Light Input` 当前提供：

- 输入：`Normal`
- 输出：`Color`
- 输出：`Direction`
- 输出：`Distance`
- 输出：`Attenuation`
- 输出：`Shadow Mask`

此外还支持在区域输入 / 输出上增添自定义 socket，用于在逐灯循环内部传递你自己的中间量。


### 内置 NPR 节点组资产

当前版本已经把 Blender 4.4 NPR 版本中的一批常用节点组，迁移并整理成了 5.1 可用的资产包。

### 当前内置的主要节点组

- `Cavity`
- `Co-Planar Edge Detection`
- `Curvature`
- `Kuwahara`
- `Shading Models`
- `Surface Curvature`

### 资产说明

- 这些节点组已经按 Blender 5.1 的格式迁移。
- 由于重复区域节点名称修改了,4.4 npr原型的工程需要自己重新连接重新区域相关的节点


## 四、当前限制与注意事项

- 大部分功能是 `Eevee` 专用，不支持 `Cycles`。
- `Render Textures` 当前最多 `4` 个槽位。
- `Filter Materials` 只能使用 `Filter` 域材质。
- `Portal` 只在同一节点树内生效，不支持跨节点树和跨节点组自动穿透。
- `Screenspace Info`、`Scene Color`、`Screen Derivative`、`Curvature` 这类节点，本质上都依赖 Eevee 的屏幕空间或当前渲染缓冲信息。
- 反射探头只会捕获NPR Tree之前的材质效果
