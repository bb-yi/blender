# Blender NPR Port GLSL Function 节点转换指南

## 文档目的

这份文档是写给 `AI`、脚本生成器、自动转换工具的。

目标不是介绍着色器原理，而是让另一个 AI 在阅读这份文档后，能够把：

- 普通 `GLSL`
- `Shadertoy GLSL`
- `HLSL`
- 一部分 `ShaderLab / Unity CGPROGRAM / HLSLPROGRAM`

稳定地转换成可直接粘贴到 `Blender NPR Port` 的 `GLSL Function` 节点里的、符合当前实现规范的 `GLSL` 源码。


## 一、先记住当前节点真正支持什么

`GLSL Function` 节点当前是“把一段用户 GLSL 函数源码注入到 Eevee 材质编译链里”。

它不是完整的 Shadertoy 运行时，也不是完整的 Unity / Unreal shader framework。

### 1. 当前可用的函数接口类型

选中的导出函数，其参数和返回值只允许使用以下边界类型：

- 输入参数 `in`
  - `float`
  - `int`
  - `bool`
  - `vec2`
  - `vec3`
  - `vec4`
  - `mat2`
  - `mat3`
  - `mat4`
  - `sampler2D`
- 输出参数 `out`
  - `float`
  - `int`
  - `bool`
  - `vec2`
  - `vec3`
  - `vec4`
  - `mat2`
  - `mat3`
  - `mat4`
- 返回值
  - `void`
  - `float`
  - `int`
  - `bool`
  - `vec2`
  - `vec3`
  - `vec4`
  - `mat2`
  - `mat3`
  - `mat4`

### 2. 当前不支持的函数接口写法

- `inout`
- `out sampler2D`
- `struct` / `array` 作为函数边界类型
- 非方阵矩阵（例如 `mat2x3`）作为函数边界类型
- 多返回值结构体
- 递归
- 依赖外部运行时注入的全局 uniform 体系
- 历史 `sample2D` 旧写法不再作为公开函数边界类型，统一改写为 `sampler2D`

### 3. 当前和 UI 相关的重要事实

- `GLSL Function` 节点只允许用于 Eevee 的 Object、Filter 和 NPR 节点域；World 节点树当前不支持
- `Function` 现在必须显式指定，不会自动选第一个函数
- `sampler2D` 会显示为 `Closure` 输入口
- `sampler2D` 可连接 `Image to Closure` 或符合约定的 `Closure Output`
- `Closure Output -> sampler2D` 当前只保证 `texture(tex, coord)` 这种直接采样形式；`coord` 的语义由闭包内部决定，普通贴图通常是 UV，NPR 图像句柄通常是 `Image Sample.Offset`
- 如果函数依赖显式 `LOD`、`grad` 或尺寸查询等图像专用能力，应优先使用 `Image to Closure`
- 导出函数边界当前已经支持 `int / bool`，适合直接作为模式开关、枚举值、`lightgroup_id` 这类参数
- 默认情况下 `vec2/vec3/vec4` 仍然显示为“向量插口”
- 只有显式写了 `subtype=color` 的 `vec3` 输入，才会显示为颜色插口
- `vec3 + subtype=color` 会显示为颜色插口，内部按 `rgb` 使用，`alpha` 固定为 `1.0`
- `vec4` 输入会显示为 `vec3 + float W` 两个插口，并在 GLSL wrapper 内重组为 `vec4`
- `mat2/mat3/mat4` 边界会按列拆分为多个向量插口；`mat4` 的每列会进一步拆成 `vec3 + float W`

### 4. `Node / Code` 编辑模式与刷新生命周期

- 节点顶部的 `Node / Code` 分段控件用于切换普通节点界面和源码编辑界面。
- 内部 `Text` 数据块可以在 `Code` 模式直接编辑。编辑内容先进入节点草稿，不会立即改写当前运行中的 `Text`、函数列表或 socket。
- 草稿变脏后，执行 `Apply`（刷新）会先解析并验证整次变更，再原子更新源码和 socket；`Discard` 会丢弃草稿并恢复当前已应用源码。
- 解析或 GPU 编译失败时，旧的 `Text` 与 socket 保持可用，不会留下半更新状态。
- 多个节点共享同一个 `Text` 时，Apply 会验证并刷新所有使用者；如果存在另一个未处理的并发草稿，本次提交会被拒绝，避免静默覆盖。
- 外部 `.glsl` 文件在 `Code` 模式中只读；需要节点内编辑时，先使用 `Make Internal` 转为内部 `Text`。
- 未提交草稿会随 `.blend` 保存并恢复，但只有 Apply 成功后的源码参与实际编译和渲染。

### 5. 内部实现和边界接口要区分

虽然函数边界只支持上面那几种类型，但函数体内部可以正常使用很多普通 GLSL 语法，比如：

- `if`
- `for`
- `while`
- `int`
- `bool`
- `uint` / `uvec*`
- `mat2` / `mat3` / `mat4`
- 局部辅助函数
- `mix` / `clamp` / `fract` / `texture` 等常见 GLSL 内建
- `textureLod` / `dFdx` / `dFdy` 等函数体内部用法

也就是说：

- “函数内部”可以相对自由
- “节点导出函数的参数和返回值”必须使用上方支持的边界类型
- “辅助函数”不必都遵守导出函数的边界类型限制；真正必须满足限制的是节点 `Function` 最终选中的那个函数
- 因此辅助函数里可以出现 `bool` / `int` / `mat*` / 非方阵矩阵 / 更自由的 `out` 参数组合，但最终报告里必须再次明确真正要调用的是哪个导出函数


## 二、AI 转换时必须遵守的总规则

如果你是另一个 AI，要把任意 shader 代码转换给这个节点用，必须遵守以下规则。

### 规则 1：最终输出必须是“普通 GLSL 函数源码”，不是完整 shader 文件

最终结果里不要保留这些内容：

- `main()`
- `mainImage()`
- `frag()`
- `vert()`
- `Pass`
- `SubShader`
- `Properties`
- `CGPROGRAM`
- `HLSLPROGRAM`
- `ENDCG`
- `ENDHLSL`
- `#version`
- `precision highp/mediump/lowp ...`
- `layout(...)`
- 平台 pragma
- 渲染状态声明
- engine include
- `/** SHADERDATA ... */`
- `//proc:...`、`//proc:noise:...` 这类资源生成或工具元数据注释

最终应只保留：

- 常量
- 必要的辅助函数
- 一个准备给节点选择的导出函数

### 规则 2：必须给出明确的导出函数名

因为节点不会自动选择第一个函数，所以 AI 输出时必须同时告诉使用者：

- `Function` 应该设成哪个名字

推荐输出格式：

```text
Function: your_function_name
```

### 规则 2A：导出函数必须补全 `@glsl_meta v1`

只要最终导出函数有输入参数或 `out` 参数，AI 输出的代码就必须在导出函数正上方补一块完整的 `@glsl_meta v1`。

不要等用户额外提醒才写 Meta。Meta 是默认输出的一部分，不是可选装饰。

Meta 必须覆盖导出函数的公开接口：

- 每个输入参数都应有一行 Meta，说明 `label` 和必要的 `description`
- 数值调节参数应尽量写 `default`，并在语义明确时写 `min` / `max`
- `0..1` 混合、阈值、强度、柔和度等参数应优先写 `subtype=factor`
- 颜色参数优先用 `vec3` 并写 `subtype=color`；需要 alpha 时使用 `vec4` 或拆成 `vec3 color + float alpha`
- 坐标、方向、比例等向量参数应写清楚语义；不要给方向/法线参数写 `subtype=direction`，这个 subtype 会触发不友好的球形方向控件；需要普通三轴向量 UI 时用 `subtype=xyz` 或省略 subtype
- 固定模式类 `int` 参数应优先写 `items="0:Label;1:Other"`，让默认值显示成下拉菜单；只有连续数值范围才用普通 `min/max` 整数框
- `items` 下拉默认隐藏前置参数名，只显示当前选项；当同一行没有足够上下文或面板里有多个相似下拉时，再写 `show_label=true`
- `sampler2D` 只能写 `label`、`description`，以及放进 panel；不要写 `default/min/max/hide_value/subtype`
- `out` 参数只能写 `label`，不要写默认值、范围、subtype 或 description
- 返回值不支持 Meta，必须在 `Outputs` 和结果说明里写清楚
- 参数超过 4 个，或语义能自然分组时，应使用一级 `@panel` / `@end_panel` 分组；高级/很少调节的参数默认 `closed=true`，主要工作流参数可以 `closed=false`
- 编译期开关、会影响 helper 函数或需要 `#if/#ifdef` 裁剪的选项，应写进独立 `@glsl_defines` 块，而不是混进函数参数 Meta

如果导出函数确实没有任何输入参数和 `out` 参数，可以省略 Meta，但结果说明里要写明“无可标注接口”。

### 规则 3：所有外部输入都改成函数参数

不要依赖下面这些外部名字直接存在：

- `iTime`
- `iResolution`
- `iMouse`
- `u_time`
- `u_resolution`
- `u_mouse`
- `_Time`
- `_ScreenParams`
- `_MainTex_ST`
- 自定义 `uniform`
- Unity 的内置矩阵和语义变量

必须把它们改成导出函数参数。

例如：

- `iTime` -> `float time`
- `iTimeDelta` -> `float time_delta`
- `iFrame` -> `float frame`
- `iResolution.xy` -> `vec2 resolution`
- `fragCoord` -> `vec2 frag_coord`
- `_MainTex` -> `sampler2D tex`
- `i.uv` -> `vec2 uv`
- `iChannelResolution[0].xy` -> `vec2 channel0_resolution`

如果源码通过别名宏间接依赖这些运行时变量，也要先展开再参数化，例如：

- `#define time iTime` -> 删除宏，直接使用 `float time`
- `#define res iResolution.xy` -> 删除宏，直接使用 `vec2 resolution`
- `#define frag gl_FragCoord.xy` -> 删除宏，改成显式参数 `vec2 frag_coord`

### 规则 4：如果来源代码依赖贴图采样，统一落成 `sampler2D`

例如：

- HLSL `Texture2D + SamplerState`
- Unity `sampler2D _MainTex`
- GLSL `sampler2D`

