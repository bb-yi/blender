# GLSL Function 节点转换指南

面向 GLSL、Shadertoy、HLSL 和 ShaderLab 的代码转换。核对版本：**Blender 5.2.2 NPR `1c82f84cbebf`（2026-10-26）**，对应 `blender_npr_post_mainfix/main`。
本指南针对 Eevee；Cycles 不执行这些自定义 GLSL，也不会自动转换为 OSL。

## 1. 先选节点和入口

| 节点 | 填写内容 |
| --- | --- |
| **GLSL Function** | 普通 GLSL 函数源码；可包含顶层 helper、具名结构体和 Meta，支持纹理、循环和多输出 |
| **GLSL Script Expression** | 单个数值表达式，如 `mix(a, b, factor)`；不写分号、return、函数定义、声明、赋值、注释或预处理指令 |

Expression 的 Float 变量是 GLSL `float`，不能直接做整数位运算；变量名、类型和向量维度由节点配置，4D 的 W 与 Color Alpha 有独立插口。不要把 Function 的 Meta、Bundle 或拆分规则套到 Expression。

**Function 留空即 Auto**：优先选择唯一带 `@glsl_meta` 的普通函数；没有这类候选时，选择唯一未被其他函数体调用的外层函数。`@glsl_closure` helper 不参与候选。多个候选会提示名称，需手动选择；显式填写名字则锁定入口，源码改名后不会自动改锁定值。通常只给导出入口加 Meta。

源码、函数名与 Node/Code 切换位于默认展开的 **Code Settings** 面板，折叠状态随节点保存；输入输出插口留在面板外。
Code 编辑先进入草稿，Apply 成功后才更新已应用源码和接口；Discard 丢弃草稿。预验证失败保留旧源码与连线；后续接线、资源或异步 GPU 错误由材质编译报告。共享 Text 的 Apply 会检查全部使用者和并发草稿；外部文件只读，需 Make Internal 后才能在节点内编辑。

## 2. 公开接口与类型

省略参数限定词等同于 `in`。边界规则适用于导出函数，普通 helper 和函数内部可使用更广的合法 GLSL；GLSL 不支持在函数体内再定义函数。

| 类型 | 输入 | out / 返回值 |
| --- | --- | --- |
| `float / int / bool` | 对应标量 | 支持 |
| `vec2 / vec3` | 对应向量；vec3 可用 `subtype=color` 改为 RGB Color | 支持 |
| `vec4` | 普通参数拆成 XYZ Vector + W Float，调用前重组 | 同样拆分 |
| `mat2 / mat3 / mat4` | 按 GLSL 列优先拆分；mat4 每列再拆 XYZ + W | 同样拆分 |
| 顶层具名纯数据 `struct`，可嵌套 | 一个 Bundle | 一个 Bundle |
| `sampler2D / sampler3D / samplerCube` | Closure 插口，按资源类型连接 | 不支持 |
| `void` | 不适用 | 允许返回，但必须至少有一个 out |

- 导出边界不支持 `inout`、数组、非方阵、`uint/uvec*/ivec*/bvec*`、匿名结构体或引擎内建结构体。struct 内也不能有资源、Closure、数组或递归成员。
- int 在函数内部是整数，**跨节点只有 `[-16777216, 16777216]` 保证精确**；不要用它传递完整 32 位 UID/哈希。普通参数、Bundle 和 typed Closure 都受传输精度约束。
- 普通 vec3 Color 输入只读取 RGB；普通 vec4 即使写 `subtype=color` 仍拆为 XYZ/W。不要丢失 W 或将其误当作自动材质透明度。
- 参数名须避开生成的 W/矩阵列标识符冲突；显示标签与 GLSL 名称分开，中文 label 不改变连接标识符。

## 3. Meta v1/v2

Meta 紧贴它所属函数的上方，一函数一块。节点允许无 Meta，但本指南及本地验证器要求交付代码补全公开参数 Meta。无版本写法按 v1 处理。

