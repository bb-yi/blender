# Blender 5.1 NPR Port GLSL Function 节点转换指南

把 GLSL / Shadertoy / HLSL / ShaderLab 片段稳定转换成 `GLSL Function` 节点可直接使用的 GLSL 源码。

---

## 一、节点支持的边界类型

| 类别 | 允许的类型 |
|---|---|
| 输入参数 | `float` `int` `bool` `vec2` `vec3` `vec4` `mat2` `mat3` `mat4` `sampler2D` |
| `out` 参数 | `float` `int` `bool` `vec2` `vec3` `vec4` `mat2` `mat3` `mat4` |
| 返回值 | `void` `float` `int` `bool` `vec2` `vec3` `vec4` `mat2` `mat3` `mat4` |

**不支持**：`inout`、`out sampler2D`、`struct`/`array` 边界、非方阵矩阵（`mat2x3` 等）、递归、全局 uniform 依赖。

函数体内部不受此限制——`if`/`for`/`while`/`uint`/`textureLod`/`dFdx` 等均可正常使用。辅助函数也不必遵守边界类型限制，只有节点 `Function` 选中的导出函数必须满足。

### UI 要点

- `sampler2D` 显示为 Closure 输入口，通过 `Image to Closure` 或 `Closure Output` 接入
- `vec3` + `subtype=color` → 颜色插口；`vec4` → 拆成 `vec3 + float W`
- `mat2/3/4` → 按列拆成多个向量插口
- `int`/`bool` 已支持，可直接做模式开关

### NPR Tree 图像句柄采样

NPR Input / AOV Input / NPR Refraction 等图像句柄仍通过 `sampler2D` 闭包输入，但 `texture(image, coord)` 的 `coord` 是 **像素偏移量**，不是 mesh UV：

- `vec2(0.0)` = 采当前像素
- `Pixel` 模式下 `vec2(1.0, 0.0)` = 右移 1 像素

闭包内部用 `Image Sample.Image + Offset`，参数应命名为 `offset`/`radius` 而非 `uv`。

---

## 二、转换规则

### 规则 1：输出普通 GLSL 函数，不是 shader 文件

删除 `main()`/`mainImage()`/`frag()`/`Pass`/`Properties`/`CGPROGRAM`/`#version`/`precision`/`layout(...)`/`SHADERDATA`/`proc:` 等所有引擎外壳。

只保留：常量、辅助函数、一个导出函数。

### 规则 2：给出明确的 Function 名

```text
Function: your_function_name
```

### 规则 3：导出函数必须补全 `@glsl_meta v1`

**Meta 必须紧贴导出函数**。不能放在文件开头或辅助函数上方——Meta 和导出函数之间不能夹任何函数、`const`、`#define` 或顶层声明。

```glsl
/* ❌ 错误：Meta 在辅助函数上方 */
/* @glsl_meta v1 uv: label="UV" ... */
float hash21(vec2 p) { ... }   ← Meta 被错误地解析给 hash21

vec4 real_export(vec2 uv) { ... }  ← 导出函数反而没有 Meta

/* ✅ 正确：Meta 紧贴导出函数 */
float hash21(vec2 p) { ... }

/* @glsl_meta v1
uv: label="UV" default=vec2(0.0) description="Texture coordinates"
*/
vec4 real_export(vec2 uv) { ... }
```

### 规则 4：所有外部输入改成函数参数

`iTime`→`float time`、`iResolution.xy`→`vec2 resolution`、`fragCoord`→`vec2 frag_coord`、`_MainTex`→`sampler2D tex`、`i.uv`→`vec2 uv`、`_Time.y`→`float time`、`_ScreenParams.xy`→`vec2 resolution`。别名宏（`#define time iTime`）也要展开。

### 规则 5：贴图统一 `sampler2D`

HLSL `Texture2D + SamplerState`、Unity `sampler2D _MainTex` 统一改成 `sampler2D tex`，采样用 `texture(tex, uv)`。依赖显式 LOD 时可保留 `textureLod(tex, uv, lod)`，并提醒使用 `Image to Closure`。