如果只是“普通静态 2D 图片，由节点面板直接选图”，优先改成：

```glsl
sampler2D tex
```

普通贴图的基础采样通常都应改成：

```glsl
texture(tex, uv)
```

如果原算法明确依赖显式 `LOD`、模糊级别、景深级别等采样语义，也可以在函数体内部保留例如：

```glsl
textureLod(tex, uv, lod)
```

也就是说：

- 公开函数边界统一使用 `sampler2D`
- `sampler2D` 的来源可以是 `Image to Closure`，也可以是符合约定的 `Closure Output`
- 普通贴图在函数体内部通常写成 `texture(tex, uv)`
- 函数体内部不必强行把所有采样都降级成最基础的 `texture`

- 不要再生成 `sample2D` 作为公开函数参数类型。
- 不要生成“`sampler2D` 能随便接任何 Closure”的说明；对程序化来源，当前只推荐 `Closure Output`。
- 如果用了 `textureLod` / `textureGrad` / `textureSize` / `texelFetch` 一类图像专用能力，应默认提醒使用 `Image to Closure`。

### 规则 4A：NPR Tree 里的图像句柄采样仍可用 `sampler2D`，但坐标是偏移量

这里必须先区分两类完全不同的“采样”：

- 普通静态图片、贴图、LUT、噪声图：按上面的规则转成 `sampler2D tex`，函数里使用 `texture(tex, uv)`，外部直接接 UV。
- `NPR Tree` 中来自 `NPR Input`、`AOV Input`、`NPR Refraction`、`NPR SSS Input`、渲染结果或 NPR 运行时缓冲的输出：也可以包装成 `sampler2D` 闭包输入，但 `texture(image, coord)` 里的 `coord` 应按 `Image Sample.Offset` 理解，不是 mesh UV。

后一类输出在节点图里是图像句柄 / `TextureHandle` 语义。它们不能作为 `GLSL Function` 的公开边界类型直接传入，但可以通过 `Closure Output -> sampler2D` 间接采样。闭包内部的采样节点是 `Image Sample`。

典型闭包节点连接应写成类似：

```text
Closure Input.UV -> Image Sample.Offset
NPR Input.Combined Color / AOV Input.Color / NPR Refraction.Combined Color -> Image Sample.Image
Image Sample.Color -> Closure Output.Color
Closure Output -> GLSL Function 的 sampler2D 输入
```

这里 `Closure Input` 的接口项仍然叫 `UV`，但在这个闭包里它只是承载 GLSL `texture(image, coord)` 的第二个参数。因为它被接到了 `Image Sample.Offset`，所以语义应写成 `offset`。

`Image Sample.Offset` 表示**相对当前像素 / 当前采样点的偏移**，不是绝对 UV：

- `Offset = vec3(0.0, 0.0, 0.0)`：采样当前像素。
- `Pixel` 模式下 `Offset = vec3(1.0, 0.0, 0.0)`：采样右侧 1 个像素，不是 `uv.x = 1.0`。
- `Pixel` 模式下 `Offset = vec3(-1.0, 0.0, 0.0)`：采样左侧 1 个像素。
- `View` 模式下 `Offset` 按节点暴露的视图 / 屏幕空间偏移语义工作，不能直接把它解释成 mesh UV。

因此 NPR Tree 图像句柄的 GLSL Function 示例应写成：

```glsl
vec4 npr_sample_current_pixel(sampler2D image)
{
  return texture(image, vec2(0.0));
}

vec4 npr_sample_pixel_right(sampler2D image, float radius)
{
  return texture(image, vec2(radius, 0.0));
}
```

如果写邻域采样、高斯模糊、边缘检测、SSAO 之类效果，函数参数也应命名为 `offset` / `radius` / `pixel_radius`，不要命名成 `uv`：

```glsl
vec4 npr_gaussian_blur_3x3(sampler2D image, vec2 offset, float radius)
{
  float r = max(radius, 0.0);
  vec4 sum = vec4(0.0);
  sum += texture(image, offset + vec2(-r, -r)) * 1.0;
  sum += texture(image, offset + vec2( 0, -r)) * 2.0;
  sum += texture(image, offset + vec2( r, -r)) * 1.0;
  sum += texture(image, offset + vec2(-r,  0)) * 2.0;
  sum += texture(image, offset + vec2( 0,  0)) * 4.0;
  sum += texture(image, offset + vec2( r,  0)) * 2.0;
  sum += texture(image, offset + vec2(-r,  r)) * 1.0;
  sum += texture(image, offset + vec2( 0,  r)) * 2.0;
  sum += texture(image, offset + vec2( r,  r)) * 1.0;
  return sum / 16.0;
}
```

如果原始屏幕 shader 写的是：

```glsl
texture(buffer, uv + delta_pixels / resolution)
```

并且用户明确说明这是 `NPR Tree` 中采样 `NPR Input` / `AOV` 这类缓冲，那么转换报告应写成：

- `buffer` 来源改为 `NPR Input` / `AOV Input` / 对应 NPR 图像句柄，再通过 `Image Sample -> Closure Output -> sampler2D` 提供给 GLSL Function。
- `uv + delta_pixels / resolution` 不要按 mesh UV 翻译；应把第二个参数改成 NPR 采样偏移，例如 `offset + delta_pixels`。
- 当前像素采样用 `texture(buffer, vec2(0.0))` 或 `texture(buffer, offset)`，其中 `offset` 默认为 `vec2(0.0)`。
- 邻域采样优先用 `Pixel` 模式，例如 `texture(buffer, offset + delta_pixels)`。
- 只有在确实需要视图空间偏移时，才说明使用 `View` 模式；不要把归一化 UV 偏移直接塞给 `Pixel` 模式。

因此：

- 不要因为没有 UV 就判定这类 `NPR Tree` 采样不能转换；这里的坐标本来就是偏移量。
- 不要因为它使用 `sampler2D` 就自动把第二个参数命名成 `uv`。
- 不要把 mesh UV 直接接到闭包内部的 `Image Sample.Offset`，除非用户明确要用 UV 值伪装偏移。
- 转换报告应写清楚：图像句柄采样发生在 `Image Sample` 闭包内部，GLSL 函数边界仍然是 `sampler2D image`，但 `texture(image, offset)` 的 `offset` 是 NPR 偏移量。

### 规则 4.1：如果要使用 Eevee 逐灯辅助接口，必须把范围说清楚

当前 `GLSL Function` 额外提供了一组 **Eevee 直接光 helper API**，目的是让函数体能在受控范围内读取逐灯直接光信息。

这组接口目前包括：

- `GLSLLight`
- `glsl_light_count()`
- `glsl_light_get(light_index)`
- `glsl_light_shadow(light_index, shading_normal)`
- `GLSL_LIGHT_TYPE_INVALID`
- `GLSL_LIGHT_TYPE_SUN`
- `GLSL_LIGHT_TYPE_POINT`
- `GLSL_LIGHT_TYPE_SPOT`
- `GLSL_LIGHT_TYPE_AREA_RECT`
- `GLSL_LIGHT_TYPE_AREA_ELLIPSE`
- 旧的逐灯宏接口和 `glsl_light_color(...)` 这类单字段 helper 已移除，不再作为公开用法支持
- `GLSLLight.vector` 表示**从当前着色点指向灯中心的归一化方向**
- `GLSLLight.type` 表示稳定的公开灯光类型：
  - `GLSL_LIGHT_TYPE_SUN`
  - `GLSL_LIGHT_TYPE_POINT`
  - `GLSL_LIGHT_TYPE_SPOT`
  - `GLSL_LIGHT_TYPE_AREA_RECT`
  - `GLSL_LIGHT_TYPE_AREA_ELLIPSE`
- `GLSLLight.lightgroup_id` 表示这盏灯的整数 `Lightgroup ID`，可直接用于自定义逐灯过滤
- `GLSLLight.position` 表示**灯中心世界坐标**；日光没有有限位置，因此返回 `vec3(0.0)`
- `GLSLLight.direction` 表示**灯的世界空间朝向轴**：
  - 日光：`sun().direction`
  - 聚光 / 面光：灯对象局部 `+Z` 轴对应的世界空间方向
  - 点光：返回 `vec3(0.0)`
- `GLSLLight.diffuse_color` 表示**对自定义逐灯模型友好的 diffuse 颜色项**：它会把 Eevee 内部的 surface radiance 权重换算成更接近 point-like 直觉的通道能量，避免点光默认半径很小时数值爆炸
- `GLSLLight.specular_color` 表示**对自定义逐灯模型友好的 specular 颜色项**：同样会把 Eevee 内部的 surface radiance 权重换算成更适合手写高光模型的通道能量
- `GLSLLight.attenuation` 表示**适合自定义逐灯模型的基础衰减项**；它内部组合了 `light_point_light(...)` 和 `light_attenuation_surface(...)`，但不包含 `NdotL`、toon ramp、half-lambert、Blinn-Phong、GGX、`light_attenuation_facing(...)`、`light_ltc(...)`、shadow 或材质侧 Fresnel / IOR / metallic / tint / roughness
- 推荐漫反射写法：`light.diffuse_color * light.attenuation * max(dot(N, light.vector), 0.0) * glsl_light_shadow(...)`
- 推荐高光写法：`light.specular_color * light.attenuation * custom_spec_term * glsl_light_shadow(...)`
- `glsl_light_count()` / `glsl_light_get(i)` 返回的是**当前片元局部可见灯列表**，不是场景全局稳定灯编号
- 因此 `glsl_light_get(0)` 的含义是“当前片元局部列表里的第 0 盏有效灯”，不是“场景里固定编号的第 0 盏灯”

必须同时明确下面几点：

- 这只是 **Eevee 直接光 helper**，不是公开 `LightData`、`light_buf` 或 Eevee 内部宏
- V1 只支持普通 `Eevee` 物体材质
- V1 只覆盖 `Deferred` / `Forward` 路径中的 **direct light + shadow**
- 对这套 **direct light helper** 来说，`FILTER`、`NPR Tree`、`World` 当前都不支持，而且它本身也不负责 probe / indirect / volume lighting

因此：