| 目标 | 写法与限制 |
| --- | --- |
| 普通输入参数 | v1/v2：`name: label=...`，其余属性须符合类型 |
| 普通非结构体 out | **仅 label** |
| 普通非结构体返回值 | 不接受 Meta；v2 的 `return:` 也不适用 |
| 结构体输入成员 | v2：`surface.field` 或更深路径；根/嵌套包只写 label、description |
| 返回结构体 / out 结构体 | v2：`return.field` / `out_name.field`；根支持 label、description，成员另可写类型允许的 subtype |

`return` 专指结构体返回值，不是默认输出标签 `Result`；不要写旧的 `function_name.param` 目标。

### 常用属性

| 属性 | 用途与边界 |
| --- | --- |
| `label="名称"`、`description="说明"` | 名称/tooltip，不改变字段键；单行文本可包含中文、空格，内部引号写 `\"`、反斜杠写 `\\` |
| `default=...` | 标量、向量、方阵的类型匹配字面量或 GLSL 表达式；整个结构体和 sampler 不接受 default |
| `min=... max=...` | 语义明确时设置输入范围；int 使用整数字面量，bool/矩阵不使用；min 不得大于 max |
| `subtype=...` | float 常用 factor/angle/distance/time；向量常用 xyz/translation/color，color 仅 vec3/vec4；int/bool/矩阵不使用 |
| `items="0:关闭;1:开启"` | 普通 int 输入或 int Define 的枚举；default 必须是其中的整数字面量，值不重复、名称不为空，不与 min/max 混用 |
| `show_label=true` | 仅配合 int items；默认隐藏枚举前置名称 |
| `hide_value=true` | 普通输入只隐藏数值框，仍可连线；不是删除接口 |

颜色应明确写 `subtype=color`，法线/方向用 `subtype=xyz` 或省略并说明坐标空间。转换规范不使用球形控件 `subtype=direction`，本地验证器也会拒绝它。
**sampler 只接受 label、description 和输入 panel 分组**。结构体成员不接受 items/show_label/hide_value；输入矩阵成员只支持 label/description/default，输出成员不能写 default/min/max，输出矩阵只写 label/description。

### 默认值的两种触发方式

- **普通参数**：字面量初始化新插口，刷新不覆盖同名插口的用户数值；表达式不写入静态 RNA 值，只在参数未连线时使用，并隐藏数值框。vec4/矩阵只要任一拆分插口连线，就改用整组插口重建，其余分量取自身插口值。
- **Bundle 成员**：仅在字段缺失时使用默认值，每个缺失成员求值一次；字段存在但未连线时使用自身数值，包括零。类型错误不能触发缺省。整包/嵌套包缺失时须所有成员都可缺省；提供矩阵时必须有全部列，只有整个矩阵缺失才能用默认矩阵。
- 默认表达式可用同份源码的常量、宏、顶层 helper 和当前材质域支持的内建助手；不引用当前函数参数或其他默认字段。复杂表达式最外层加括号，保持单行。int items 不使用表达式 default。

### 输入分组与编译宏

以下为 Meta **片段**，放入导出入口的 Meta 块中：

```glsl
@panel "高级" closed=true
roughness: label="粗糙度" default=0.5 min=0.0 max=1.0 subtype=factor
@end_panel
```

panel 默认 closed=true，只支持一级且须闭合；只能容纳整个输入参数，不能标注成员或输出。较少调节的参数折叠，主要输入默认可见。

编译结构/性能档位用独立 `@glsl_defines v1`；连线驱动或逐像素值用函数参数。下面是可放在源码任意顶层位置的宏块：

```glsl
/* @glsl_defines v1 closed=true
@define USE_RIM bool default=true label="边缘光"
@define MODE int default=1 items="0:基础;1:增强" label="模式"
*/
```

bool 关闭时不生成对应 define；int 生成 `#define NAME value`。宏名合法且不超过 63 字符，不得重复；多个宏块的 closed 设置须一致。Defines 面板默认展开，可用 closed=true 折叠。
节点只提前裁剪可确定的条件：已知节点宏、受支持的无后缀有符号 32 位整数条件，以及 defined、比较、括号、!、&&、||。无法确定的表达式、无符号/宽整数、宏替换表达式和 GPU 后端宏保留给后端。
确定分支中可包含顶层声明；未知条件不能保护顶层函数、变量或 struct，应把分支移进固定签名的函数体。不要依赖任意 C 预处理表达式都能被节点分析器求值。

