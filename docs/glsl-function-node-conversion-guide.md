# Blender 5.1 NPR Port GLSL Function 节点转换指南

## 文档目的

这份文档是写给 `AI`、脚本生成器、自动转换工具的。

目标不是介绍着色器原理，而是让另一个 AI 在阅读这份文档后，能够把：

- 普通 `GLSL`
- `Shadertoy GLSL`
- `HLSL`
- 一部分 `ShaderLab / Unity CGPROGRAM / HLSLPROGRAM`

稳定地转换成可直接粘贴到 `Blender 5.1 NPR Port` 的 `GLSL Function` 节点里的、符合当前实现规范的 `GLSL` 源码。


## 一、先记住当前节点真正支持什么

`GLSL Function` 节点当前是“把一段用户 GLSL 函数源码注入到 Eevee 材质编译链里”。

它不是完整的 Shadertoy 运行时，也不是完整的 Unity / Unreal shader framework。

### 1. 当前可用的函数接口类型

选中的导出函数，其参数和返回值只允许使用以下边界类型：

- 输入参数 `in`
  - `float`
  - `vec2`
  - `vec3`
  - `vec4`
  - `sampler2D`
- 输出参数 `out`
  - `float`
  - `vec2`
  - `vec3`
  - `vec4`
- 返回值
  - `void`
  - `float`
  - `vec2`
  - `vec3`
  - `vec4`

### 2. 当前不支持的函数接口写法

- `inout`
- `out sampler2D`
- `int` / `bool` / `mat*` / `struct` / `array` 作为函数边界类型
- 多返回值结构体
- 递归
- 依赖外部运行时注入的全局 uniform 体系

### 3. 当前和 UI 相关的重要事实

- `Function` 现在必须显式指定，不会自动选第一个函数
- `sampler2D` 在节点图形界面中不会显示成可连线输入口
- `sampler2D` 对应的图片是在节点参数区直接选择
- 允许存在多个 `sampler2D` 参数，每个参数都在节点参数区单独选图
- 只要某个 `sampler2D` 参数还没选图，节点的 `parse status` 就可能保持 `ERROR`
- `sampler2D` 当前不支持直接连线输入
- `sampler2D` 当前不支持 `UDIM` 平铺图片
- 所有 `sampler2D` 使用同一组节点级采样设置：
  - `Sampler Interpolation`
  - `Sampler Extension`
- `vec4` 虽然是合法边界类型，但当前 UI 中仍按“向量插口”处理，不会变成专门的颜色插口；如果只需要 `RGB`，优先考虑 `vec3`

### 4. 内部实现和边界接口要区分

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
- “节点导出函数的参数和返回值”必须严格受限
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

### 规则 4：如果来源代码依赖贴图采样，优先保留为 `sampler2D`

例如：

- HLSL `Texture2D + SamplerState`
- Unity `sampler2D _MainTex`
- GLSL `sampler2D`

统一改成：

```glsl
sampler2D tex
```

基础采样通常改成：

```glsl
texture(tex, uv)
```

如果原算法明确依赖显式 `LOD`、模糊级别、景深级别等采样语义，也可以在函数体内部保留例如：

```glsl
textureLod(tex, uv, lod)
```

也就是说：

- 边界类型统一成 `sampler2D`
- 函数体内部不必强行把所有采样都降级成最基础的 `texture`

不要生成“需要把图片节点连进来”的说明，因为当前 `sampler2D` 不是那种工作流。

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

- 如果某个通道本质上就是“普通静态 2D 图片采样”，改成 `sampler2D`
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
- 验证情况: 已解析 / 已渲染 / 未验证
````

如果函数有额外 `out` 参数，也写清楚：

```text
Outputs:
- return: float
- out color: vec3
```

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

### 1. `int` / `bool` 不能作为节点接口

不要生成这种导出函数：

```glsl
float test(int mode, bool enabled)
```

应该改成：

```glsl
float test(float mode, float enabled)
{
  int mode_i = int(mode);
  bool enabled_b = enabled > 0.5;
  ...
}
```