- 不要生成“可以直接访问 Eevee 全部灯光内部数据结构”的说明
- 不要生成“在任意 shader 域都支持逐灯 helper”的说明
- 不要把 indirect / probe / volume 结果说成这套 helper 的能力范围

### 规则 4.2：如果只是想在函数体里读几何体输入，优先使用内置几何 helper

当前 `GLSL Function` 还提供了一组**几何 helper**，目的是让函数体直接读取常用几何输入，而不是额外再拉一圈节点线。

这组接口目前包括：

- `glsl_position()`
- `glsl_normal()`
- `glsl_true_normal()`
- `glsl_incoming()`

它们的语义直接对齐内置 `Geometry` 节点：

- `glsl_position()` = `Geometry.Position`
- `glsl_normal()` = `Geometry.Normal`
- `glsl_true_normal()` = `Geometry.True Normal`
- `glsl_incoming()` = `Geometry.Incoming`

使用这组 helper 时要明确：

- 这是**几何输入快捷接口**，不是新的导出函数边界类型
- 它的优势是函数体里更紧凑，不代表外部节点输入方式被废弃
- 如果某段逻辑本身就更适合由节点图显式传参，例如要让同一个函数复用外部改写后的法线、位置或别的来源，仍然应该保留普通输入参数
- 当前这组几何 helper 按普通 `Eevee` 物体材质和 `NPR Tree` 的几何语义工作；`FILTER`、`World` 不作为稳定保证范围

因此：

- 不要把它说成“任意域都稳定可用”的全局 GLSL 内建
- 不要把它和 `Texture Coordinate`、`Camera`、`Object Info` 等别的输入节点混成一组等价接口

### 规则 4.3：如果要读 `Shader Info` 那种环境光，使用 `glsl_ambient_lighting()`

当前 `GLSL Function` 还提供了一个**环境光 helper**：

- `glsl_ambient_lighting()`

它的语义对齐 `Shader Info` 的 `Ambient Lighting` 输出，也就是当前着色点的 probe / 环境**间接漫反射**结果。

使用这条接口时要明确：

- 它只返回这类环境漫反射项，不包含 reflection probe 反射颜色
- 它也不等于 `Light Probe Color` 的 `Combined`
- 它依赖 Eevee 的 light probe 数据路径，因此应当按 light-probe 相关能力来描述，而不是按逐灯 direct-light helper 来描述
- 当前不把 `FILTER`、`World` 当作稳定保证范围

因此：

- 不要把 `glsl_ambient_lighting()` 说成“完整间接光照总和”
- 不要把 reflection、combined、probe radiance、screen-space indirect 等其他概念混进这个 helper 的定义里

### 规则 4.4：写自定义 PBR / 半 PBR 时，优先组合几何 helper、环境光 helper 和逐灯 helper

当前推荐的组合方式是：

- 几何：`glsl_normal()`、`glsl_true_normal()`、`glsl_incoming()`
- 环境：`glsl_ambient_lighting()`
- 逐灯：`glsl_light_count()`、`glsl_light_get(i)`、`glsl_light_shadow(i, N)`

典型写法应按下面这个层次组织：

- 漫反射部分：`diffuse_brdf * light.diffuse_color * light.attenuation * NdotL * shadow`
- 高光部分：`specular_brdf * light.specular_color * light.attenuation * NdotL * shadow`
- 环境部分：`glsl_ambient_lighting() * base_color * ambient_term`

这里的关键点是：

- `GLSLLight.diffuse_color` / `GLSLLight.specular_color` 已经是对自定义模型友好的逐灯颜色项
- `GLSLLight.attenuation` 只负责基础逐灯衰减，不替你计算 `NdotL`、GGX、toon ramp 或阴影
- 因此 `shadow` 仍然应显式写成 `glsl_light_shadow(i, N)`
- 如果想写更接近标准 PBR 的版本，优先在函数体内部自己实现 Fresnel / NDF / Geometry，而不要再寻找旧的 `glsl_light_diffuse_attenuation(...)` 或 `glsl_light_specular_attenuation(...)`

仓库内的 [`blender-npr-features-and-usage.md`](../blender-npr-features-and-usage.md) 已附带一段可直接粘贴到 `GLSL Function` 的 PBR 风格完整示例，可作为转换结果的参考模板。


### 规则 5：如果原代码是屏幕着色器，要把它改写成“可被材质节点调用的函数”

最常见的错误是直接把 Shadertoy 或后处理 shader 原封不动贴进来。

不可以这样做。

必须把它改成类似这种形式：

```glsl
vec4 my_effect(vec2 frag_coord, vec2 resolution, float time)
{
  ...
}
```

或者：

```glsl
vec3 my_effect(vec2 uv, float time)
{
  ...
}
```

总之要变成“普通函数”，而不是“shader 入口”。

### 规则 6：把宏分支、危险捷径和死代码收敛成稳定版本

真实转换时，很多源码表面上是“GLSL”，但里面混着大量只适用于原运行时的宏和写法。

必须额外注意：

- 如果源码里有 `#define FEATURE`、`#ifdef XXX`、`#ifndef XXX` 这类功能开关，不要把所有分支原样保留给节点；优先收敛成“当前实际启用的那条分支”
- 只有当某个开关确实需要在节点运行时调节时，才把它改成受支持的函数参数，例如 `float enabled`
- 删除未使用的辅助函数、注释掉的旧代码、媒体链接、说明文字、被宏禁用的分支
- 对依赖实现细节的捷径写法要显式改写，例如 `smoothstep(a, b, t)` 在 `a > b` 时不要直接赌实现行为，应该改写成你自己可控的稳定辅助函数
- 如果源码里有 `#define time iTime`、`#define T u_time` 这类“给运行时变量改别名”的宏，不要保留；应该展开成真实参数名
- 如果源码里有共享可变的全局状态，例如 `float gTime;` 先在某处写入、再在多个 helper 中隐式读取，优先改成显式参数传递或局部变量
- 如果源码里把比较结果直接塞进数值向量，例如 `vec3(dir.x == 0.0, dir.y == 0.0, dir.z == 0.0)`，不要赌隐式转换；改成显式三元表达式
- 如果源码里用了 `mat3x2` / `mat2x3` / `mat4x3` 这类非方阵，或者同时出现 `vec * mat` 与 `mat * vec` 的双向乘法，优先拆成语义明确的辅助函数，不要把方向感很差的矩阵表达式原样照搬

### 规则 7：区分“普通贴图采样”和“运行时反馈 / 多通道缓冲”

Shadertoy 的 `iChannel0~3` 并不总是“普通 2D 贴图”。

有些来源代码里：

- `iChannel0` 是噪声图、LUT、普通图片
- `iChannel1` 是上一帧结果
- `iChannel2` / `iChannel3` 是 `Buffer A/B/C/D` 的中间结果
- 某个通道可能还是视频、键盘纹理、cubemap、数据贴图

对当前 `GLSL Function` 节点来说，最稳妥的规则是：

- 如果某个通道本质上就是“普通静态 2D 图片采样，直接在节点面板选图”，改成 `sampler2D`
- 如果某个通道希望在节点图里接图片或程序化纹理，且核心采样能收敛为 `texture(tex, uv)`，公开函数边界仍然改成 `sampler2D`，再在节点侧用 `Image to Closure` 或 `Closure Output` 提供来源
- 如果某个通道在 `NPR Tree` 中实际来自 `NPR Input`、`AOV Input`、`NPR Refraction`、`NPR SSS Input` 或其他 NPR 运行时图像句柄，仍然可以改成 `sampler2D` 闭包输入；但闭包内部应使用 `Image Sample.Image + Offset`，GLSL 里的 `texture(image, coord)` 第二个参数应按 offset 写法和命名
- 如果某个通道依赖“上一帧反馈”“多 pass 缓冲”“运行时积累”“专用输入设备纹理”，不要假装它和普通 `sampler2D` 完全等价
- 这类运行时依赖要么删除、要么近似、要么改成普通外部输入参数，但必须明确说明是“近似改写”，不是等价转换

### 规则 8：最终输出末尾必须附带转换结果说明

转换结果不能只给代码，还必须明确告诉使用者这次转换到底是：

- `成功`
- `部分成功`
- `失败`

并且至少说明这些内容：

- 是否成功转换成当前节点可接受的函数边界类型
- 是否存在不支持而被删除、近似、替换的部分
- 是否保留了运行时依赖，或者已经去掉
- 如果做过本地解析或编译验证，要写明验证结果
- 结果说明、状态报告、删改说明、验证说明默认使用“用户当前使用的语言”

不要让使用者自己猜：

- 这段代码是不是完整可用
- 哪些效果丢了
- 哪些地方只是近似
- 有没有不支持的类型或运行时资源

说明：

- 如果用户用中文提问，就用中文写结果报告
- 如果用户用英文提问，就用英文写结果报告
- 像 `Function` 这种需要直接对应节点 UI 的字段名可以保留原样，但解释性文字和结果报告应优先跟随用户语言


## 三、AI 推荐输出格式

为了让人类或下一个工具最少返工，建议按下面格式输出：

下面模板用中文演示；如果用户使用其他语言，结果报告部分应切换到对应语言。

````text
Function: effect_name

Inputs:
- uv: vec2
- time: float
- tex: sampler2D

Outputs:
- return: vec3

Meta Checklist:
- @glsl_meta v1 已写在导出函数正上方
- 每个输入参数都有 label/description 和必要的 default/min/max/subtype
- sampler2D 只使用 label/description/panel
- out 参数只使用 label
- 参数分组 panel 已闭合

Code:
```glsl
...最终代码...
```

转换结果:
- 状态: 成功 / 部分成功 / 失败
- 最终调用函数: ...
- 已支持边界类型: ...
- 不支持或已删除: ...
- 已近似或已替代: ...
- Alpha 处理: 已保留 / 已拆分 / 原始 alpha 恒定 / 已省略
- Meta 情况: 已补全 / 无可标注接口 / 未补全并说明原因
- 验证情况: 已解析 / 已渲染 / 未验证
````

如果函数有额外 `out` 参数，也写清楚：

```text
Outputs:
- return: float
- out color: vec3
```

如果函数有可调参数、贴图输入或 `out` 参数，`Code` 中应直接包含对应的 `@glsl_meta v1` 块，而不是只在文字说明里建议用户自己补。