## 4. struct ↔ Bundle

| 结构体成员 | Bundle 表示 |
| --- | --- |
| float/int/bool | 对应标量 |
| vec2/vec3/vec4 | 完整 2D/3D/4D Vector，不拆 W |
| vec3/vec4 + subtype=color | RGB/RGBA Color，vec4 保留 Alpha |
| mat2/mat3/mat4 | 子包 `c0…cN-1`，每列为对应维度向量，不转置 |
| 嵌套结构体 | 同名子包 |

字段按完整路径、名称和类型绑定，区分大小写，与排列顺序无关。Color 可按对应向量读取 RGB/RGBA，数值向量维度须匹配。
允许额外输入字段，但输出只含结构体声明的字段；要保留其他数据，显式 Join Bundle，重复键沿用 Join 的优先级。

**创建和同步**：普通 Combine Bundle 接到函数输入、Separate Bundle 接到输出，再点标题栏同步图标或侧栏 Sync。空包连接只自动补必需字段；显式 Sync 还会初始化具有一致常量默认值的可选字段。表达式默认值、不同消费者的不同默认值及超出整数传输精确范围的默认值保持省略。
手动添加同名同类型字段会覆盖默认值，包括默认表达式；共享包的显式字段影响所有消费者。

Sync 保留现有标识符、数值、连线和额外字段，不猜测重命名、不删除已连接的旧字段；共享消费者类型冲突时原子报错，不做部分更新。函数接口变化不会静默重写共享来源。
最多嵌套 32 层，展开后每侧最多 512 个插口，普通参数和资源/Closure 也计入。非 3D Bundle 向量仅用于 Shader 树，不扩大 Geometry Nodes 或 Zone 的接口类型；新功能工程需要新版本读取。

详细规则见 [结构体与捆包](glsl-function-bundles.md)。

## 5. 纹理和 Closure：按来源选择

| 来源 | 边界 / 连接 | 坐标与限制 |
| --- | --- | --- |
| 普通 2D 图像 | sampler2D ← Image to Closure | UV 绝对坐标；需要 LOD、导数、尺寸或 texel 查询时优先走真实图像资源 |
| 程序化 2D / NPR 图像句柄 | sampler2D ← Closure Output | 只保证直接 `texture(tex, coord)`；坐标语义由闭包内部决定 |
| 程序化三维场/SDF | sampler3D ← Closure Output | Closure Input 的 `UV: Vector` 传完整 XYZ；只保证 `texture(volume, vec3)`，不支持 LOD/grad/size/fetch/gather |
| 硬件 3D LUT | sampler3D ← Image to Closure 的 3D LUT Strip | 原生 3D 纹理语义，不用程序化闭包近似插值 |
| Cube | samplerCube ← Image to Closure 的 Cube Strip | 六个等大方形面水平排列 +X/-X/+Y/-Y/+Z/-Z；方向采样、textureLod、textureSize(ivec2)，自动生成 mip |

Cube 不支持程序化 Closure、直接读取 DDS Cube 或保留游戏原始 mip 链；导入时核对面朝向。3D LUT/Cube 尺寸在 GPU 资源准备时检查，READY 不保证图像有效。
sampler2D/3D **真正未连接**时隐式使用不透明白色，不创建 Image 或 GPU 纹理；合法颜色查询返回 vec4(1)，textureSize 为对应维度的全 1。非法 GLSL 操作（如 3D textureGather）、显式错误连接或缺失图片仍报错；Cube 不适用此白色回退。
sampler Closure 的标准接口为 `UV: Vector`、`Color: RGBA`、`Alpha: Float`，新 Alpha 默认 1；旧 Color-only Closure 保持兼容。

**NPR 图像句柄的坐标是偏移，不是 mesh UV**：