### 规则 6：收敛不稳定写法

- 宏开关：收敛成当前启用分支，或改成函数参数
- 反向 `smoothstep(a,b,t)`（`a>b`）：写自己的稳定辅助函数
- 别名宏 `#define T u_time`：展开成真实参数
- 共享全局状态 `float gTime;`：改成显式参数传递
- `bool` 隐式转 `float`（`vec3(x==0.0,...)`）：改成显式三元
- 非方阵 `mat3x2` 双向乘法：拆成语义明确的 `dot(...)` helper
- 删除死代码、注释旧代码、未使用辅助函数

### 规则 7：上一帧反馈 / 多 pass 缓冲

Shadertoy `iChannel1` 等如果是上一帧/Buffer 通道，不能假装等价于普通 `sampler2D`。要么删除、要么近似、要么改成普通输入参数，但必须说明是"近似改写"。

### 规则 8：附转换结果说明

明确标注 `成功`/`部分成功`/`失败`，列出删除、近似、alpha 处理、验证情况。结果报告语言跟随用户语言。

---

## 三、`@glsl_meta v1` 语法

### 基本格式

```glsl
/* @glsl_meta v1
param_name: label="显示名" default=... min=... max=... subtype=... description="..."
*/
vec3 my_function(vec3 param_name) { ... }
```

**GLSL 参数名必须是合法英文标识符**。中文/可读名称写进 `label`，不要给参数起中文名。

### 支持的键

| 键 | 适用类型 | 说明 |
|---|---|---|
| `label` | 所有 | UI 显示名，支持中文/空格 |
| `default` | float/vec2/3/4 | 标量或 GLSL 表达式；表达式会隐藏数值框 |
| `min` / `max` | float/int | 数值范围 |
| `subtype` | float/vec* | `factor`/`color`/`xyz`/`translation`/`percentage`/`angle`/`time` 等 |
| `description` | 输入参数 | tooltip 文本 |
| `hide_value` | 输入参数 | 隐藏数值框，保留 socket |
| `items` | int | 下拉菜单 `"0:Label;1:Other"` |
| `show_label` | int items | 下拉前是否显示参数名 |

### 各参数类型的 Meta 写法

| 参数类型 | 应写的 Meta |
|---|---|
| `float` 强度/混合/阈值/柔和度 | `label` + `default` + `min/max` + `subtype=factor` + `description` |
| `float` 普通数值 | `label` + `default` + `description` |
| `int` 固定模式 | `label` + `default` + `items="0:A;1:B"` |
| `int` 连续范围 | `label` + `default` + `min/max` |
| `vec2/3` 坐标/偏移/比例 | `label` + `default=vec*(...)` + `description` |
| `vec3` 颜色 | `label` + `default=vec3(...)` + `subtype=color` |
| `vec3` 方向/法线/视线 | `label` + `default` + `description`；**不要用 `subtype=direction`** |
| `sampler2D` | 只写 `label` + `description` + panel |
| `out` 参数 | 只写 `label` |
| 返回值 | 不写 Meta |

### ⚠ `subtype=direction` 禁忌

法线、方向、视线、切线等向量参数**不要使用 `subtype=direction`**——它会在 UI 中显示成球形方向控件，极难直接输入。用 `subtype=xyz` 或省略 subtype。

```glsl
/* ✅ 正确 */
normal_ws: label="World Normal" default=normalize(glsl_normal()) hide_value=true description="Surface normal"

/* ❌ 错误：球形方向控件 */
normal_ws: label="World Normal" default=normalize(glsl_normal()) subtype=direction
```

### `@panel` / `@end_panel` 分组

参数超过 4 个或语义能分组时**必须**分组：

```glsl
/* @glsl_meta v1
uv: label="UV" default=vec2(0.0) description="UV coordinates"

@panel "Color" closed=false
base_color: label="Base Color" default=vec3(1.0) subtype=color description="Base color"
tint: label="Tint" default=vec3(0.8,0.6,0.2) subtype=color description="Tint color"
@end_panel

@panel "Advanced" closed=true
threshold: label="Threshold" default=0.5 min=0.0 max=1.0 subtype=factor description="Cutoff"
@end_panel
*/
vec3 effect(vec2 uv, vec3 base_color, vec3 tint, float threshold)
{
  return mix(base_color, tint, threshold);
}
```

