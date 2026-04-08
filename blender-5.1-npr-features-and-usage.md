# Blender 5.1 NPR Port 新增功能与使用说明

## 文档范围

这份文档说明当前 `Blender 5.1 NPR Port` 相比官方 `Blender 5.1` 已经加入、并且当前分支内实际存在的 NPR / Eevee 扩展功能，以及它们的基本使用方法。


## 与官方 Blender 5.1 的主要区别

当前这个 5.1 NPR 版本，和官方 Blender 5.1 相比，主要多了以下几类能力：

1. `Eevee` 的场景级扩展工作流
   - `Render Textures`
   - `Filter Materials`

2. `Eevee` 的新着色器节点
   - `Filter Object Info`
   - `Render Info`
   - `Scene Time`
   - `Screen Derivative`
   - `World Environment`
   - `Light Probe Color`
   - `World To Tangent`
   - `Basis Transform`
   - `Bevel`

3. `Goo Engine` 移植节点
   - `Screenspace Info`
   - `Curvature`
   - `Raycast`
   - `Shader Info`
   - `Light Info`
   - `SDF Primitive`
   - `SDF Operator`
   - `SDF Vector Operator`

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

5. 界面整理节点
   - `Portal In / Portal Out`

6. 界面与工作流补充
   - `材质选择器预览开关`
   - `骨骼 Outliner 隐藏`


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
5. 在滤镜材质里使用 `Scene Color` 读取场景数据，也可以用 `Filter Object Info` 读取指定对象的变换 / 颜色信息。
6. 通过 `Execution Stage` 选择滤镜执行位置。

#### 重要说明

- `Scene Color` 节点的默认采样坐标为`纹理坐标`节点的`Window`输出
- 支持`AOV`输入
- `Execution Stage` 目前提供三个位置:
  - `Before Volume Fog`
  - `Before Depth of Field`
  - `Before Composite`


## 二、主要扩展节点

**1. Filter 域节点**

### Filter Object Info

#### 入口

`Add > Input > Filter Object Info`

仅在 `Filter` 域下可用。

<div align="center">
  <img src="docs/images/SnowShot_2026-04-01_20-29-09.png" alt="Filter Object Info" style="border-radius: 10px;">
  <br>
</div>

#### 作用

读取指定对象的世界空间变换和视口显示颜色，方便在 `Filter Materials` 中做基于对象状态的全屏滤镜控制。

它适合用来驱动遮罩中心、方向性渐变、跟随控制物体的位置特效，或者直接把对象颜色当作滤镜参数输入。

#### 节点设置

- 节点面板内可以指定一个 `Object`

#### 输出

- `Location`
- `Rotation`
- `Scale`
- `Color`

#### 输出说明

- `Location`：所选对象的世界空间位置
- `Rotation`：所选对象的世界空间欧拉旋转，单位为弧度
- `Scale`：所选对象的世界空间缩放
- `Color`：所选对象的视口显示颜色

#### 说明

- 这里读取的是指定对象的数据，不是当前正在被滤镜处理的对象
- 如果没有指定对象，会输出默认值：位置 / 旋转 / 颜色为 `0`，缩放为 `1`

### Scene Color

#### 入口

`Add > Input > Scene Color`

仅在 `Filter` 域下可用。

#### 作用

读取 Eevee 当前场景缓冲，可在节点面板中切换 `Source`：

- `Color`
- `Depth`
- `Normal`
- `Shadow`
- `Position`

`Shadow` 会读取 Eevee 的阴影渲染通道，输出 0-1 灰度阴影值，可直接用于 Toon 分层、阈值、混合等滤镜处理。
`Position` 会读取 Eevee 的世界空间位置通道，输出全局坐标。

#### 输入输出

- 输入：`Vector`
- 输出：`Color`、`Alpha`
- `Camera` 面板输出：`Camera Position`、`Axis X`、`Axis Y`、`Axis Z`