### 转换结果说明建议

`转换结果` 建议至少包含下面几项：

- `状态`
  - `成功`：核心效果已转成当前节点可直接使用的版本，没有已知必然报错的边界问题
  - `部分成功`：完成了可用近似，但删掉或替换了部分原效果
  - `失败`：由于边界类型、运行时依赖或其他限制，无法给出当前节点可直接使用的版本
- `已支持边界类型`
  - 明确写这次导出函数最终使用了哪些受支持边界类型，例如 `vec2 / float / sampler2D / out float`
- `最终调用函数`
  - 明确写最终在节点 `Function` 一栏中应该填写哪个函数名
  - 如果源码中保留了多个辅助函数，也要再次确认真正要选的是哪一个导出函数
- `不支持或已删除`
  - 明确列出被删除或当前节点不支持的东西，例如 `iChannel1 backbuffer feedback`、`UDIM`、`inout`
- `已近似或已替代`
  - 明确列出做过近似的部分，例如“将上一帧反馈删除”“把宏分支固定为当前启用版本”
- `Alpha 处理`
  - 如果原 shader 的 `fragColor.a` 有意义，要明确写出它是保留为 `vec4.a`、拆成单独 `out float alpha`，还是被省略
  - 如果原始 alpha 本来恒定为 `1.0`，也建议直接写明，避免用户误以为漏转
- `Meta 情况`
  - 明确写出 `@glsl_meta v1` 是否已补全
  - 如果没有写 Meta，必须说明导出函数没有输入参数和 `out` 参数，或说明当前限制导致无法标注
  - 不要把“用户可自行补 Meta”当作完成状态
- `验证情况`
  - 如果只做了静态转换，写 `未验证`
  - 如果通过节点解析，写 `已解析`
  - 如果通过本地渲染或编译，写 `已渲染`
- 如果用户不是中文语境，上面这些字段名和状态值应切换成对应语言，而不是强行保留中文


## 四、从不同来源转换时的具体策略

## 1. 从普通 GLSL 转换

如果来源本来就是 GLSL：

1. 删除 `main` / `mainImage` / pipeline 外壳
2. 保留必要的辅助函数
3. 选一个主函数作为节点导出函数
4. 把所有外部 `uniform` 改成函数参数
5. 把不支持的边界类型改成支持类型
6. 明确给出 `Function` 名称

### 适合保留的内容

- `const`
- 数学辅助函数
- `palette`
- `noise`
- `hash`
- `sdSphere` 这类 SDF 函数

### 应该移除或改写的内容

- framebuffer 输出
- `fragColor`
- `gl_FragCoord` 直接依赖
- `precision highp float;` 这类文件级精度声明
- `#version`
- `layout(...)`
- 自定义 uniform 块
- `/** SHADERDATA ... */`、`//proc:*` 这类元数据
- 注释掉的旧代码、视频链接、音乐链接等与算法无关的内容
- 仅用于切换功能的宏分支和未启用分支
- 依赖上一帧结果或其他 pass 缓冲的运行时逻辑


## 2. 从 Shadertoy GLSL 转换

Shadertoy 常见入口：

```glsl
void mainImage(out vec4 fragColor, in vec2 fragCoord)
```

转换原则：

- `fragColor` 改成返回值
- `fragCoord` 保留为参数
- `iResolution` 改成 `vec2 resolution`
- `iTime` 改成 `float time`
- `iTimeDelta` 改成 `float time_delta`
- `iFrame` 改成 `float frame`，需要整数语义时在函数内部 `int(frame)`
- `iMouse` 改成 `vec4 mouse` 或拆成更小参数
- 纹理通道 `iChannel0~3` 改成 `sampler2D`
- 如果保留 `mouse`，最好明确约定语义，例如 `mouse.xy` 为像素坐标，`mouse.z > 0.0` 表示按下
- 如果原效果依赖模糊采样或景深采样，可以把 `textureLod(iChannel0, uv, lod)` 改成 `textureLod(tex, uv, lod)`
- 如果原效果依赖 `Buffer A/B/C/D`、上一帧反馈、视频流、键盘纹理等 Shadertoy 运行时资源，要明确写明哪些部分被删除或近似

### 标准改写模板

```glsl
vec4 effect_name(vec2 frag_coord, vec2 resolution, float time)
{
  vec2 uv = frag_coord / resolution;
  ...
  return color;
}
```

### Shadertoy 来源的常见错误

- 直接保留 `mainImage`
- 保留 `iTime`
- 保留 `iResolution`
- 保留 `fragColor =`
- 保留 `iChannel0` 但没有改成参数
- 保留一堆 `#define` / `#ifdef` 分支，但不说明最终到底走哪条逻辑
- 直接照搬类似 `#define S(a, b, t) smoothstep(a, b, t)` 这种依赖反向边界的捷径写法
- 把上一帧反馈通道、Buffer 通道、视频通道直接当成普通静态 `sampler2D`


## 3. 从 HLSL 转换

HLSL 转 GLSL 时，优先做“语义剥离”和“内建函数映射”。

### 必须去掉的 HLSL / DX 语义

- `: SV_Target`
- `: SV_POSITION`
- `: TEXCOORD0`
- `float4 frag(v2f i) : SV_Target`

### HLSL 到 GLSL 常见映射

| HLSL | GLSL |
|---|---|
| `float` | `float` |
| `float2` | `vec2` |
| `float3` | `vec3` |
| `float4` | `vec4` |
| `half` / `fixed` | `float` |
| `half2` / `fixed2` | `vec2` |
| `half3` / `fixed3` | `vec3` |
| `half4` / `fixed4` | `vec4` |
| `lerp(a,b,t)` | `mix(a,b,t)` |
| `frac(x)` | `fract(x)` |
| `ddx(x)` | `dFdx(x)` |
| `ddy(x)` | `dFdy(x)` |
| `rsqrt(x)` | `inversesqrt(x)` |
| `mul(a,b)` | 需要按矩阵乘法方向手动改写 |
| `saturate(x)` | `clamp(x, 0.0, 1.0)` |
| `tex2D(tex, uv)` | `texture(tex, uv)` |
| `atan2(y, x)` | `atan(y, x)` |

### HLSL 转换中的强制规则

- `half`、`fixed` 全部收敛成 `float`
- `Texture2D` / `SamplerState` 尽量合并为单个 `sampler2D`
- 输入结构体拆成显式函数参数
- 不要保留 `cbuffer`
- 不要保留语义标注


## 4. 从 ShaderLab / Unity 片段转换

ShaderLab 只能“部分转换”，不能整段照搬。

AI 必须只抽取真正有用的着色逻辑：

- `CGPROGRAM` / `HLSLPROGRAM` 中的辅助函数
- `frag` 或核心颜色计算逻辑

AI 必须删除：

- `Shader`
- `SubShader`
- `Pass`
- `Tags`
- `Blend`
- `ZTest`
- `Cull`
- `Stencil`
- `Properties`
- `#pragma`
- `Fallback`

### ShaderLab 推荐抽取顺序

1. 找到最终颜色计算逻辑
2. 找到依赖的辅助函数
3. 找到依赖的贴图、时间、UV、屏幕参数
4. 把这些外部依赖改成函数参数
5. 写成纯 GLSL 导出函数

### Unity 常见内建量改写

| Unity / ShaderLab | Blender GLSL Function 参数建议 |
|---|---|
| `_Time.y` | `float time` |
| `i.uv` | `vec2 uv` |
| `_MainTex` | `sampler2D tex` |
| `_MainTex_ST` | 拆成 `vec2 uv_scale, vec2 uv_offset` 或直接预烘焙进 uv 计算 |
| `_ScreenParams.xy` | `vec2 resolution` |


## 五、边界类型不支持时怎么改

这是 AI 最容易翻车的地方。

### 1. `int` / `bool` 可以直接作为节点接口

当前公开边界已经支持 `int` 和 `bool` 输入、`out` 参数与返回值，不要再为了节点接口把它们改写成 `float`。例如：

```glsl
/* @glsl_meta v1
mode: label="Mode" description="Integer mode selector"
enabled: label="Enabled" description="Enables the selected mode"
out_mode: label="Selected Mode"
*/
bool test(int mode, bool enabled, out int out_mode)
{
  out_mode = mode;
  return enabled && mode > 0;
}
```

模式型整数可以通过 `items` 元数据生成下拉选项；连续整数和布尔值也可以保留原生 socket 语义。

### 2. 方阵矩阵可以作为节点接口，非方阵仍要改写

当前 `GLSL Function` 的公开边界已经支持 `mat2`、`mat3`、`mat4`，可以直接作为输入参数、`out` 参数或返回值使用。节点 UI 会按 GLSL 列主序把它们拆成多组插口：

- `mat2` -> `C1`、`C2` 两个 `vec2`
- `mat3` -> `C1`、`C2`、`C3` 三个 `vec3`
- `mat4` -> `C1`、`C2`、`C3`、`C4` 四列，每列拆成 `vec3 + W float`

因此这类方阵接口不需要再为了节点边界强行拆成多个独立函数参数：

```glsl
/* @glsl_meta v1
transform: label="Transform" description="Column-major transform matrix"
out_basis: label="Basis"
*/
mat4 matrix_boundary_example(mat4 transform, out mat3 out_basis)
{
  out_basis = mat3(transform);
  return transform;
}
```

但仍然不要把下列类型作为公开边界：

- `mat2x3`、`mat3x2`、`mat4x3` 等非方阵矩阵
- `struct` / `array`
- 方向语义很难读清的混合矩阵边界

这类情况应改成：

- 展开为语义明确的 `vec2` / `vec3` / `vec4` 参数
- 或把矩阵保留在函数内部/辅助函数内部构造
- 或用 `dot(...)` helper 明确表达投影方向

### 3. 多返回值不要用结构体

错误：

```glsl
ResultData effect(...)
```

应该改成：

```glsl
float effect(vec2 uv, out vec3 color)
{
  ...
}
```

### 4. `void` 返回值不是“没有输出”

当前节点允许导出函数返回 `void`，但前提是它仍然要通过 `out` 参数暴露至少一个输出。

错误：

```glsl
void do_nothing(vec2 uv)
{
}
```

应该改成：