- `closed=true`：高级/很少改的参数
- `closed=false`：主要工作流参数
- 只支持一级面板，不支持嵌套
- 必须显式 `@end_panel` 闭合

### 表达式默认值

```glsl
position_ws: default=glsl_position()
normal_ws: default=normalize(glsl_normal())
mask: default=(smoothstep(0.2, 0.8, glsl_position().z))
```

表达式默认值会隐藏数值框，未连接时使用表达式，连线后使用连线值。可引用源码内的 top-level helper 或内置 helper（`glsl_position()`/`glsl_normal()`/`glsl_incoming()`/`glsl_ambient_lighting()`）。

---

## 四、内置 Helper

| Helper | 对齐节点 | 语义 |
|---|---|---|
| `glsl_position()` | Geometry.Position | 世界空间位置 |
| `glsl_normal()` | Geometry.Normal | 世界空间法线 |
| `glsl_true_normal()` | Geometry.True Normal | 几何法线 |
| `glsl_incoming()` | Geometry.Incoming | 指向相机的方向 |
| `glsl_ambient_lighting()` | Shader Info.Ambient Lighting | 环境间接漫反射 |

### Eevee 逐灯 Helper

```glsl
int count = glsl_light_count();
GLSLLight light = glsl_light_get(i);
// light.vector: 指向灯中心的归一化方向
// light.position: 灯世界坐标（Sun 返回 vec3(0.0)）
// light.direction: 灯朝向轴
// light.diffuse_color / specular_color: 对自定义模型友好的颜色项
// light.attenuation: 基础衰减（不含 NdotL/shadow/GGX）
// light.type: GLSL_LIGHT_TYPE_SUN/POINT/SPOT/AREA_RECT/AREA_ELLIPSE
// light.lightgroup_id: 灯组 ID
float shadow = glsl_light_shadow(i, N);
```

推荐组合：`diffuse_brdf * light.diffuse_color * light.attenuation * NdotL * shadow`

限制：只支持 Eevee 物体材质的 Deferred/Forward 路径 direct light + shadow。不支持 FILTER/NPR Tree/World，不包含 probe/indirect/volume。

---

## 五、来源转换速查

### HLSL → GLSL 映射表

| HLSL | GLSL |
|---|---|
| `float2/3/4` | `vec2/3/4` |
| `half`/`fixed` | `float` |
| `lerp(a,b,t)` | `mix(a,b,t)` |
| `frac(x)` | `fract(x)` |
| `ddx/ddy` | `dFdx/dFdy` |
| `rsqrt(x)` | `inversesqrt(x)` |
| `saturate(x)` | `clamp(x,0.0,1.0)` |
| `tex2D(tex,uv)` | `texture(tex,uv)` |
| `mul(a,b)` | 按矩阵方向手动改写 |
| `: SV_Target` / `: TEXCOORD0` | 删除 |

### Shadertoy 转换

- `mainImage(out vec4 fragColor, in vec2 fragCoord)` → `vec4 effect(vec2 frag_coord, vec2 resolution, float time)`
- `iTime`→参数、`iResolution`→参数、`iMouse`→参数或删除
- `iChannel0~3`→`sampler2D`（区分静态贴图和反馈通道）
- 删除 Buffer A/B/C/D 上一帧反馈

### ShaderLab 转换

只抽取 `CGPROGRAM`/`HLSLPROGRAM` 中的核心逻辑。删除 `Shader`/`SubShader`/`Pass`/`Tags`/`Blend`/`ZTest`/`Cull`/`Stencil`/`Properties`/`#pragma`/`Fallback`。

---

## 六、边界类型改写速查