不连接 `Vector` 时，节点会继续使用 Render Texture 相机的默认投影自动采样。
`Camera Position` 与 `Axis X/Y/Z` 可用于在材质里把世界坐标转换到对应的 RT 相机空间，例如使用 `dot(P - Camera Position, Axis Z)` 计算该相机空间的 Z 值。


**2. Eevee 通用辅助节点**

### Render Info

#### 入口

`Add > Input > Render Info`

在 `Eevee` 下可用。

#### 输出
- `Frag Coord`
- `Width`
- `Height`

#### 作用

提供当前 Eevee 渲染窗口的坐标和像素尺寸。

#### 说明

- `Frag Coord.xy` 为归一化到 `0-1` 的屏幕 UV
- `Frag Coord.z` 为当前片元深度
- `Width` / `Height` 为当前渲染区域的像素尺寸

### Scene Time

#### 入口

`Add > Input > Scene Time`

在 `Eevee` 下可用。

#### 输入

- `Scale`

#### 输出

- `Frame`
- `Seconds`
- `Timeline`
- `Scaled Frame`

#### 作用

提供当前场景时间相关的数值输出。

#### 说明

- `Frame` 为当前帧数
- `Seconds` 为当前帧对应的秒数
- `Timeline` 会把场景开始帧到结束帧映射到 `0-1`
- `Scaled Frame` 为当前帧除以 `Scale` 之后的结果

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

### World Environment

#### 入口

`Add > Input > World Environment`

在 `Eevee` 物体材质和 `NPR Tree` 中可用。

#### 输入输出

- 输入：`Direction`
- 输出：`Color`

#### 作用

直接采样 `Eevee` 的世界环境颜色，不依赖屏幕背后是否还有几何。

#### 说明

- 适合获取被遮挡情况下的 world 环境颜色
- 不读取屏幕背后物体的颜色
- 输出更接近 `Eevee` 的环境 / probe 结果，而不是屏幕空间缓冲
- `Direction` 不连接时，默认使用当前表面的视线方向
- `Direction` 连接后，可以按指定方向采样世界环境

### Light Probe Color

#### 入口

`Add > Input > Light Probe Color`

在 `Eevee` 物体材质和 `NPR Tree` 中可用。

#### 输入输出

- 输入：`Direction`
- 输出：`Reflection`、`Irradiance`、`Combined`

#### 作用

直接读取 `Eevee` 当前可用的光照探头结果，分别输出反射探头颜色、环境谐波漫反射颜色，以及两者叠加后的结果。

#### 说明

- `Reflection` 更接近反射探头 / 世界环境方向采样结果
- `Irradiance` 更接近体积光照探头或环境谐波的漫反射光照结果
- `Combined` 为 `Reflection + Irradiance`
- `Direction` 不连接时：
  - `Reflection` 默认使用当前表面的视线方向
  - `Irradiance` 默认使用当前表面的着色法线方向
- `Direction` 连接后，可以按指定方向采样 probe 信息

### World To Tangent

#### 入口

`Add > Utilities > Vector > World To Tangent`

在 `Eevee` 物体材质和 `NPR Tree` 中可用。

#### 输入输出

- 输入：`Vector`
- 输出：`Vector`

#### 作用

把一个世界空间方向向量转换到当前表面的切线空间。

#### 说明

- 主要用于把世界空间方向改写成以 `Tangent / Bitangent / Normal` 为基底的局部方向
- 节点面板中可指定 `UV Map`，该 UV 的切线会作为转换基底
- 适合拿来做各向异性方向控制、切线空间流向、局部扫描方向等效果
- 当前版本处理的是“向量 / 方向”变换，不是“位置点”变换
- 普通网格需要有效的 UV 切线数据；曲线 / 毛发会优先使用已有的曲线切线基底
- 如果对象没有可用切线基底，结果会退化为 `0`

### SDF Primitive

#### 入口

`Add > Texture > SDF Primitive`

#### 输出

- `Distance`

#### 作用

在材质节点里直接生成符号距离场（SDF）基础形体，适合做程序遮罩、轮廓、形体过渡，以及后续布尔组合的基础输入。

#### 主要模式

