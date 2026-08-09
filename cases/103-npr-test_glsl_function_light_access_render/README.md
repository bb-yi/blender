# npr-test_glsl_function_light_access_render

## 测试内容

验证 `GLSL Function` 中公开的灯光访问 API 能在 Eevee 渲染里读取、累计和分类真实灯光数据。

覆盖内容：

- `DITHERED` 和 `BLENDED` 材质路径下，单灯光照应明显高于无灯场景。
- 多灯场景能同时得到红灯和绿灯贡献，且不会产生爆亮。
- 自定义 specular 累计在有灯时可见、无灯时接近黑。
- shadow mask 能区分被遮挡和未遮挡采样点。
- 读取 light count、position、direction 和 type 字段，对 POINT/SPOT/AREA/SUN 给出预期结果。

## 通过条件

- 有灯样本亮度大于无灯样本至少 `0.05`，无灯样本接近黑。
- 多灯中心像素红、绿通道都大于 `0.05`，蓝通道小于 `0.03`。
- 阴影样本比 lit/unblocked 参考至少暗 `0.15`。
- light count 颜色接近 `(0.25, 0.25, 0.25)`。
- 各灯类型的 position/direction/type 读数在脚本定义的容差内。

## 测试入口

`run.py`

## 原始测试

`blender_5_1_port\tests\python\npr\test_glsl_function_light_access_render.py`