```text
NPR/AOV 等图像句柄 → Image Sample.Image
Closure Input.UV → Image Sample.Offset
Image Sample.Color / Alpha → Closure Output → 函数 sampler2D
```

`texture(image, vec2(0))` 采当前像素；Pixel 模式下 `(1,0)` 表示右移一个像素。需要透明度时接出独立 Alpha，不能只保留 RGB。Shadertoy 的反馈/多 pass 仍需显式实现或说明近似，不能靠 sampler 自动补齐运行时。

Image to Closure 可经转接点、Bundle、嵌套节点组传递，保留资源与采样设置。接到空 Evaluate Closure 或 Sync 后，接口为 UV Vector → Color、Alpha；2D 取 XY、3D LUT 取 XYZ、Cube 取方向，未连接 UV 使用数值默认值 `(0,0,0)`。
这里的 `UV` 是闭包参数键。原生图像闭包兼容旧键 `Vector`，但明确存在 `UV` 时始终使用 `UV`（与插口顺序、是否连线无关）；坐标必须是单个三维向量。缺少坐标输入项或类型不符会在 Evaluate Closure 上报告编译错误，不再静默使用内部默认坐标。
仅当同步来源全部是 Image to Closure 时，显式 Sync 会将旧 `Vector` 迁移为 `UV`，保留兼容插口的标识、连线和默认值；普通自定义 Closure 和混合来源继续严格按各自参数键匹配。已存在但未连接的 `UV` 仍正常使用该插口的数值默认值。
解算 Color 保留 RGBA，同时另出 Alpha，别重复相乘。采样遵循原生 sampler，不额外套用 Image Texture 的 LOD 偏置/Alpha 处理；不增加 UDIM 支持，Cubic/Smart 仍使用既有线性过滤。

### Typed Closure callback

`@glsl_closure v1` 标记可由节点子图替换的普通 helper；它必须可从导出入口到达，未连接时执行原 helper 函数体。

- ABI 只允许 float/int/bool/vec2/3/4；必须有返回值或至少一个 out，不支持 sampler、矩阵、数组、struct、inout。返回插口保留名 Result，参数不能同名。
- 含 callback 的源码不允许重载/重复定义或可达递归；不通过函数式宏直接或间接调用 callback。
- header 只支持 label/description。输入 item 只支持 label/description 与 vec3 的 subtype=color；Result/out 可写类型适用的 subtype，但 v1 vec4 item 不支持 subtype。
- 不使用普通参数的 default、min/max、items、hide_value、show_label、closed 或 panel。节点组内按创建闭包的实例上下文展开。

标记 helper 的**片段**如下，仍需普通导出入口调用：

```glsl
/* @glsl_closure v1 label="Typed Remap"
color: subtype=color
Result: subtype=color
*/
vec3 typed_remap(vec3 color, float strength, out float mask)
{
  mask = clamp(strength, 0.0, 1.0);
  return color * mask;
}
```

## 6. Eevee helper 的适用范围

| helper | 含义 / 可保证范围 |
| --- | --- |
| glsl_position() / glsl_normal() / glsl_true_normal() / glsl_incoming() | 对齐 Geometry 对应输出；物体材质与 NPR 几何语义，Filter/World 不作稳定保证 |
| glsl_ambient_lighting() | Shader Info 的 Ambient Lighting，即 probe/环境间接漫反射；不含反射/完整间接总和，Filter/World 不作保证 |
| glsl_view_matrix() / glsl_projection_matrix() / glsl_view_projection_matrix()，以及各自 glsl_*_matrix_inverse() | 当前 draw 的视图/投影状态，含 overscan/crop/TAA jitter；Surface/NPR/Filter，顶点与片元阶段 |
| glsl_model_matrix() / glsl_model_matrix_inverse() / glsl_model_view_matrix() / glsl_model_view_projection_matrix() / glsl_normal_matrix() | 当前物体变换，Surface/NPR；Filter 模型矩阵回退单位阵，normal_matrix 是 object→world 法线变换 |
| glsl_light_count() / glsl_light_get(i) / glsl_light_shadow(i, N) | 仅普通 Eevee 物体材质的 Deferred/Forward 直接光与阴影；不用于 Filter/NPR/World，不负责 probe/volume/indirect |

