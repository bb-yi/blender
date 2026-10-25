# GLSL Function：结构体与捆包

具名 GLSL `struct` 可以作为 GLSL Function 的输入、返回值和 `out` 参数。
每个结构体对应一个 Bundle 插口，嵌套结构体对应嵌套捆包。

## 一个完整例子

选择导出函数 `tint_surface`。其 `surface` 输入和 `Result` 输出都是捆包。
输出连接 Separate Bundle 后点击 Sync，可以取出 `base_color` 等成员。

```glsl
struct SurfaceData {
    vec4 base_color;
    vec3 normal;
    float roughness;
};

/* @glsl_meta v2
surface: label="表面数据" description="颜色、法线和粗糙度"
surface.base_color: subtype=color default=vec4(1.0) description="保留 Alpha"
surface.normal: default=glsl_normal() description="物体材质中的世界空间法线"
surface.roughness: default=0.5 min=0.0 max=1.0
gain: label="强度" default=0.75 min=0.0 max=2.0
return: label="结果"
return.base_color: subtype=color
*/
SurfaceData tint_surface(SurfaceData surface, float gain)
{
    surface.base_color.rgb *= gain;
    return surface;
}
```

输入捆包完全断开时，上例的所有成员都有默认值，因此仍能求值。
`glsl_normal()` 等助手的含义取决于材质域；按具体 Object、World、Filter 或 NPR 用法验证。

## 类型和字段匹配

| GLSL 类型 | 捆包成员 |
|---|---|
| `float`、`int`、`bool` | Float、Integer、Boolean |
| `vec2`、`vec3`、`vec4` | 2D、3D、4D Vector |
| `vec3/vec4` 配置 `subtype=color` | Color；RGB 或完整 RGBA |
| `mat2/mat3/mat4` | 子捆包 `c0…cN-1`，每项是对应维度的列向量 |
| 其他具名纯数据结构体 | 子捆包 |

字段按名称匹配，大小写敏感，排列顺序不影响取值。显示标签可以使用中文，字段键保持 GLSL 标识符。
Color 可向 `vec3` 提供 RGB、向 `vec4` 提供 RGBA；数值向量的维度必须匹配。
矩阵使用 GLSL 列顺序，不转置。例如 `mat4` 的 `c3.w` 对应 `matrix[3][3]`。

额外输入字段允许保留，但返回捆包只含返回结构体声明的字段，原输入包不会被修改。
需要携带其他字段时，将结果与原包显式 Join；重复键沿用 Join Bundle 的现有优先顺序。

普通 GLSL Function 参数仍使用原有 vec4 的 XYZ/W 插口及矩阵按列插口布局。
Shader Bundle 的向量维度会保存到工程中；几何节点的 Bundle 向量保持原有 3D 行为。

## 默认值和 Meta v2

v1 及无版本 Meta 的旧语法保持兼容。v2 允许 `参数.成员.子成员`，返回值使用保留目标 `return`。
`out` 使用参数自身名称。Meta 块仍位于导出函数正上方。

- 根捆包和嵌套结构体逐成员声明默认值，不接受整个结构体的 `default=`。
- 缺失的数值、向量或矩阵成员使用显式默认值；没有默认值的成员报错。
- 完全缺失的子捆包只有在所有成员都能缺省时才有效。
- 矩阵默认值作用于整个矩阵。提供矩阵子捆包时必须提供全部列，不允许用默认矩阵补齐部分列。
- 字段已经存在、但字段插口未连接时，使用字段自身的数值，包括零；常量折叠不会改变这一规则。
- 连接无效或类型错误时报告错误，不触发缺省。
- 默认表达式在需要时求值，可使用常量、同一源码中的 helper、宏和现有内建助手。
  不能引用当前函数参数或其他默认字段，避免隐式求值顺序和循环依赖。

成员 Meta 支持 `label/description/default`，数值成员另支持类型适用的 `min/max/subtype`。
矩阵只支持 `label/description/default`；输出成员只支持 `label/description/subtype`。
面板用于整个输入参数。成员上的 `items/show_label/hide_value` 明确报错；这些控件设置继续用于普通参数或来源节点。

## 创建、覆盖和同步

结构体仅增加 Bundle 插口，不增加额外成员面板或创建按钮。
添加一个普通 Combine Bundle，连接结构体输入后，点击其标题栏的同步图标或侧栏中的 Sync，
即可创建缺少的成员及必要的嵌套捆包，整个操作可一次撤销。Separate Bundle 沿用相同的同步入口。

空包刚连接时，只自动同步必需成员；显式点击 Sync 还会添加具有一致常量默认值的可选成员，
并使用该常量初始化数值。表达式默认值、多个消费者之间不同的默认值，以及超出跨节点整数精确范围的默认值保持省略，
在各自函数中继续求值。
全部成员都有默认值时，空捆包本身也是有效输入。

成员查看和编辑沿用 Combine/Separate Bundle。要覆盖某个默认值，在 Combine Bundle 中添加同名、同类型的字段，
再设置数值或连接上游节点。显式字段会替代函数侧默认值，包括默认表达式；共享包中的字段会影响所有消费者。

Sync 添加缺少的必需字段，保留已有标识符、值、连线及额外字段。
它不会猜测重命名，也不会删除已连接的过期字段。类型冲突时不会执行部分同步，错误会指出字段和消费者。
嵌套 Separate Bundle 可以继续 Sync；手工建立的捆包同样可以连接 GLSL Function。

## 错误与兼容边界

解析失败保留最后一次有效接口。材质编译会报告缺失成员、错误维度、部分矩阵等问题。
`ShaderNodeGLSLFunction.error_message` 提供当前源码和编译版本对应的诊断；过期编译结果不会覆盖新状态。
默认表达式的 GPU 错误会标明对应字段。World 等域保持引擎原有的失败显示方式，不能仅凭回退颜色判断成功。

- 首版结构体成员限于上述纯数据类型；不支持数组、sampler、Closure、递归结构体及导出接口 `inout`。
- 原有独立 sampler 输入和 typed Closure callback 可以与结构体参数混用，其原有签名规则继续适用。
- 最多 32 层结构体；展开后每侧最多 512 个插口，普通参数和 callback 也计入。
- `int` 跨节点沿用 float 传输，只有 `[-16777216, 16777216]` 保证整数精确；不承诺完整 32 位整数精度。
- 新版本可读取旧工程；含新结构体接口的工程需要支持该功能的版本。
- 验证器的 `READY` 代表接口解析通过；实际材质仍需 OpenGL/Vulkan 渲染验证。

源码回归：`tests/python/npr/test_glsl_function_bundle.py`、编辑器测试及性能对照测试。
Release case：`glsl-function-bundle`，分别验证 OpenGL 和 Vulkan。
