# npr-test_filter_glsl_function_render

## 测试内容

验证 Eevee Filter material 中的 `GLSL Function` 能参与实际渲染。

具体检查两个场景：

- 用 `Scene Color` 读取蓝色发光平面，再由 GLSL Function 反相，中心像素应变成黄色。
- 用 `Image to Closure` 提供 1x1 纹理给 `sampler2D` 参数，Filter 输出应采到纹理颜色 `(1.0, 0.25, 0.0)`。

`Image to Closure` 的可查看测试工程位于 `assets/020-image-to-closure-sampler2d/scene.blend`。

## 通过条件

- 反相场景中心像素红、绿通道都大于 `0.9`，蓝通道小于 `0.1`。
- sampler2D 场景中心像素红通道大于 `0.9`，绿通道在 `0.2..0.3`，蓝通道小于 `0.1`。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_filter_glsl_function_render.py`