| 错误写法 | 正确写法 |
|---|---|
| `float test(int mode, bool enabled)` | `float test(float mode, float enabled)` 内部转 `int(mode)`/`enabled>0.5` |
| `ResultData effect(...)` | `float effect(..., out vec3 color)` |
| `void do_nothing(vec2 uv)` | `void do_something(vec2 uv, out vec3 color)` |
| `mat3x2 tri; vec2 * tri` | 拆成 `dot(...)` helper |
| `#define S(a,b,t) smoothstep(a,b,t)` + `S(0.4, 0.0, d)` | 写稳定辅助函数 |
| `vec3(x==0.0, ...)` | `vec3(x==0.0 ? 1.0 : 0.0, ...)` |
| `float gTime;` 全局共享 | 改成显式参数 |

---

## 七、AI 输出格式

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
/* @glsl_meta v1
uv: label="UV" default=vec2(0.0) description="Texture coordinates"
time: label="Time" default=0.0 description="Animation time in seconds"
tex: label="Texture" description="Texture closure source"
*/
vec3 effect_name(vec2 uv, float time, sampler2D tex)
{
  return texture(tex, uv).rgb;
}
```

转换结果:
- 状态: 成功 / 部分成功 / 失败
- 最终调用函数: effect_name
- 不支持或已删除: ...
- 已近似或已替代: ...
- Alpha 处理: 已保留 / 已拆分 / 原始恒定 / 已省略
- 验证情况: 已解析 / 已渲染 / 未验证
````

---

## 八、完整示例

### 示例 1：Shadertoy → 节点函数

```glsl
// 原始
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
  vec2 uv = fragCoord / iResolution.xy;
  vec3 col = 0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0.0, 2.0, 4.0));
  fragColor = vec4(col, 1.0);
}
```

```glsl
// 转换后  Function: shadertoy_color
/* @glsl_meta v1
frag_coord: label="Fragment Coordinate" default=vec2(0.0) description="Pixel coordinate"
resolution: label="Resolution" default=vec2(1920.0, 1080.0) description="Screen resolution"
time: label="Time" default=0.0 description="Animation time"
*/
vec4 shadertoy_color(vec2 frag_coord, vec2 resolution, float time)
{
  vec2 uv = frag_coord / resolution;
  vec3 col = 0.5 + 0.5 * cos(time + uv.xyx + vec3(0.0, 2.0, 4.0));
  return vec4(col, 1.0);
}
```

### 示例 2：HLSL → 节点函数

```hlsl
// 原始
float4 frag(v2f i) : SV_Target {
  float wave = sin(_Time.y + i.uv.x * 10.0);
  float3 col = lerp(float3(0.2,0.4,0.8), float3(1.0,0.9,0.2), saturate(wave));
  return float4(col, 1.0);
}
```

```glsl
// 转换后  Function: hlsl_wave_color
/* @glsl_meta v1
uv: label="UV" default=vec2(0.0) description="Texture coordinates"
time: label="Time" default=0.0 description="Animation time"
*/
vec4 hlsl_wave_color(vec2 uv, float time)
{
  float wave = sin(time + uv.x * 10.0);
  vec3 col = mix(vec3(0.2,0.4,0.8), vec3(1.0,0.9,0.2), clamp(wave, 0.0, 1.0));
  return vec4(col, 1.0);
}
```

### 示例 3：长参数列表 + 面板分组

```glsl
// 转换后  Function: raymarched_plane_hole
/* @glsl_meta v1
uv: label="UV" default=vec2(0.0) description="Plane UV in [0,1]"
center: label="Center" default=vec2(0.5, 0.5) description="Hole center"
outer_size: label="Outer Size" default=vec2(0.86, 0.62) description="Outer rect size"

@panel "Shape" closed=true
wall_thickness: label="Wall Thickness" default=0.17 min=0.02 max=0.45 subtype=factor
corner_radius: label="Corner Radius" default=0.09 min=0.0 max=0.35 subtype=factor
depth: label="Depth" default=0.58 min=0.05 max=2.0 subtype=factor
edge_softness: label="Edge Softness" default=0.018 min=0.001 max=0.08 subtype=factor
@end_panel

@panel "Noise" closed=true
noise_frequency: label="Noise Frequency" default=34.0 min=0.0 max=120.0
noise_amplitude: label="Noise Amplitude" default=0.018 min=0.0 max=0.08 subtype=factor
@end_panel

@panel "Color" closed=false
base_color: label="Base Color" default=vec3(0.12, 0.12, 0.12) subtype=color
wall_color: label="Wall Color" default=vec3(0.38, 0.20, 0.68) subtype=color
floor_color: label="Floor Color" default=vec3(0.18, 0.08, 0.28) subtype=color
rim_color: label="Rim Color" default=vec3(1.00, 0.52, 0.18) subtype=color
@end_panel

@panel "Lighting" closed=true
light_dir: label="Light Direction" default=vec3(0.35, 0.25, 0.90) description="Directional light direction"
normal_ws: label="World Normal" default=normalize(glsl_normal()) hide_value=true description="Plane normal"
view_ws: label="View Direction" default=normalize(glsl_incoming()) hide_value=true description="Surface to camera"
@end_panel
*/
vec4 raymarched_plane_hole(vec2 uv, vec2 center, vec2 outer_size,
                           float wall_thickness, float corner_radius,
                           float depth, float edge_softness,
                           float noise_frequency, float noise_amplitude,
                           vec3 base_color, vec3 wall_color,
                           vec3 floor_color, vec3 rim_color,
                           vec3 light_dir, vec3 normal_ws, vec3 view_ws)
{
  // ...实现...
  return vec4(base_color, 1.0);
}
```

---

## 九、`@glsl_defines v1` 编译期宏

用于编译期开关，和 `@glsl_meta` 独立。生成的 `#define` 对辅助函数也可见。