```glsl
void do_something(vec2 uv, out vec3 color)
{
  color = vec3(uv, 0.0);
}
```

### 5. 纹理采样不要保留 Unity / DX 风格对象系统

错误：

```hlsl
Texture2D tex;
SamplerState samp;
float4 frag(...) : SV_Target
{
  return tex.Sample(samp, uv);
}
```

应该改成：

```glsl
vec4 frag_color(sampler2D tex, vec2 uv)
{
  return texture(tex, uv);
}
```

### 6. 反向 `smoothstep` 和类似捷径不要直接照搬

很多现成 shader 会写这种宏：

```glsl
#define S(a, b, t) smoothstep(a, b, t)
```

然后在实际调用里大量出现：

```glsl
S(0.4, 0.0, d)
S(1.0, y, st.y)
```

这种写法在原作者目标环境里“可能刚好能跑”，但它依赖的是反向边界的实现细节，不适合作为稳定转换结果直接保留。

更稳妥的做法是：

- 显式写一个你自己控制的辅助函数
- 在函数里自己处理正向、反向、退化区间
- 再把所有原来的 `S(...)` 调用替换掉

### 7. 不要依赖布尔值到数值类型的隐式转换

错误：

```glsl
dir += vec3(dir.x == 0.0, dir.y == 0.0, dir.z == 0.0) * 1e-5;
```

应该改成：

```glsl
dir += vec3(dir.x == 0.0 ? 1e-5 : 0.0,
            dir.y == 0.0 ? 1e-5 : 0.0,
            dir.z == 0.0 ? 1e-5 : 0.0);
```

说明：

- 原 shader 有时会依赖目标环境对 `bool` / `bvec*` 的宽松处理
- 稳定转换时不要假设“比较结果可以直接当 `float` 用”
- 这类写法优先改成显式三元表达式、`mix`、`step` 或手动 `float(...)` 化

### 8. 非方阵或方向不清晰的矩阵运算优先展开成辅助函数

有些来源代码会写出这类表达式：

```glsl
mat3x2 tri = mat3x2(...);
vec3 coord = some_vec2 * tri;
vec2 uv = tri * some_vec3;
```

即使原环境能跑，这类写法在转换后也常常不够直观，不利于稳定维护和排错。

更推荐的做法是：

- 把“投影”与“反投影”拆成两个显式 helper
- 用 `dot(...)` 或明确分量组合来表达变换
- 不要把“向量在左乘”还是“矩阵在左乘”的理解成本留给用户

### 9. 不要保留共享可变全局状态

错误：

```glsl
float gTime = 0.0;

float wave(vec3 p)
{
  return sin(gTime + p.x);
}

vec3 effect(vec2 uv, float time)
{
  gTime = time;
  return vec3(wave(vec3(uv, 0.0)));
}
```

应该改成：

```glsl
float wave(vec3 p, float time)
{
  return sin(time + p.x);
}

vec3 effect(vec2 uv, float time)
{
  return vec3(wave(vec3(uv, 0.0), time));
}
```

说明：

- 像 `gTime`、`gFrame`、`gMouse` 这种“先写全局，再在 helper 里读”的模式，不适合当作稳定转换模板保留
- 优先改成显式参数传递，避免隐式副作用和阅读歧义

### 10. 原 shader 的 alpha 要显式处理

如果原始代码里 `fragColor.a` 不是固定 `1.0`，不要在转换报告里省略这件事。

可选做法：

- 直接保留为 `vec4` 返回值
- 或拆成 `out float alpha`
- 如果当前转换故意只保留 RGB，也必须在结果报告里明确写出 alpha 被省略

如果原 shader 的 alpha 本来恒定为 `1.0`，也建议在结果报告里写明“原始 alpha 恒定，因此未单独处理”。


## 六、AI 必须做的自检清单

在输出最终代码前，必须逐条检查：

1. 最终是不是“普通函数源码”，而不是完整 shader 文件？
2. 是否明确给出了 `Function` 名称？
3. 是否还残留了 `main` / `mainImage` / `fragColor`？
4. 是否还残留了 `uniform` / `cbuffer` / `Properties` / `Pass`？
5. 所有外部输入是否都已改成函数参数？
6. 导出函数的参数和返回值是否只用了允许的边界类型？
7. 是否错误保留了 `inout`？
8. 是否把 `sampler2D` 写成了 `out`？
9. 如果用了普通贴图采样，是否已经改成 `texture(tex, uv)`；如果是 NPR 图像句柄采样，是否已经改成 `texture(image, offset)` 语义；如果确实需要显式级别，是否说明了 `textureLod` 等限制？
10. 如果导出函数返回 `void`，是否仍然通过 `out` 参数暴露了至少一个输出？
11. 如果有 `sampler2D` 参数，是否提醒了使用者在节点参数区为每个 `sampler2D` 选择图片？
12. 如果有 `sampler2D` 参数，是否明确说明应通过 `Image to Closure` 或符合约定的 `Closure Output` 提供来源？
13. 如果有 `sampler2D` 参数，是否错误假设它支持任意 Closure、任意图像专用采样函数或旧的面板选图工作流？
14. 如果原始来源使用了历史 `sample2D` 旧写法，是否已经统一改写成公开的 `sampler2D` 边界类型？
15. 如果用户明确说是 `NPR Tree`，且来源是 `NPR Input` / `AOV Input` / `NPR Refraction` / `NPR SSS Input` 这类图像句柄，是否保留 `sampler2D` 闭包输入，同时说明闭包内部要用 `Image Sample.Image + Offset`？
16. 是否明确说明 `texture(image, offset)` 里的 `offset` 不是 UV，`vec2(0.0)` 采当前像素，`Pixel` 模式下 `vec2(1.0, 0.0)` 表示右移 1 个像素？
17. 如果源码里有宏开关、注释掉的旧代码、未使用辅助函数，是否已经收敛或删除？
18. 如果源码里有反向 `smoothstep` 或类似依赖实现细节的捷径写法，是否已经改成稳定辅助函数？
19. 如果源码依赖上一帧反馈、Buffer A/B/C/D、多 pass 中间结果、视频或键盘通道，是否已经明确说明删除、近似或替代方案？
20. 如果来自 HLSL，是否已去掉所有语义标注？
21. 如果来自 ShaderLab，是否只保留了核心逻辑？
22. 是否避免依赖特定引擎的 include 和无法离开原运行时的宏？
23. 最终输出里是否明确写了本次转换是 `成功`、`部分成功` 还是 `失败`？
24. 最终输出里是否再次明确写了节点 `Function` 一栏最终应调用的函数名？
25. 最终输出里是否明确列出了不支持、删除、近似、替代和验证情况？
26. 最终输出里的结果报告语言，是否跟随了用户当前使用的语言？
27. 是否移除了 `precision` / `#version` / `layout(...)` / `SHADERDATA` / `proc:` 这类文件级声明或工具元数据？
28. 如果源码通过 `#define time iTime` 之类的别名宏引用运行时变量，是否已经展开并参数化？
29. 如果源码里用了共享可变全局状态（如 `gTime`），是否已经改成显式参数或局部变量？
30. 如果源码里有把比较结果直接拿去构造 `vec*` 或参与数值运算的写法，是否已经改成显式数值表达式？
31. 如果源码里用了 `mat3x2` / `mat2x3` 或其他方向不够直观的矩阵乘法，是否已经改写成更清晰的 helper？
32. 如果原 shader 的 alpha 有实际含义，是否已经明确说明保留、拆分还是省略？


## 七、推荐的最小输出模板

如果你是另一个 AI，除非用户明确要求别的格式，否则建议按下面模板输出：

````text
Function: converted_function

Inputs:
- uv: vec2
- time: float
- tex: sampler2D

Outputs:
- return: vec3

Code:
```glsl
vec3 converted_function(vec2 uv, float time, sampler2D tex)
{
  vec3 col = texture(tex, uv).rgb;
  return col;
}
```

转换结果:
- 状态: 成功
- 最终调用函数: converted_function
- 已支持边界类型: vec2, float, sampler2D, vec3 return
- 不支持或已删除: 无
- 已近似或已替代: 无
- Alpha 处理: 原始 alpha 恒定
- 验证情况: 未验证
````

如果是部分成功，也建议像这样写：

````text
转换结果:
- 状态: 部分成功
- 最终调用函数: converted_function
- 已支持边界类型: vec2, vec2, float, sampler2D, vec3 return
- 不支持或已删除: 删除 iChannel1 的 backbuffer feedback
- 已近似或已替代: 将多 pass 运行时依赖替换为单 pass 近似
- Alpha 处理: 已省略
- 验证情况: 已解析
````


## 八、示例

## 示例 1：Shadertoy `mainImage` 转节点函数

### 原始形式

```glsl
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
  vec2 uv = fragCoord / iResolution.xy;
  vec3 col = 0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0.0, 2.0, 4.0));
  fragColor = vec4(col, 1.0);
}
```

### 转换后

```text
Function: shadertoy_color
```

```glsl
vec4 shadertoy_color(vec2 frag_coord, vec2 resolution, float time)
{
  vec2 uv = frag_coord / resolution;
  vec3 col = 0.5 + 0.5 * cos(time + uv.xyx + vec3(0.0, 2.0, 4.0));
  return vec4(col, 1.0);
}
```


## 示例 2：HLSL 片段函数转节点函数

### 原始形式

```hlsl
float4 frag(v2f i) : SV_Target
{
    float2 uv = i.uv;
    float wave = sin(_Time.y + uv.x * 10.0);
    float3 col = lerp(float3(0.2, 0.4, 0.8), float3(1.0, 0.9, 0.2), saturate(wave));
    return float4(col, 1.0);
}
```

### 转换后

```text
Function: hlsl_wave_color
```

```glsl
vec4 hlsl_wave_color(vec2 uv, float time)
{
  float wave = sin(time + uv.x * 10.0);
  vec3 col = mix(vec3(0.2, 0.4, 0.8), vec3(1.0, 0.9, 0.2), clamp(wave, 0.0, 1.0));
  return vec4(col, 1.0);
}
```


## 示例 3：Unity 贴图采样逻辑转节点函数

### 原始形式

