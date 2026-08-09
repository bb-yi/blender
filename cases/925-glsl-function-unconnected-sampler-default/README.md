# GLSL Function 未连接 sampler 默认值

## 测试内容

验证未连接的 `sampler2D` / `sampler3D` Closure 输入不会使整个 GLSL Function 输出黑色，而是在 GPU 专用化阶段按不透明白色常量纹理处理：

- 直接、嵌套和 alias sampler 调用均返回 `vec4(1.0)`。
- 覆盖 `texture`、带 bias 的 `texture`、`textureLod`、`textureGrad`、`textureSize`、`texelFetch` 以及 sampler2D 的 `textureGather`。
- 同一函数内已连接 sampler 保持真实图像颜色，只有未连接 sampler 使用白色。
- 连接、断开和重新连接后，shader 专用化与渲染结果正确切换。
- Node Group 的 Closure 输入未连接时使用白色；材质实例从外部连接真实来源后恢复图像颜色。
- 参数数量非法的采样调用不会因为 fallback 重写而变成合法代码。
- `textureGather(sampler3D, ...)` 不会因为 fallback 重写而变成合法调用。
- 检查生成 shader，确认 fallback 使用 float dummy 与常量 helper，不声明或绑定真实 sampler 资源。

## 通过条件

- OpenGL、Vulkan 子进程都必须确认实际后端并完成测试。
- 白色结果各 RGBA 通道与 `1.0` 的误差不超过 `0.02`。
- 已连接图像和已连接 Group Input 的结果与 `(0.25, 0.50, 0.75)` 的误差不超过 `0.02`；断开后恢复白色，重连后恢复图像颜色。
- 参数数量非法的采样及无效 sampler3D gather 路径输出黑色，并记录可读的专用化失败原因。
- shader dump 中 `fallback_queries` 的两个 sampler 参数和 sampler alias 均为 `float`，2D/3D helper 返回 `vec4(1.0)`，且不存在针对 fallback 参数的残留纹理调用。

## 测试入口

`run.py`

## 原始测试

`blender_npr_post\tests\python\npr\test_glsl_function_unconnected_sampler_default.py`