```glsl
/* @glsl_defines v1 closed=true
@define USE_RIM bool default=true label="Rim" description="Compile rim highlight"
@define MODE int default=1 items="0:Base;1:Half;2:Boost" label="Mode"
*/

vec3 rim_helper(vec3 color)
{
#ifdef USE_RIM
  color += vec3(0.1);
#endif
  return color;
}

/* @glsl_meta v1
color: label="Color" default=vec3(1.0) subtype=color
*/
vec3 stylize(vec3 color)
{
#if MODE == 2
  color *= 0.5;
#endif
  return rim_helper(color);
}
```

- `bool` → 开关；关闭时不生成 `#define`
- `int` + `items` → 下拉；生成 `#define NAME value`
- `int` + `min/max` → 整数框
- 宏名最长 63 字符
- `#ifdef`/`#if` 可在函数体内部使用，但不能包住顶层函数/全局变量声明

---

## 十、AI 转换提示词

```text
把下面这段 shader 转换为 Blender 5.1 NPR Port 的 GLSL Function 节点可用 GLSL。

规则：
1. 输出普通 GLSL 函数源码，不要 shader 文件。
2. 给出 Function 名。
3. 导出函数正上方写完整的 @glsl_meta v1，紧贴函数，不能夹辅助函数。
4. GLSL 参数名用英文标识符；中文名写进 label。
5. 法线/方向/视线参数不要用 subtype=direction。
6. 参数超过 4 个用 @panel / @end_panel 分组。
7. 外部 uniform/time/resolution/mouse/贴图全改成函数参数。
8. 边界类型只用 float/int/bool/vec2/3/4/mat2/3/4/sampler2D 及 out。禁止 inout、out sampler2D。
9. HLSL/ShaderLab 去掉语义、Pass、Properties、pragma。
10. 贴图采样用 texture(tex, uv)；需要 LOD 用 textureLod。
11. 宏开关、反向 smoothstep、别名宏、全局共享状态、bool 隐式转换、非方阵矩阵 → 收敛成稳定版本。
12. 多输出用 out 参数，不用 struct。
13. 附转换结果：成功/部分成功/失败，列出删改和 alpha 处理。

输出格式：
Function: ...
Inputs: ...
Outputs: ...
Code: ```glsl ... ```
转换结果: ...

待转换代码：
```