```hlsl
sampler2D _MainTex;

fixed4 frag(v2f i) : SV_Target
{
    fixed4 c = tex2D(_MainTex, i.uv);
    return c;
}
```

### 转换后

```text
Function: sample_main_tex
```

```glsl
vec4 sample_main_tex(sampler2D tex, vec2 uv)
{
  return texture(tex, uv);
}
```


## 示例 4：需要多个输出时的写法

```text
Function: split_mask_color
```

```glsl
float split_mask_color(vec2 uv, out vec3 color)
{
  float mask = smoothstep(0.4, 0.6, uv.x);
  color = vec3(uv, 1.0 - uv.x);
  return mask;
}
```


## 示例 5：反向 `smoothstep` 和 `textureLod` 的改写

### 原始片段

```glsl
#define S(a, b, t) smoothstep(a, b, t)

float mask = S(0.4, 0.0, d);
vec3 col = textureLod(iChannel0, uv + n, focus).rgb;
```

### 转换思路

- `S(0.4, 0.0, d)` 这种反向边界不要直接照搬
- `textureLod` 如果原算法确实依赖显式 `LOD`，可以保留
- 纹理通道仍然统一收敛成 `sampler2D tex`

### 转换后

```text
Function: sample_blurred_mask
```

```glsl
float stable_smooth(float a, float b, float t)
{
  float denom = b - a;
  if (abs(denom) < 1e-5) {
    return t < a ? 0.0 : 1.0;
  }
  float x = clamp((t - a) / denom, 0.0, 1.0);
  return x * x * (3.0 - 2.0 * x);
}

vec3 sample_blurred_mask(vec2 uv, float d, float focus, sampler2D tex)
{
  float mask = stable_smooth(0.4, 0.0, d);
  vec3 col = textureLod(tex, uv, focus).rgb;
  return col * mask;
}
```


## 示例 6：Shadertoy 上一帧反馈通道的处理

### 原始片段

```glsl
vec4 past = texture(iChannel1, q);
if (condition) {
  col = mix(col, past.rgb, 0.85);
}
```

### 转换原则

- 如果 `iChannel1` 本质上是上一帧结果或 `Buffer A/B/C/D` 输出，不要声称它能被当前节点“等价支持”
- 可以：
  - 删除这段反馈逻辑
  - 改成普通静态贴图近似
  - 或把它改成另一个普通 `sampler2D` 输入，但必须明确说明这是“近似替代”

### 推荐说明方式

```text
原 shader 中的 iChannel1 用作上一帧反馈。
当前 GLSL Function 节点没有 Shadertoy backbuffer / multi-pass 运行时，
因此这里删除该反馈混合，只保留核心单帧效果。
```


## 示例 7：`mat3x2` 三角网格投影写法的稳定改写

### 原始片段

```glsl
mat3x2 tri = mat3x2(-1,0, 0.5,0.866, 0.5,-0.866);
vec3 coord = (res * 0.5 - fragCoord) * tri / res.y * SCALE + SCALE;
if (texture(u_tex, .71 - (tri * vox) / 8e2).r < .6) break;
```

### 转换思路

- 这类写法同时混用了 `vec2 * mat3x2` 和 `mat3x2 * vec3`
- 即使原环境允许，也不适合作为稳定转换模板直接保留
- 更稳妥的方式是把“投影”和“反投影”拆成两个语义明确的 helper

### 转换后

```text
Function: triangle_dda
```

```glsl
vec3 triangle_project(vec2 p)
{
  return vec3(dot(p, vec2(-1.0, 0.0)),
              dot(p, vec2(0.5, 0.866)),
              dot(p, vec2(0.5, -0.866)));
}

vec2 triangle_unproject(vec3 v)
{
  return vec2(-v.x + 0.5 * v.y + 0.5 * v.z,
              0.866 * (v.y - v.z));
}
```

说明：

- 这种改法会比把非方阵乘法原样照搬更清楚
- 同时也更适合在转换报告里解释“哪些地方做了等价重写”


## 九、推荐给 AI 的转换提示词

如果你想让另一个 AI 帮你转换 shader，推荐直接给它下面这段要求：

```text
把下面这段 shader 代码转换为 Blender NPR Port 的 GLSL Function 节点可直接使用的 GLSL。

必须遵守这些规则：
1. 最终结果只能是普通 GLSL 函数源码，不要输出完整 shader 文件。
2. 明确给出 Function 应设置的函数名。
3. 只要导出函数有输入参数或 out 参数，代码里必须在导出函数正上方写完整的 `@glsl_meta v1`，不要让用户后续自己补。
4. Meta 要覆盖每个输入参数和 out 参数：输入参数写 label/description 和必要的 default/min/max/subtype；sampler2D 只写 label/description/panel；out 参数只写 label；返回值不支持 Meta，必须在 Outputs 中说明。
5. 参数超过 4 个或语义能自然分组时，使用一级 `@panel` / `@end_panel` 分组，panel 必须闭合。
6. 把所有外部 uniform / 时间 / 分辨率 / 鼠标 / 贴图输入改成函数参数。
7. 导出函数的参数和返回值只允许使用 float、int、bool、vec2、vec3、vec4、mat2、mat3、mat4、sampler2D，以及 out float/int/bool/vec2/vec3/vec4/mat2/mat3/mat4。
8. 不允许使用 inout，也不允许 out sampler2D。
9. 如果来源是 HLSL 或 ShaderLab，去掉语义、Pass、Properties、pragma 和引擎包装层。
10. 贴图采样优先改成 `texture(tex, uv)`；如果原算法明确依赖显式 `LOD`，可以保留为 `textureLod(tex, uv, lod)`，并说明这更适合配合 `Image to Closure`。
11. 如果存在宏开关、死代码、未使用辅助函数、反向 `smoothstep`、运行时别名宏、共享可变全局状态、布尔到数值隐式转换、非方阵矩阵双向乘法这类不稳定写法，要收敛成稳定版本。
12. 如果需要多个输出，用 out 参数，不要用 struct 返回。
13. 如果原 shader 的 alpha 有意义，要明确说明是保留、拆分还是省略。
14. 在结果最后明确说明转换是 success / partial / failed，并列出不支持、删除、近似、替代、alpha 处理、Meta 情况、验证情况。
15. 输出格式为：
   Function: ...
   Inputs: ...
   Outputs: ...
   Meta Checklist: ...
   Code: ```glsl ... ```
   Conversion Result: ...
   其中要再次明确 function_to_call: ...

待转换代码如下：
```


## 十、结论

对当前 `GLSL Function` 节点来说，最稳定的思路不是“把原 shader 原封不动搬过来”，而是：

1. 抽取核心算法
2. 去掉引擎壳
3. 把外部依赖参数化
4. 把接口压缩到节点支持的那几种边界类型
5. 输出一个明确可选的导出函数，并给公开接口补全 Meta

只要严格遵守这份文档，绝大多数数学类 GLSL、很多 HLSL 片段逻辑、以及相当一部分 ShaderLab 片段逻辑，都可以被稳定改写为当前 Blender 节点可直接使用的版本。
## 附录：GLSL Function Meta 语法

`GLSL Function` 节点支持从 GLSL 源码里的块注释读取少量 Meta 信息，用来描述输入参数在 Blender 节点界面中的默认值、范围、子类型和注释。

这个 Meta 系统只负责节点 UI 语义，不改变 GLSL 函数逻辑本身。

当前范围主要是：

- 默认值
- 最小值
- 最大值
- `int` 选择列表
- 隐藏数值输入控件
- socket subtype
- socket 显示名
- socket 注释 / tooltip
- 一级折叠面板分组

AI 生成最终 GLSL Function 代码时，Meta 的默认要求是“补全”，不是“可选”。除非导出函数没有任何输入参数和 `out` 参数，否则最终代码块里必须直接包含 `@glsl_meta v1`。

### 1. 基本格式

Meta 必须写在函数正上方的块注释里，并以 `@glsl_meta` 开头：

```glsl
/* @glsl_meta v1
base_color: label="基础色" default=vec3(1.0) subtype=color description="Base color before stylization"
strength: label="强度" default=0.5 min=0.0 max=1.0 subtype=factor description="Blend amount"
tint: label="目标颜色" default=vec3(1.0, 0.8, 0.2) subtype=color description="Target tint color"
*/
vec3 stylize(vec3 base_color, float strength, vec3 tint)
{
  return mix(base_color, tint, strength);
}
```

### 2. 作用规则

Meta 只会作用到它正下方那个函数。

也就是说：

- 每个函数最多对应一块 Meta
- Meta 块应该紧贴函数上方
- Meta 块内部只写“参数名: 属性”
- 不再需要也不再推荐写 `function some_name`
- 不再需要也不再推荐写 `some_function.some_param`

如果 `@glsl_meta` 后面不是紧接着一个函数定义，而是夹了别的顶层代码，当前实现会报错。

#### 2.0 AI 补全清单

当你生成或转换一个导出函数时，先按函数签名逐项检查 Meta：

- `float` 调节项：写 `label`、`default`、`description`；能确定范围时写 `min/max`
- `float` 的强度、混合、遮罩、阈值、柔和度、概率类参数：优先写 `subtype=factor`
- `int` / `bool` 开关：写清楚 `label`、`default` 和 `description`；如果 `int` 是固定模式枚举，优先写 `items`
- `int items` 下拉：默认不写 `show_label`，让控件只显示当前选项；当下拉项离开上下文会难以理解时写 `show_label=true`
- `vec2` 坐标、偏移、比例：写 `label`、`default=vec2(...)` 和 `description`
- `vec3` 颜色：写 `subtype=color`，并给出颜色默认值；需要 alpha 时用 `vec4`，它会拆成 `vec3 + float W`
- `vec3` 方向、法线、位置：不要误标为颜色；用 `description` 写清楚空间语义；不要使用 `subtype=direction`，需要普通三轴输入时用 `subtype=xyz` 或省略 subtype
- `sampler2D`：只写 `label` 和 `description`，必要时放进 `@panel`
- `out` 参数：只写 `label`；不要写 `default/min/max/subtype/description`
- 返回值：不写 Meta；在 `Outputs` 中说明返回类型和含义
- 参数较多时：用一级 `@panel "分组名" closed=true/false` 分组，并用 `@end_panel` 闭合