### 2. `mat*` 不能作为节点接口

不要把矩阵暴露为函数参数或返回值。

应改成：

- 传入需要的若干 `vec*`
- 或在函数内部自己构造矩阵

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
9. 如果用了纹理采样，是否已经改成 `texture(tex, uv)`，或者在确实需要时保留为 `textureLod(tex, uv, lod)`？
10. 如果导出函数返回 `void`，是否仍然通过 `out` 参数暴露了至少一个输出？
11. 如果有 `sampler2D` 参数，是否提醒了使用者在节点参数区为每个 `sampler2D` 选择图片？
12. 如果有 `sampler2D` 参数，是否错误假设它支持连线输入或 `UDIM` 平铺图片？
13. 如果源码里有宏开关、注释掉的旧代码、未使用辅助函数，是否已经收敛或删除？
14. 如果源码里有反向 `smoothstep` 或类似依赖实现细节的捷径写法，是否已经改成稳定辅助函数？
15. 如果源码依赖上一帧反馈、Buffer A/B/C/D、多 pass 中间结果、视频或键盘通道，是否已经明确说明删除、近似或替代方案？
16. 如果来自 HLSL，是否已去掉所有语义标注？
17. 如果来自 ShaderLab，是否只保留了核心逻辑？
18. 是否避免依赖特定引擎的 include 和无法离开原运行时的宏？
19. 最终输出里是否明确写了本次转换是 `成功`、`部分成功` 还是 `失败`？
20. 最终输出里是否再次明确写了节点 `Function` 一栏最终应调用的函数名？
21. 最终输出里是否明确列出了不支持、删除、近似、替代和验证情况？
22. 最终输出里的结果报告语言，是否跟随了用户当前使用的语言？
23. 是否移除了 `precision` / `#version` / `layout(...)` / `SHADERDATA` / `proc:` 这类文件级声明或工具元数据？
24. 如果源码通过 `#define time iTime` 之类的别名宏引用运行时变量，是否已经展开并参数化？
25. 如果源码里用了共享可变全局状态（如 `gTime`），是否已经改成显式参数或局部变量？
26. 如果源码里有把比较结果直接拿去构造 `vec*` 或参与数值运算的写法，是否已经改成显式数值表达式？
27. 如果源码里用了 `mat3x2` / `mat2x3` 或其他方向不够直观的矩阵乘法，是否已经改写成更清晰的 helper？
28. 如果原 shader 的 alpha 有实际含义，是否已经明确说明保留、拆分还是省略？


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
把下面这段 shader 代码转换为 Blender 5.1 NPR Port 的 GLSL Function 节点可直接使用的 GLSL。

必须遵守这些规则：
1. 最终结果只能是普通 GLSL 函数源码，不要输出完整 shader 文件。
2. 明确给出 Function 应设置的函数名。
3. 把所有外部 uniform / 时间 / 分辨率 / 鼠标 / 贴图输入改成函数参数。
4. 导出函数的参数和返回值只允许使用 float、vec2、vec3、vec4、sampler2D，以及 out float/vec2/vec3/vec4。
5. 不允许使用 inout，不允许 out sampler2D。
6. 如果来源是 HLSL 或 ShaderLab，去掉语义、Pass、Properties、pragma 和引擎包装层。
7. 贴图采样统一改成 `texture(tex, uv)`；如果原算法明确依赖显式 `LOD`，可以保留为 `textureLod(tex, uv, lod)`。
8. 如果存在宏开关、死代码、未使用辅助函数、反向 `smoothstep`、运行时别名宏、共享可变全局状态、布尔到数值隐式转换、非方阵矩阵双向乘法这类不稳定写法，要收敛成稳定版本。
9. 如果需要多个输出，用 out 参数，不要用 struct 返回。
10. 如果原 shader 的 alpha 有意义，要明确说明是保留、拆分还是省略。
11. 在结果最后明确说明转换是 success / partial / failed，并列出不支持、删除、近似、替代、alpha 处理、验证情况。
12. 输出格式为：
   Function: ...
   Inputs: ...
   Outputs: ...
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
5. 输出一个明确可选的导出函数