World 应显式传入如 Texture Coordinate.Normal 的方向；不要从材质域可创建节点推断所有 helper 都可用。

### GLSLLight 与 Lightgroup ID

`glsl_light_get(i)` 的 i 是**当前着色点局部可见列表序号**，不是稳定场景编号。按灯光属性里的整数 Lightgroup ID 筛选时比较 `light.lightgroup_id`；0 表示第 0 组，同组可有多盏灯。

| 字段 | 语义 |
| --- | --- |
| valid / type / lightgroup_id | 是否有效、公开灯类型、灯光组 ID |
| vector / position / direction / distance | 着色点指向灯中心的归一化方向、灯中心世界坐标、灯的世界朝向轴、距离 |
| diffuse_color / specular_color | 适合手写光照模型的颜色项，不存在通用 color 或 roughness 字段 |
| attenuation | 基础衰减，不含 NdotL、BRDF、shadow、LTC、Fresnel 或材质 roughness |
| influence_radius | 表面光照有效影响半径，不是光源形状半径；太阳/无效灯为 0 |

日光 position 为零，direction 为日光方向；聚光/面光 direction 对应灯局部 +Z 的世界方向，点光 direction 为零。类型使用 `GLSL_LIGHT_TYPE_INVALID/SUN/POINT/SPOT/AREA_RECT/AREA_ELLIPSE` 常量。
`GLSLLight` 是引擎库类型，且有内部 uint index/UID，**不能直接作为 Bundle 接口或重定义同名类型**；复制所需字段到自定义纯数据 struct。
`glsl_light_default()` 只构造无效初值。灯光着色器自定义参数可用 `glsl_light_parameter_float/int/bool/vec2/vec3/vec4/color(light, "参数名", fallback, valid)`；直接调用、字符串字面量名称、匹配类型 fallback、bool valid，不能据此读取任意对象属性。

自定义漫反射组合为 `light.diffuse_color * light.attenuation * max(dot(N, light.vector), 0.0) * glsl_light_shadow(i, N)`；高光显式计算自己的 BRDF，环境项另取 ambient helper。不要寻找已移除的单字段 light helper 或使用 light_buf/LightData 等内部接口。

## 7. 外部源码转换速查

| 来源 / 写法 | 转换方式 |
| --- | --- |
| 完整 shader | 去掉 main/mainImage、入口语义、#version、precision、layout、引擎 include、工具元数据；保留普通函数 |
| uniform / cbuffer / 全局运行时量 | 改成显式参数；时间、分辨率、鼠标等由外部驱动，不保留共享可变全局状态 |
| Shadertoy fragCoord/iResolution/iTime/iChannel | 改成坐标、分辨率、时间、sampler 参数；说明像素/UV、宽高比和坐标原点；反馈、多 pass、视频/键盘通道需另行处理 |
| HLSL half/fixed/floatN、lerp、saturate、frac | float/vecN、mix、clamp(x,0,1)、fract |
| Texture2D + SamplerState、Sample/SampleLevel | sampler2D + texture/textureLod，明确资源来源；不能保留 DX 对象/寄存器系统 |
| mul(A,B) | 按原行列/坐标约定改为乘法，不盲目调换；复杂非方阵投影可用 dot helper 展开 |
| HLSL 输入 struct | 删除字段语义；纯数据可保留为 GLSL 具名 struct，资源拆成独立 sampler，不必强拆全部成员 |
| ShaderLab | 抽取核心着色逻辑，移除 Properties/SubShader/Pass/pragma/CGPROGRAM 包装与引擎宏 |

类型/维度显式匹配；比较结果参与数值运算时写 `condition ? 1.0 : 0.0`，不依赖 bool 隐式转数值。反向 smoothstep 改成 `1.0 - smoothstep(low, high, x)` 或稳定 helper，处理零分母。
只在原算法需要时保留显式 LOD/导数；删除死代码和无用宏，保留用户已有 Meta、函数结构、默认值与无关效果。
原 Alpha 须说明保留在 vec4.a、独立 out，还是有意省略；颜色到 Surface 不会自动建立透明材质逻辑。