- 3D 形体：`Sphere`、`Box`、`Torus`、`Cone`、`Point Cone`、`Cylinder`、`Point Cylinder`、`Capsule / Line`、`Octahedron`、`Hex Prism`、`Hex Prism Incircle`、`Plane`、`Solid Angle`、`Pyramid`、`Disc`、`3D Circle`
- 2D 形体：`Circle`、`Rectangle`、`Ellipse`、`Triangle`、`Pentagon`、`Hexagon`、`Isosceles Triangle`、`Trapezoid`、`Rhombus`
- 风格化 2D：`Star`、`Heart`、`Pie`、`Arc`、`Moon`、`Vesica`、`Cross`、`Rounded X`、`Horseshoe`、`Round Joint`、`Flat Joint`
- 曲线 / 片段：`Line`、`Corner`、`Quadratic Bezier`、`Point Triangle`、`Quad`、`Parabola`、`Parabola Segment`、`Uneven Capsule`

#### 输入说明

- 固定基础输入为 `Vector`
- 其余插口会按模式动态显示并重命名，常见参数有 `Size`、`Radius`、`Angle`、`Roundness`、`Linewidth`、`Point` 到 `Point_003`、`Value1` 到 `Value4`
- 节点面板提供 `Mode` 和 `Invert`

#### 使用说明

- 输出的是距离值，不是颜色
- 通常配合 `Math`、`ColorRamp`、`Map Range`、`SDF Operator` 等节点，把距离场转换成遮罩或最终图形
- `Invert` 可直接翻转内外关系，方便把同一形体改成“孔”或“壳”

### SDF Operator

#### 入口

`Add > Converter > SDF Operator`

#### 输出

- `Distance`

#### 作用

对一个或两个 SDF 距离场做组合、裁切和轮廓变形，用来把多个基础形体继续拼成更复杂的结果。

#### 主要运算

- 单输入：`Dilate`、`Onion`、`Annular`、`Mask`、`Flatten`、`Invert`、`Hermite Pulse`
- 双输入：`Blend`、`Exclusion XOR`、`Divide`、`Pipe`、`Engrave`、`Groove`、`Tongue`
- 并集：`Union`、`Smooth Union`、`Round Union`、`Columns Union`、`Stairs Union`、`Chamfer Union`
- 交集：`Intersect`、`Smooth Intersect`、`Round Intersect`、`Columns Intersect`、`Stairs Intersect`、`Chamfer Intersect`
- 差集：`Difference`、`Smooth Difference`、`Round Difference`、`Columns Difference`、`Stairs Difference`、`Chamfer Difference`

#### 输入说明

- 基础输入包含 `Distance`、`Distance_001`、`Value`、`Value_001`、`Count`
- 实际显示的插口名称和数量会随 `Operation` 自动变化
- `Mask` 模式会额外显示 `Invert`

#### 使用说明

- 常见流程是先用多个 `SDF Primitive` 生成距离场，再通过 `SDF Operator` 做并集、交集或差集
- `Smooth`、`Round`、`Chamfer`、`Stairs`、`Columns` 这类模式适合做更有风格化过渡的布尔边界
- 最终依然输出距离值，通常还需要再接阈值、颜色映射或透明度控制节点

### SDF Vector Operator

#### 入口

`Add > Utilities > Vector > SDF Vector Operator`

#### 输出

- `Vector`
- `Position`
- `Value`

#### 作用

在进入 `SDF Primitive` 之前，先对采样坐标、UV 或向量域做重复、镜像、旋转、扭曲、平铺和范围映射等处理。

它更像是 SDF 工作流里的“坐标预处理器”：

- 先改写空间
- 再生成 SDF 形体
- 最后用 `SDF Operator` 继续组合距离场

#### 主要模式

- 网格 / 重复：`Plane Reflect`、`Mirror`、`Polar`、`Repeat Infinite`、`Repeat Infinite Mirror`、`Repeat Finite`、`Octant`
- 形变 / 变换：`Swizzle`、`Rotate`、`Spin`、`Extrude`、`Twist`、`Swirl`、`Pinch Inflate`、`Radial Shear`、`Bend`
- UV 处理：`UV Rotate`、`UV Scale`、`UV Grid`、`UV Random Rotate`、`UV Random Flip`、`UV Tileset`
- 范围映射：`Map -1-1`、`Map -0.5-0.5`、`Map 0-1`

