# GLSL Function Closure sampler3D

## 测试内容

验证 `Closure Input.UV (Vector) -> SDF Primitive -> Closure Output.Color -> GLSL Function sampler3D` 的完整 GPU 路径：

- GLSL Function 暴露 `Coordinate` 向量输入，并直接执行 `texture(sdf_volume, coordinate)`；测试端分别设置 Z=`0.0/0.5/1.0`，确认输入 Z 分量真实进入程序化闭包。
- 以 128 步 raymarch 渲染三维球体，并用三维有限差分输出法线。
- 编译并渲染两个参数化 SDF 球体的 Smooth Union。
- 在同一 GLSL Function 中同时执行 vec2 sampler2D helper 和 vec3 sampler3D helper，确认坐标类型互不串线。
- 分别给 sampler2D 和 sampler3D Closure 提供已连接和未连接数值型 Float Alpha，确认 `texture(...).a = Color.a * Alpha` 且 RGB 保持不变；随后验证 Alpha 默认值 `1.0` 保留 `Color.a`，完全没有 Alpha socket 时 RGBA 同样保留 `Color.a`、Vector Color 回退为 `1.0`。
- 在 shader 预热后直接修改 Closure Output 的 Color 和 Alpha 数值，确认像素立即更新且材质 `shader_compile_timestamp` 不变，证明数值通过 uniform 更新而不是重新编译 shader。
- 继续渲染 `Image to Closure` 的 `3D LUT Strip`，确认真实 GPU 3D 纹理路径没有回归。
- 将没有 GPU helper 实现的 IES Texture 接入 Closure Output，确认失败日志可读且不会产生假采样结果。

## 通过条件

- 对称的低/高 Z 探针接近，中间 Z 与端点至少相差 `0.15`。
- 球体中心命中、角落未命中；左右 X 法线和上下 Y 法线通道差异均大于 `0.18`。
- Smooth Union 中心输出为有限灰度值，三个颜色通道差异小于 `0.02`。
- 混合 helper 中心颜色与 `(0.75, 0.20, 0.20)` 的各通道误差小于 `0.05`。
- 已连接和未连接数值型 Closure Alpha 探针中心颜色均与 `(0.28, 0.585, 0.80)` 的各通道误差小于 `0.05`，证明独立 Alpha 与 `Color.a` 相乘；Alpha 默认 `1.0` 的探针与 `(0.80, 0.90, 0.80)` 的误差小于 `0.05`；无 Alpha socket 的兼容回退探针与 `(0.80, 1.00, 0.80)` 的误差小于 `0.05`。
- 直接数值更新前后像素分别接近 `(0.28, 0.585, 0.80)` 和 `(0.30, 0.10, 0.85)`，同时更新后的 `shader_compile_timestamp` 必须与预热后完全相同。
- 3D LUT Strip 中心颜色与 `(0.2, 0.6, 0.9)` 的各通道误差小于 `0.05`。
- IES helper 路径必须记录 `could not compile for sampler3D parameter`，并回落为黑色输出。

## 测试入口

`run.py`

## 原始测试

`blender_npr_post\tests\python\npr\test_glsl_function_closure_sampler3d.py`