## 8. 完整示例

以下源码可直接放入 Function，函数名可留空用 Auto。普通 vec4 返回值会拆成 XYZ/W；结构体输出经 Separate Bundle + Sync 读取。

### 8.1 最小纹理函数

```glsl
/* @glsl_meta v1
tex: label="主贴图" description="通过 Image to Closure 提供纹理"
uv: label="UV" default=vec2(0.0)
*/
vec4 sample_main_tex(sampler2D tex, vec2 uv)
{
  return texture(tex, uv);
}
```

tex 接 Image to Closure，uv 接纹理坐标；返回值的 W 保留 Alpha。

### 8.2 两个 Bundle 输入，返回值加两个 Bundle out

```glsl
struct SurfaceData {
  vec4 base_color;
  vec2 uv;
};

struct LightingControls {
  vec3 tint;
  float gain;
  mat2 uv_transform;
};

struct LightingResult {
  vec4 color;
  vec2 uv;
};

/* @glsl_meta v2
surface: label="表面"
surface.base_color: label="基础色" subtype=color default=vec4(0.8, 0.4, 0.2, 0.75)
surface.uv: label="UV" default=vec2(0.25, 0.5)
controls: label="控制"
controls.tint: label="染色" subtype=color default=vec3(0.5, 1.0, 0.25)
controls.gain: label="强度" default=0.8 min=0.0 max=2.0
controls.uv_transform: label="UV 矩阵" default=mat2(1.0, 0.25, 0.5, 1.0)
return: label="处理后表面"
return.base_color: subtype=color
original: label="原始表面"
original.base_color: subtype=color
report: label="计算结果"
report.color: subtype=color
*/
SurfaceData shade_bundles(SurfaceData surface,
                         LightingControls controls,
                         out SurfaceData original,
                         out LightingResult report)
{
  original = surface;
  SurfaceData result = surface;
  result.base_color.rgb *= controls.tint * controls.gain;
  result.uv = controls.uv_transform * surface.uv;
  report.color = result.base_color;
  report.uv = result.uv;
  return result;
}
```

两个输入断开也有效，因为成员都有默认值。输出依次为处理后表面、原始表面和计算结果；Alpha 保持 0.75，默认 UV 结果为 `(0.5, 0.5625)`。
矩阵列为 `(1.0,0.25)`、`(0.5,1.0)`；Bundle 字段键使用 `uv_transform.c0/c1`，不是普通矩阵插口的 C1/C2 显示名。

### 8.3 按 Lightgroup ID 输出灯光 Bundle

用于 Eevee 物体表面材质。返回当前列表第一盏匹配灯；需要选择唯一对象时给目标灯独立 ID，无匹配返回 valid=false。

```glsl
struct LightQuery {
  int lightgroup_id;
};

struct LightBundle {
  bool valid;
  int type;
  int lightgroup_id;
  vec3 vector;
  vec3 position;
  vec3 direction;
  float distance;
  vec3 diffuse_color;
  vec3 specular_color;
  float attenuation;
  float influence_radius;
};

/* @glsl_meta v2
query: label="灯光选择"
query.lightgroup_id: label="灯光组 ID" default=1 min=0
return: label="灯光数据"
return.diffuse_color: label="漫反射颜色" subtype=color
return.specular_color: label="高光颜色" subtype=color
*/
LightBundle light_by_group(LightQuery query)
{
  LightBundle result;
  result.valid = false;
  result.type = GLSL_LIGHT_TYPE_INVALID;
  result.lightgroup_id = query.lightgroup_id;
  result.vector = vec3(0.0, 0.0, 1.0);
  result.position = vec3(0.0);
  result.direction = vec3(0.0);
  result.distance = 0.0;
  result.diffuse_color = vec3(0.0);
  result.specular_color = vec3(0.0);
  result.attenuation = 0.0;
  result.influence_radius = 0.0;

  int count = glsl_light_count();
  for (int i = 0; i < count; i++) {
    GLSLLight light = glsl_light_get(i);
    if (!light.valid || light.lightgroup_id != query.lightgroup_id) {
      continue;
    }
    result.valid = true;
    result.type = light.type;
    result.lightgroup_id = light.lightgroup_id;
    result.vector = light.vector;
    result.position = light.position;
    result.direction = light.direction;
    result.distance = light.distance;
    result.diffuse_color = light.diffuse_color;
    result.specular_color = light.specular_color;
    result.attenuation = light.attenuation;
    result.influence_radius = light.influence_radius;
    break;
  }
  return result;
}
```