#### 输入输出说明

- 固定基础输入为 `Vector`
- 其余插口会按模式动态显示并重命名，常见名称有 `Spacing`、`Count`、`Center`、`Offset`、`Normal`、`Strength`、`Radius`、`Index`、`Padding`、`Scale`
- `Vector` 输出表示处理后的坐标或 UV，可直接继续送入 `SDF Primitive` 或其他程序节点
- `Position` 只在部分模式出现，通常用于输出格子坐标或镜像 / 分块后的辅助位置
- `Value` 只在部分模式出现，含义会随模式变化，常见是遮罩值、极坐标分段辅助值，或 `Extrude` 的内部距离

#### 面板选项

- `Operation`：选择当前坐标处理模式
- `Axis`：只在依赖主轴的模式里出现，用来指定当前操作基于哪组轴顺序处理

#### 使用说明

- 常见流程是 `Texture Coordinate / Object` -> `SDF Vector Operator` -> `SDF Primitive` -> `SDF Operator`
- `Repeat`、`Mirror`、`Polar`、`Octant` 适合做规则重复、轴对称、环形重复和象限对称，不需要真的复制几何
- `Rotate`、`Spin`、`Twist`、`Swirl`、`Bend` 适合先把空间扭曲，再让基础形体沿扭曲后的空间生成
- `UV Grid`、`UV Random Rotate`、`UV Random Flip`、`UV Tileset` 适合做图案平铺、瓦片随机朝向和单张贴图分块复用
- `Map -1-1`、`Map -0.5-0.5`、`Map 0-1` 适合在 UV 范围和 SDF 常见坐标范围之间快速切换

### Bevel

#### 入口

`Add > Input > Bevel`

在 `Eevee` 下可用。

#### 输入输出

- 输入：`Radius`、`Normal`
- 输出：`Normal`

#### 面板选项

- `Samples`

#### 作用

在 `Eevee` 中生成近似的倒角法线，用来让硬边看起来更圆润。

#### 说明

- `Cycles` 仍然使用官方原本的真实几何倒角算法
- `Eevee` 这里使用的是同物体屏幕空间近似
- 结果依赖当前视角、深度缓冲和可见邻域，不等同于 `Cycles` 的真实 `Bevel`

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
- `Bevel Normal`

#### 面板选项

- `Local`

#### 作用

移植的Goo Engine中的曲率节点,提供曲率和边缘光输出

#### 说明

- `Local` 开启后，会尽量只按当前物体自身的信息计算
- `Bevel Normal` 会输出一个基于屏幕空间邻域重建的近似倒角法线
- `Bevel Normal` 适合接到点乘、假倒角、高光偏移等用法里
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
  - 可切换阴影模式
  - `Built-in` 默认模式，使用 Eevee 原本的阴影计算
  - `Stable` 特殊模式，输出稳定的0-1灰度阴影遮罩，可直接拿去做toon阶梯、阈值和半影着色
  - `Soft Filtered` 在 `Stable` 基础上，对当前表面附近的一像素邻域做额外采样和平均，把黑白抖动阴影变成更平滑的灰度半影
- `Ambient Lighting`
  - 来自探针 / 环境间接光的环境照明信息
- `Half-Lambert Factor`
  - 每个灯光的半兰伯特光照之和,再钳制到0-1

#### 额外说明

- 节点面板新增 `Shadow Mode`
  - `Built-in` / `Stable` / `Soft Filtered`