只要严格遵守这份文档，绝大多数数学类 GLSL、很多 HLSL 片段逻辑、以及相当一部分 ShaderLab 片段逻辑，都可以被稳定改写为当前 Blender 节点可直接使用的版本。
## 附录：GLSL Function Meta 语法

`GLSL Function` 节点支持从 GLSL 源码里的块注释读取少量 Meta 信息，用来描述输入参数在 Blender 节点界面中的默认值、范围和子类型。

这个 Meta 系统只负责节点 UI 语义，不改变 GLSL 函数逻辑本身。

当前范围主要是：

- 默认值
- 最小值
- 最大值
- socket subtype

### 1. 基本格式

Meta 必须写在函数正上方的块注释里，并以 `@glsl_meta` 开头：

```glsl
/* @glsl_meta v1
strength: default=0.5 min=0.0 max=1.0 subtype=factor
tint: default=vec3(1.0, 0.8, 0.2)
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

#### 2.1 推荐结构

```glsl
/* @glsl_meta v1
strength: default=0.5
tint: default=vec3(1.0, 0.8, 0.2)
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

示例：

```glsl
strength: default=0.5
uv_scale: default=vec2(1.0, 1.0)
tint: default=vec3(1.0, 0.8, 0.2)
color_a: default=vec4(1.0, 0.5, 0.2, 1.0)
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

#### 3.4 `subtype`

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
- `direction`
- `velocity`
- `acceleration`
- `euler`
- `xyz`

示例：

```glsl
strength: default=0.5 min=0.0 max=1.0 subtype=factor
offset: default=vec3(0.0) subtype=translation
normal_dir: default=vec3(0.0, 0.0, 1.0) subtype=direction
```

### 4. 行为规则

#### 4.1 默认值同步规则

`default` 不会在每次 redraw 时反复覆盖用户手动修改的 socket 值。

当前逻辑是：

- 当 Meta 内容第一次生效时，把 `default` 写入 socket 默认值
- 只要 Meta 没变，用户手动改过的默认值会保留
- 当 Meta 本身发生变化时，再同步一次新的默认值

这意味着它更适合作为“函数作者建议值”，而不是强制锁死值。

#### 4.2 `min/max/subtype` 的作用

这三项属于 socket 声明的一部分，会直接影响 Blender 节点界面和 socket 类型。

例如：

- `subtype=factor` 会让 float 输入变成 `NodeSocketFloatFactor`
- `subtype=xyz` 会让 vector 输入变成 `NodeSocketVectorXYZ`

### 5. 当前限制

当前版本有这些限制：

- 只支持输入参数
- 不支持返回值 Meta
- 不支持 `out` 参数 Meta
- 不支持 `inout`
- 不支持 `sampler2D` Meta
- 不支持 `int` / `bool` / `mat*` / `struct` / `array` 边界参数 Meta
- 如果 Meta 指向了不存在的参数，会报错
- 一个函数当前只支持一块 Meta

### 6. 推荐写法

```glsl
/* @glsl_meta v1
threshold: default=0.35 min=0.0 max=1.0 subtype=factor
edge_width: default=0.08 min=0.0 max=1.0 subtype=factor
edge_color: default=vec3(1.0, 0.5, 0.1)
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

#### 7.3 给 `sampler2D` 写 Meta

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

`sampler2D` 当前由节点上的图片选择器处理，不走这个 Meta 通道。

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

- 可以把“建议默认值”写进 Meta，而不是只写在说明文字里
- 可以把“参数范围”写进 Meta，减少手动调节点的成本
- 可以把 Blender 语义明确的参数标为合适 subtype
- 不要把运行时资源、贴图选择、函数逻辑分支控制错误地塞进 Meta

最稳妥的理解是：

- GLSL 函数体负责算法
- Meta 注释负责节点 UI 语义