默认选择组 1。输出 Sync 后接 diffuse_color/specular_color，不要沿用旧包的 color/roughness 连接；Sync 保留旧字段，需要手动改接。
Color 可直连材质 Surface，显示灯光颜色项；不会自动附加衰减、NdotL 或阴影，完整光照须按前节公式计算。

### 8.4 普通参数的矩阵及 int/bool 表达式默认值

```glsl
mat2 default_basis()
{
  return mat2(0.25, 0.5, 0.75, 1.0);
}

/* @glsl_meta v1
basis: label="基矩阵" default=default_basis()
mode: label="模式" default=(1+2)
enabled: label="启用" default=(1.0<2.0)
*/
vec3 defaults_example(mat2 basis, int mode, bool enabled)
{
  float weight = enabled && mode == 3 ? 0.25 : 0.0;
  return vec3(basis[0][1], basis[1][0], weight);
}
```

输入均断开时返回 `(0.5, 0.75, 0.25)`。表达式 default 不写入插口静态数值；任一矩阵列连线会关闭整个矩阵默认表达式。

## 9. 验证与交付

从工作区根目录运行，**显式指定实际交付的 Blender**；验证器不会自动跟随最新 Release/mainfix，默认路径可能指向旧安装。测试使用隔离用户配置，不保存或重置用户工程。

```powershell
$nprBlender = 'release\blender-5.2.2-npr-port-win64-1c82f84cbebf-20261026-092514\blender.exe'
python tools\validate_glsl_function.py 'temp\effect.glsl' --function sample_main_tex --blender $nprBlender --require-blender --json
```

必须得到 `valid=true`、`blender=READY`、`findings=[]` 才算静态与 Blender 接口解析通过。验证器的入口推断比节点 Auto 保守，多 helper 时用 --function 指定，同时核对节点 Auto。
`gpu: not run (interface validation only)` 就是尚未做 GPU 检查；Bundle、默认表达式、Closure、sampler、Eevee helper 或特殊材质域需对应 GPU/render 用例。结构体不能只做静态检查。查看 node.error_message，不能把退出码零、READY、白色 sampler 回退或没有粉色当作渲染正确。

交付前集中检查：

1. 导出边界、Auto 候选、Meta 目标/版本合法；宏与 panel 闭合，无源码/资源残留依赖。
2. 字段名称/类型/维度匹配，W、Alpha、矩阵列序和默认值触发规则正确；共享包/过期字段已处理。
3. 纹理来源、坐标和材质域明确；删除、近似、反馈与透明度差异如实说明。
4. 写明实际解析/GPU 测试和未验证范围，不把部分可用宣称为完全等价。

结果说明保持简短，使用用户语言：

```text
入口：Auto -> 函数名，或显式锁定名称
连接：必要输入 / 输出与 Alpha 处理
差异：无，或具体删除/近似内容
验证：PASS（列出静态、Blender parser、实际 GPU 测试）/ STATIC_ONLY / FAIL
```

语法权威：[Function 源码](../source/blender/nodes/shader/nodes/node_shader_glsl_function.cc)、[灯光 helper](../source/blender/gpu/shaders/material/gpu_shader_material_glsl_light_access.glsl)、[Expression 源码](../source/blender/nodes/shader/nodes/node_shader_script_expression.cc)。
回归入口：`tests/python/bl_node_glsl_function.py`、`tests/python/npr/test_glsl_function_{auto_entry,bundle,bundle_ui}.py`。