- 当 `Shadow Mode = Stable` 或 `Soft Filtered` 时，可用 `Stable Samples` 提高阴影质量
- `Soft Filtered` 更适合把单帧黑白阴影重建成连续灰度，但性能会比 `Stable` 更高
- 节点面板新增整数 `Lightgroup`
  - 只有 `Lightgroup ID` 相同的灯光，才会参与这个 `Shader Info` 节点的直接光照与阴影计算
  - 默认值为 `0`，表示只接收 `Lightgroup ID = 0` 的灯光
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
 - `NPR Tree` 的关键帧与驱动器使用独立的 `NPR Tree Action`，可在 `Material Properties > Animation > NPR Tree Action` 查看、切换或新建。

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


## 四、界面与工作流补充

### 1. 材质选择器预览开关

#### 作用

控制材质下拉列表 / 搜索列表中是否渲染材质预览图。

这个开关主要用于在材质很多时，减少展开材质选择器时的预览生成开销。

#### 入口

`Edit > Preferences > Editing > Objects > Materials > Material Selector Previews`

#### 行为说明

- 开启时：材质选择器会按当前逻辑显示材质预览图。
- 关闭时：材质选择器会退回普通材质图标，不再在下拉列表里触发材质预览渲染。
- 默认值为开启。

#### 当前范围

- 目前只影响 `template_ID(...)` 这类材质选择器下拉列表中的材质预览显示。
- 不影响 `Material Properties` 面板中的大预览球。
- 不影响材质本身的正常渲染结果。

### 2. Eevee 灯光 Lightgroup ID

#### 作用

为 Eevee 灯光指定一个整数灯光组编号，供 `Shader Info` 节点做分组过滤。

#### 入口

`Light Data > Light > Lightgroup ID`

#### 行为说明

- 默认值为 `0`
- `Shader Info` 节点的 `Lightgroup` 也为 `0` 时，只会计算 `Lightgroup ID = 0` 的灯光
- 如果某个 `Shader Info` 节点设置为其他整数值，则只有相同编号的灯光会参与该节点计算
- 这个分组过滤当前只影响 `Shader Info` 节点，不会改动 Eevee 普通材质主通道的默认灯光结果

### 3. 启动图版本标识

#### 作用

在启动图右上角的版本文字后追加当前 NPR 构建标识与构建日期，方便区分自定义构建版本。

#### 当前显示格式

- `版本号 + npr post + 构建日期`
- 例如：`5.1.0 npr post 2026-03-27`

### 4. 骨骼 Outliner 隐藏

#### 作用

为每个 `Pose Bone` 增加单独的 Outliner 隐藏标记，用来整理复杂绑定的层级显示。

它适合把机制骨、辅助骨或不需要频繁查看的控制层从 Outliner 中隐藏，只保留更重要的骨架结构。

#### 入口

- `Bone Properties > Viewport Display > Hide in Outliner`
- `Outliner > Filter > Hidden PoseBones`

#### 行为说明

- 每个 `Pose Bone` 都有自己的 `Hide in Outliner` 开关。
- 这个开关默认是开启的。
- `Outliner` 里的 `Hidden PoseBones` 过滤项默认也是开启的，所以默认不会立刻改变现有骨架的显示结果。
- 当关闭 `Outliner > Filter > Hidden PoseBones` 后，勾选了 `Hide in Outliner` 的姿态骨骼会从 Outliner 树中隐藏。
- 如果某个被隐藏的父骨骼仍然有可见子骨骼，可见子骨骼会继续保留在树里，不会整支层级一起消失。

#### 当前范围

- 当前只作用于 `Pose Bone`
- 不作用于 `Edit Bone`
- 只改变 `Outliner` 的层级显示，不影响骨骼的变换、动画、驱动器和渲染结果


## 五、当前限制与注意事项

- 大部分功能是 `Eevee` 专用，不支持 `Cycles`。
- `Render Textures` 当前最多 `4` 个槽位。
- `Filter Materials` 只能使用 `Filter` 域材质。
- `Portal` 只在同一节点树内生效，不支持跨节点树和跨节点组自动穿透。
- `Screenspace Info`、`Scene Color`、`Screen Derivative`、`Curvature`、`Bevel` 这类节点，本质上都依赖 Eevee 的屏幕空间或当前渲染缓冲信息。
- 反射探头只会捕获NPR Tree之前的材质效果