如果某个参数是内部常量更合适，就不要暴露成函数参数；一旦暴露成函数参数，就要给它写对应 Meta。

布局建议：

- 主要输入（颜色、UV、主强度、模式）放在默认区域或 `closed=false` 面板里
- 高级调节、调试输出、很少改的阈值放进 `closed=true` 面板
- 固定模式选择用 `int items`，不要用多个 bool 互斥开关
- 需要连线驱动的模式选择仍然用普通函数 `int` 参数；需要编译期裁剪或让辅助函数看到的模式选择用 `@glsl_defines`
- 一个面板里只有一个下拉时通常不需要 `show_label=true`；多个相邻下拉或没有清晰分组标题时再显示 label

#### 2.1 推荐结构

```glsl
/* @glsl_meta v1
base_color: label="基础色" default=vec3(1.0) subtype=color description="Input color"
strength: label="强度" default=0.5 min=0.0 max=1.0 subtype=factor description="Blend amount"
tint: label="目标颜色" default=vec3(1.0, 0.8, 0.2) subtype=color description="Target color"
*/
vec3 stylize(vec3 base_color, float strength, vec3 tint)
{
  return mix(base_color, tint, strength);
}
```

这里的 `strength` 和 `tint` 会自动解释为正下方 `stylize` 的参数。

#### 2.2 不再推荐的旧写法

```glsl
/* @glsl_meta v1
stylize.strength: default=0.5
stylize.tint: default=vec3(1.0, 0.8, 0.2)
*/
```

这种旧写法现在不再推荐，建议改成“一函数一块 Meta，直接放在函数上方”。

### 3. 当前支持的键

#### 3.1 `default`

- `float` 使用标量
- `vec2/vec3/vec4` 使用对应构造器
- `float/vec2/vec3/vec4` 也可以直接写 GLSL 表达式默认值
- `mat2/mat3/mat4` 当前不支持 Meta 默认值；节点会生成按列拆分的输入插口
- 当 `default=` 是表达式时，socket 未连接会使用这段表达式；一旦连线，就使用连线值
- 当 `default=` 是表达式时，节点会自动隐藏这个输入的数值输入框，但保留 socket 本身
- 表达式默认值可以引用同一份源码里的 top-level helper 函数，也可以直接调用 `glsl_position()`、`glsl_normal()`、`glsl_ambient_lighting()` 这类内置 helper

示例：

```glsl
strength: default=0.5
uv_scale: default=vec2(1.0, 1.0)
tint: default=vec3(1.0, 0.8, 0.2)
color_a: default=vec4(1.0, 0.5, 0.2, 1.0)
```

表达式默认值示例：

```glsl
position_ws: default=glsl_position()
normal_ws: default=normalize(glsl_normal())
ambient: default=glsl_ambient_lighting()
```

更完整的推荐写法：

```glsl
vec3 default_surface_color()
{
  return glsl_position() * 0.25 + vec3(0.5);
}

/* @glsl_meta v1
surface_color: default=default_surface_color()
*/
vec4 emit_surface_color(vec3 surface_color)
{
  return vec4(surface_color, 1.0);
}
```

如果表达式比较复杂，建议最外层包一层括号，这样在 Meta 行里更稳妥：

```glsl
mask: default=(smoothstep(0.2, 0.8, glsl_position().z))
```

#### 3.2 `min`

用于设置输入 socket 的最小值。

```glsl
strength: min=0.0
```

#### 3.3 `max`

用于设置输入 socket 的最大值。

```glsl
strength: max=1.0
```

#### 3.4 `items`

用于给 `int` 输入参数声明固定选择列表。它只改变未连接时默认值的 UI 表现：socket 类型仍然是 `int`，仍然可以连线，函数调用也仍然传入整数。

带 `items` 的下拉菜单默认只显示当前选项，不显示前置参数名；需要保留前置名称时写 `show_label=true`。

```glsl
/* @glsl_meta v1
method: label="方法" default=1 items="0:Christensen-Burley;1:Random Walk;2:Random Walk Skin"
*/
vec4 subsurface_mode(vec4 color, int method)
{
  if (method == 2) {
    return color * vec4(1.0, 0.9, 0.85, 1.0);
  }
  return color;
}
```

规则：

- 只允许用于 `int` 输入参数。
- 格式固定为 `整数:显示名`，多个项用 `;` 分隔。
- `default` 必须是整数字面量，并且必须等于列表中的某个值。
- 列表值不能重复；显示名不能为空。
- `items` 不能和 `min` / `max` 混用。需要连续范围时用普通整数输入框；需要固定模式时用 `items`。
- `show_label=true|false` 只能和 `int items` 一起使用；省略时默认不显示下拉菜单前的参数名。
- 显示名只影响 UI，不参与 GLSL 参数名、socket identifier 或函数调用。

使用建议：

- 当一个 `int` 参数只是固定算法/混合/采样模式时，用 `items`，不要让用户手输整数。
- 当参数需要连续整数范围（例如采样数量、迭代次数）时，用普通 `min/max`，不要写 `items`。
- 当模式值需要从其他节点连线控制时，用函数参数 `int items`；下拉只影响未连接时的默认值，连线后仍按 int socket 工作。
- 当模式会改变编译期代码路径、需要影响辅助函数，或希望 wrapper 生成 `#define` 时，用 `@glsl_defines` 的 int `items`，不要把它伪装成函数参数。
- 默认让下拉控件隐藏前置 label，使节点更紧凑；只有多个下拉靠在一起且标题不清楚时写 `show_label=true`。

#### 3.5 `subtype`

用于设置 Blender socket subtype。

`float` 常用值：

- `none`
- `unsigned`
- `percentage`
- `factor`
- `mass`
- `angle`
- `time`
- `time_absolute`
- `distance`
- `wavelength`

`vec2/vec3/vec4` 常用值：

- `none`
- `factor`
- `percentage`
- `translation`
- `velocity`
- `acceleration`
- `euler`
- `xyz`
- `color`（`vec3` 会显示为颜色插口；`vec4` 可解析但仍会拆成 `vec3 + float W`）

示例：

```glsl
strength: default=0.5 min=0.0 max=1.0 subtype=factor
offset: default=vec3(0.0) subtype=translation
normal_dir: default=vec3(0.0, 0.0, 1.0) subtype=xyz
tint: default=vec3(1.0, 0.8, 0.2) subtype=color
overlay: default=vec4(1.0, 0.8, 0.2, 0.5)
```

`direction` 虽然是 Blender 的向量 subtype，但它会在节点 UI 中显示成球形方向控件，不适合 GLSL Function 自动生成的方向、法线、视线、切线参数；这类参数应靠 `label` / `description` 写清楚坐标空间，并使用 `subtype=xyz` 或不写 subtype。

#### 3.6 `hide_value`

用于隐藏输入 socket 的数值输入框，但保留 socket 本身。

这意味着：

- 仍然可以连线
- 仍然保留接口
- 只是节点面板里不显示具体数值输入控件

示例：

```glsl
strength: default=0.5 hide_value=true
```

当前支持的布尔写法：

- `true` / `false`
- `1` / `0`
- `yes` / `no`
- `on` / `off`

#### 3.7 `label`

用于给 socket 设置节点界面上的显示名。它只改变节点 UI 文本，不改变 GLSL 参数名，也不改变 socket identifier。

这适合把必须保持合法 GLSL 标识符的参数名显示成中文或更易读的名称：

```glsl
base_color: label="基础色" default=vec4(1.0, 1.0, 1.0, 1.0)
tex: label="贴图"
out_color: label="输出色"
```

规则：

- `label="..."` 支持空格和中文
- 如果文本里需要写引号，使用 `\"`
- 如果文本里需要写反斜杠，使用 `\\`
- V1 只支持单行显示名，不支持跨行文本
- `label` 不改变 socket identifier；例如参数 `base_color` 的输入仍然是 `In_base_color`
- 输入参数都支持 `label`
- `out` 参数只支持 `label`，不支持默认值、范围、隐藏值、subtype、description 或 panel
- `sampler2D` 支持 `label`

#### 3.8 `description`

用于给输入 socket 写描述文本。这个文本会进入 socket declaration 的 `description`，在 Blender 节点 tooltip 中显示。

推荐使用带引号的单行字符串：

```glsl
strength: default=0.5 min=0.0 max=1.0 subtype=factor description="Blend amount for the effect"
tint: default=vec3(1.0, 0.8, 0.2) subtype=color description="Main tint color"
tex: label="贴图" description="Source texture closure"
```

规则：

- `description="..."` 支持空格
- 如果文本里需要写引号，使用 `\"`
- 如果文本里需要写反斜杠，使用 `\\`
- V1 只支持单行描述，不支持跨行文本
- `description` 只影响 UI，不改变 socket identifier、默认值、范围或 GLSL 函数调用方式
- `sampler2D` 只支持 `label`、`description` 和 panel 分组，不支持 `default/min/max/hide_value/subtype`

#### 3.9 `@panel` / `@end_panel`

用于把大量输入参数分组到节点上的一级折叠面板里。面板只影响 UI 排列，不改变 socket identifier，也不改变 GLSL 函数调用方式。

```glsl
/* @glsl_meta v1
base_color: label="基础色" default=vec3(1.0) subtype=color description="Base color before shading"

@panel "Specular" closed=true
specular: label="高光强度" default=0.5 min=0.0 max=1.0 subtype=factor description="Specular contribution"
roughness: label="粗糙度" default=0.5 min=0.0 max=1.0 subtype=factor description="Microfacet roughness"
anisotropy: label="各向异性" default=0.0 min=-1.0 max=1.0 description="Anisotropic highlight direction bias"
@end_panel

@panel "Thin Film" closed=false
film_thickness: label="薄膜厚度" default=0.0 min=0.0 description="Thin-film thickness control"
film_ior: label="薄膜 IOR" default=1.5 min=1.0 description="Thin-film index of refraction"
@end_panel
*/
vec3 shader(
  vec3 base_color,
  float specular,
  float roughness,
  float anisotropy,
  float film_thickness,
  float film_ior)
{
  return base_color;
}
```

规则：

- `@panel "Name" closed=true|false` 开始一个面板
- `closed` 省略时默认为 `true`
- 面板名可以加引号；不带空格的名字可以不加引号
- `@end_panel` 必须显式关闭当前面板
- 面板内允许 `param:` 这种空属性行，表示只把参数放到当前面板，不改默认值或范围
- V1 只支持一级面板，不支持嵌套
- 重复 `@end_panel`、未关闭面板、嵌套 `@panel` 都会报 parse error

### 4. 行为规则

#### 4.1 默认值同步规则

`default` 不会在每次 redraw 时反复覆盖用户手动修改的 socket 值。

当前逻辑是：

- 当 `default` 是 literal（例如 `0.5`、`vec3(...)`）时，Meta 第一次生效会把它写入 socket 默认值
- 刷新后只要参数名和 socket identifier 没变，用户手动改过的 literal 默认值会保留
- Meta 本身发生变化时，新出现的 socket 会使用新的声明默认值；已有同名 socket 不会被强制重置
- 当 `default` 是表达式时，不会写入 RNA/socket 静态默认值
- 表达式默认值只在 wrapper 运行时参与：未连接时用表达式，已连接时用连线值

这意味着它更适合作为“函数作者建议值”，而不是强制锁死值。

#### 4.2 `min/max/subtype/label/description` 的作用

这些项属于 socket 声明的一部分，会直接影响 Blender 节点界面、socket 类型或 tooltip。

例如：

- `subtype=factor` 会让 float 输入变成 `NodeSocketFloatFactor`
- `subtype=xyz` 会让 vector 输入变成 `NodeSocketVectorXYZ`
- `vec3/vec4` 默认仍然是向量输入，不会自动变成颜色输入
- `subtype=color` 会让 `vec3` 输入变成 `NodeSocketColor`
- `vec3 + subtype=color` 进入 GLSL 函数时只使用 `rgb`
- `vec4` 输入会拆成 `vec3 + float W`，进入 GLSL 函数时会重组为完整 `rgba`
- `label="..."` 会显示为 socket 名称，不影响 GLSL 参数名、identifier 或计算
- `description="..."` 会显示在 socket tooltip 中，不影响计算

### 5. 当前限制

当前版本有这些限制：

- 除 `out` 参数的 `label` 以外，Meta 只支持输入参数
- 不支持返回值 Meta
- 不支持 `inout`
- `sampler2D` 只支持 `label`、`description` 和 panel 分组，不支持默认值、范围、隐藏值或 subtype
- `mat2/mat3/mat4` 边界参数的 Meta 只建议写 `label/description/panel`，不要写 `default/min/max/subtype`
- 不支持 `struct` / `array` 边界参数 Meta
- panel 只支持一级，不支持嵌套
- panel 必须显式 `@end_panel` 关闭
- 表达式默认值当前只支持输入参数 `float / vec2 / vec3 / vec4`
- 表达式默认值不要引用同函数的其他参数名；这类值在 wrapper 里不会自动展开成可见局部变量
- 如果 Meta 指向了不存在的参数，会报错
- 一个函数当前只支持一块 Meta

### 6. 推荐写法

```glsl
/* @glsl_meta v1
base_color: label="基础色" default=vec3(1.0) subtype=color description="Base color before dissolve"
threshold: label="阈值" default=0.35 min=0.0 max=1.0 subtype=factor description="Mask cutoff"
edge_width: label="边缘宽度" default=0.08 min=0.0 max=1.0 subtype=factor description="Soft edge width"
edge_color: label="边缘颜色" default=vec3(1.0, 0.5, 0.1) subtype=color description="Edge highlight color"
*/
vec3 dissolve_mask(vec3 base_color, float threshold, float edge_width, vec3 edge_color)
{
  return base_color;
}
```

### 7. 不推荐写法

#### 7.1 旧的全局目标写法

不要再写：

```glsl
/* @glsl_meta v1
function stylize
strength: default=0.5
*/
```

也不要再写：

```glsl
/* @glsl_meta v1
stylize.strength: default=0.5
*/
```

现在更推荐也更稳定的方式，是把 Meta 直接放在函数正上方，并且块内只写参数名。

#### 7.2 伪造 GLSL 形参默认值

不要写：

```glsl
vec3 stylize(vec3 base_color, float strength = 0.5)
```

当前节点不会把这种写法当成 Blender 参数默认值系统。

#### 7.3 给 `sampler2D` 写数值 Meta

不要写：

```glsl
/* @glsl_meta v1
tex: default=0.5
*/
vec4 sample_it(sampler2D tex, vec2 uv)
{
  return texture(tex, uv);
}
```

`sampler2D` 当前通过 `Image to Closure` 或 `Closure Output` 接入来源，不支持默认值、范围、隐藏值或 subtype。

可以写 `label`、`description`，也可以放进 panel：

```glsl
/* @glsl_meta v1
@panel Texture closed=true
tex: label="贴图" description="Texture closure used by texture(tex, uv)"
uv: label="坐标" default=vec2(0.0) description="Texture coordinates"
@end_panel
*/
vec4 sample_it(sampler2D tex, vec2 uv)
{
  return texture(tex, uv);
}
```

#### 7.4 给不存在的参数写 Meta

不要写：

```glsl
/* @glsl_meta v1
missing_param: default=1.0
*/
vec3 stylize(vec3 base_color, float strength)
{
  return base_color;
}
```

这会直接触发解析错误。

### 8. 给 AI 的转换建议

如果你是另一个 AI，要把外部 GLSL / HLSL / ShaderLab 片段转换成 `GLSL Function` 节点可用代码：

- 默认必须给导出函数补全 `@glsl_meta v1`，除非导出函数没有任何输入参数和 `out` 参数
- 必须把“建议默认值”写进 Meta，而不是只写在说明文字里
- 必须把语义明确的参数范围写进 Meta，减少手动调节点的成本
- 必须把 Blender 语义明确的参数标为合适 subtype
- 必须给 `sampler2D`、坐标、颜色、强度、阈值、输出参数写清楚可读 label
- 必须在输出前检查 Meta 是否只引用真实存在的函数参数或 `out` 参数
- 不要把运行时资源、贴图选择、函数逻辑分支控制错误地塞进 Meta

最稳妥的理解是：

- GLSL 函数体负责算法
- Meta 注释负责节点 UI 语义

## 附录：GLSL Function Defines 语法

`@glsl_defines` 用来声明编译期宏开关。它和 `@glsl_meta` 是两套独立信息：Meta 只描述函数参数和 socket UI，Defines 会生成独立的 `Defines` 面板，并在编译 wrapper 前写入 `#define`。

基本写法：

```glsl
/* @glsl_defines v1 closed=true
@define USE_RIM bool default=true label="Rim Lighting" description="Compile rim lighting branch"
@define EFFECT_MODE int default=2 min=0 max=3 label="Effect Mode" description="Compile-time effect variant"
@define METHOD int default=1 label="Method" items="0:Christensen-Burley;1:Random Walk;2:Random Walk Skin"
*/
```

规则：

- `@glsl_defines v1` 可以写成单独块，不需要紧贴某个函数。
- 头部支持 `closed=true|false`，控制 `Defines` 面板首次出现时是否默认折叠；省略时默认为 `closed=false`。
- `@define NAME bool default=true|false` 会显示为布尔开关；关闭时不会生成对应 `#define`。
- `@define NAME int default=... min=... max=...` 会显示为整数输入，并生成 `#define NAME value`。
- `@define NAME int default=... items="0:Label;1:Other"` 会显示为下拉菜单，并生成 `#define NAME value`。
- 带 `items` 的宏下拉菜单默认只显示当前选项；需要保留前置宏名时写 `show_label=true`。
- `items` 只允许用于 `int` 宏，不能用于 `bool` 宏，也不能和 `min/max` 混用。
- `items` 的值不能重复，显示名不能为空；`default` 必须是列表中的某个整数值。
- `show_label=true|false` 只能和 `int items` 一起使用。
- 宏名必须是合法 GLSL 标识符，最长 63 个字符；更长的宏名会报 parse error，避免保存到节点 DNA 时被截断。
- `label` 和 `description` 只影响面板显示，不改变宏名；`description` 会显示在对应宏控件下方。
- 同一份源码可以有多个 `@glsl_defines` 块，但宏名不能重复；如果多个块都写了 `closed`，取值必须一致。
- 宏名是整份 GLSL 源码级别的编译开关，辅助函数也能通过 `#ifdef` / `#if` 看到这些宏，不是只作用于导出函数。
- `#ifdef` / `#if` 可以放在函数体或辅助函数体内部，用来切换局部逻辑。
- 顶层 GLSL 函数、全局变量、struct 等声明不能包在 `#ifdef` / `#if` 里；节点解析器需要稳定的顶层 API。需要可选 helper 时，把 helper 保持为顶层固定函数，把条件分支移进函数体。
- 顶层只包含预处理器指令的条件块可以保留，例如按宏切换 `#define` 常量。

使用和布局建议：

- 影响 shader 编译结构、helper 函数、采样路径或较重效果开关的选项，优先放进 `@glsl_defines`。
- 需要被其他节点连线驱动、每个像素可能变化的值，仍然用普通函数参数，不要放进宏。
- `Defines` 面板默认可以 `closed=true`，适合放编译期开关、性能档位、调试模式；如果它是日常必须调的主模式，可以写 `closed=false`。
- 互斥编译模式用 `int items` 下拉，不要暴露多个互相冲突的 bool 宏。
- 宏下拉默认隐藏前置 label，让面板更紧凑；多个下拉连续出现或没有清楚分组语义时，再给该宏写 `show_label=true`。
- `label` 应写给用户看的短名称；`description` 用来说明性能、视觉影响或编译期开关的代价，不要把这些解释写进宏名。

示例：

```glsl
/* @glsl_defines v1 closed=true
@define USE_RIM bool default=true label="Rim" description="Compile rim highlight"
@define MODE int default=1 items="0:Base;1:Half;2:Boost" label="Mode" description="Compile-time branch mode"
*/

vec3 rim_helper(vec3 color)
{
#ifdef USE_RIM
  color += vec3(0.1);
#endif
  return color;
}

vec3 stylize_with_defines(vec3 color)
{
#if MODE == 2
  color *= 0.5;
#endif
  return rim_helper(color);
}
```
